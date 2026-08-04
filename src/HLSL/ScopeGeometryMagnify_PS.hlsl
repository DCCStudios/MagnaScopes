#include "Triangle.hlsli"
#include "ScopeShadow.hlsli"

struct ScopeGeometryPixel
{
    float4 position : SV_Position;
    noperspective float3 lensProjective : TEXCOORD0;
};

float2 SolvePixelOffset(
    float2 columnX,
    float2 columnY,
    float2 rightHandSide,
    out bool valid)
{
    const float determinant =
        columnX.x * columnY.y - columnY.x * columnX.y;
    valid = abs(determinant) > 0.0000001f;
    const float safeDeterminant = valid ? determinant : 1.0f;
    return float2(
        rightHandSide.x * columnY.y - columnY.x * rightHandSide.y,
        columnX.x * rightHandSide.y - rightHandSide.x * columnX.y) /
        safeDeterminant;
}

float4 main(ScopeGeometryPixel input) : SV_Target0
{
    const float2 screenUv = input.position.xy * PixelSize;

    const float activation = saturate(SCOPE_FADE_ACTIVATION);
    // Grow the optical power continuously during ADS instead of switching a
    // fully magnified image on after the scope animation. ScopeFade remains
    // the physical boundary for every intermediate value.
    const float magnification = lerp(
        1.0f,
        clamp(SCOPE_FADE_MAGNIFICATION, 1.0f, 15.0f),
        activation);
    // Lens distortion is centered on the physical ScopeFade aperture while
    // magnification remains centered on the authored reticle. Zero strength
    // is exactly the established Stage 4e.2 mapping.
    const float2 publishedLensBasisX =
        float2(SCOPE_LENS_BASIS_XX, SCOPE_LENS_BASIS_XY);
    const float2 publishedLensBasisZ =
        float2(SCOPE_LENS_BASIS_ZX, SCOPE_LENS_BASIS_ZY);
    // Eye travel is already published in display X/Y. It must not disappear
    // merely because the old CPU-projected ScopeFade basis becomes
    // foreshortened at a particular camera pitch or world heading.
    const bool physicalEyeTravelValid =
        SCOPE_PHYSICAL_EYEBOX_VALID > 0.0001f;
    // Axial eye relief is a camera-local depth delta, not another screen-space
    // translation. Reuse the existing virtual scene/shadow separation controls
    // as independent gains: scene depth controls subtle optical breathing,
    // while shadow depth controls exit-pupil breathing. The authored
    // ScopeFade vertices and aperture center are never changed.
    const float physicalEyeBoxBlend =
        saturate(SCOPE_PHYSICAL_EYEBOX_VALID);
    const float axialEyeRelief =
        physicalEyeTravelValid ?
            clamp(SCOPE_EYE_RELIEF_DELTA, -0.25f, 0.25f) *
                physicalEyeBoxBlend * activation :
            0.0f;
    const float sceneDepth = clamp(ScopeSceneDepth, 0.0f, 4.0f);
    const float shadowDepth = clamp(ScopeShadowDepth, 0.0f, 4.0f);
    // Fore/aft breathing is its own control. It used to be driven by
    // ScopeSceneDepth, which also scales lateral parallax, so raising lateral
    // parallax unavoidably made the image appear to move closer and farther.
    // Worse, the underlying eye-relief signal was measured along the aperture's
    // own local normal, and that frame rotates with the weapon, so lateral
    // weapon swing during a yaw leaked into the axial term: panning left and
    // right read as depth. Default zero keeps apparent size fixed.
    const float axialBreathing = clamp(ScopeAxialBreathing, 0.0f, 4.0f);
    // Positive relief means the eyepiece moved farther from the camera. Its
    // apparent optical image and exit pupil therefore contract; moving closer
    // expands them. Clamp both responses so a malformed pose cannot flash a
    // full-screen sample or close the pupil into a black disk.
    const float axialSceneScale = clamp(
        1.0f - axialEyeRelief * axialBreathing,
        0.92f,
        1.08f);
    const float opticalMagnification = max(
        1.0f,
        magnification * axialSceneScale);

    const float2 authoredAimOffset =
        SCOPE_AIM_OFFSET_VALID > 0.5f ?
            float2(SCOPE_AIM_OFFSET_X, SCOPE_AIM_OFFSET_Y) :
            float2(0.0f, 0.0f);
    const float2 publishedAimPixels =
        float2(SCOPE_AIM_CENTER_X, SCOPE_AIM_CENTER_Y);
    // Where the sight picture sits inside the housing. STS publishes where its
    // ScopeFade mesh is, which is not always where a given scope model wants
    // the optical image, so this is authored per scope in aperture radii along
    // the optic's own axes.
    const float2 lensUserOffset =
        float2(SCOPE_LENS_OFFSET_X, SCOPE_LENS_OFFSET_Y);
    const float reciprocalClipW = input.lensProjective.z;
    const bool reciprocalClipWValid = abs(reciprocalClipW) > 0.000001f;
    const float safeReciprocalClipW =
        reciprocalClipWValid ? reciprocalClipW : 1.0f;
    const float2 normalizedLensPosition =
        input.lensProjective.xy / safeReciprocalClipW;

    // Reconstruct the current ScopeFade projective frame from the exact
    // replayed draw. The homogeneous XY numerator and reciprocal-W are affine
    // in display space because the geometry shader marks them noperspective.
    // Solving numerator == 0 therefore gives one projectively correct center
    // across the lens. The previous derivative of normalizedLensPosition was
    // only a local linearization of a rational mapping, so its inferred center
    // changed per pixel and exposed triangle facets under magnification.
    const float2 projectiveNumerator = input.lensProjective.xy;
    const float2 numeratorDx = ddx(projectiveNumerator);
    const float2 numeratorDy = ddy(projectiveNumerator);
    const float reciprocalWDx = ddx(reciprocalClipW);
    const float reciprocalWDy = ddy(reciprocalClipW);
    bool centerSolveValid = false;
    const float2 pixelsToCenter = SolvePixelOffset(
        numeratorDx,
        numeratorDy,
        -projectiveNumerator,
        centerSolveValid);

    // The authored aim coordinate is another point on the same projective
    // plane. Solve numerator(p) == aim * reciprocalW(p) instead of applying
    // an affine basis to the local offset. This keeps an off-center reticle
    // pivot fixed under roll, foreshortening, and perspective W gradients.
    bool aimSolveValid = false;
    const float2 aimColumnX =
        numeratorDx - authoredAimOffset * reciprocalWDx;
    const float2 aimColumnY =
        numeratorDy - authoredAimOffset * reciprocalWDy;
    const float2 aimRightHandSide =
        authoredAimOffset * reciprocalClipW - projectiveNumerator;
    const float2 pixelsToAim = SolvePixelOffset(
        aimColumnX,
        aimColumnY,
        aimRightHandSide,
        aimSolveValid);

    // Lens Center is a third point on that same projective plane, so it is
    // solved the same exact way rather than pushed through the published
    // aperture basis. That basis is not a similarity transform -- see the mask
    // comment below -- so using it here would have moved the magnified content
    // by a different amount than it moved the mask.
    bool lensOffsetSolveValid = false;
    const float2 pixelsToLensOffset = SolvePixelOffset(
        numeratorDx - lensUserOffset * reciprocalWDx,
        numeratorDy - lensUserOffset * reciprocalWDy,
        lensUserOffset * reciprocalClipW - projectiveNumerator,
        lensOffsetSolveValid);

    const bool drawFrameValid =
        reciprocalClipWValid && centerSolveValid && aimSolveValid;
    const float2 centerPixels = input.position.xy + pixelsToCenter;
    const float2 currentAimPixels = input.position.xy + pixelsToAim;

    // Recover the two projected unit-radius endpoints through the same exact
    // homogeneous solve. Their mean distance from the center is the local
    // display-space radius used to convert normalized eye travel to pixels.
    bool unitXSolveValid = false;
    bool unitZSolveValid = false;
    const float2 unitXCoordinates = float2(1.0f, 0.0f);
    const float2 unitZCoordinates = float2(0.0f, 1.0f);
    const float2 pixelsToUnitX = SolvePixelOffset(
        numeratorDx - unitXCoordinates * reciprocalWDx,
        numeratorDy - unitXCoordinates * reciprocalWDy,
        unitXCoordinates * reciprocalClipW - projectiveNumerator,
        unitXSolveValid);
    const float2 pixelsToUnitZ = SolvePixelOffset(
        numeratorDx - unitZCoordinates * reciprocalWDx,
        numeratorDy - unitZCoordinates * reciprocalWDy,
        unitZCoordinates * reciprocalClipW - projectiveNumerator,
        unitZSolveValid);
    const float2 unitXPixel = input.position.xy + pixelsToUnitX;
    const float2 unitZPixel = input.position.xy + pixelsToUnitZ;
    const bool exactDrawFrameValid =
        drawFrameValid && unitXSolveValid && unitZSolveValid;
    if (!exactDrawFrameValid) {
        // Exact ScopeFade replay must never fall back to a CPU projection from
        // another frame. A malformed/degenerate wedge therefore fails open to
        // the unmodified source pixel. This avoids the old off-screen pivot,
        // black disk, or heading-dependent flash while preserving the authored
        // scope geometry already rendered by Fallout.
        return float4(
            tBACKBUFFER.SampleLevel(gSamLinear, screenUv, 0.0f).rgb,
            1.0f);
    }
    const float currentProjectedRadius = max(
        0.5f *
            (length(unitXPixel - centerPixels) +
             length(unitZPixel - centerPixels)),
        1.0f);
    const float2 currentCenterUv = centerPixels * PixelSize;

    // Optical tube depth. The authored aperture is the near end of a tube and
    // the magnified image sits far behind it, so when the housing sways the
    // image should not travel with it one-for-one. The game thread publishes
    // how far the aperture has moved from its settled screen position, in
    // aperture radii; declining to follow that fraction of the motion is what
    // reads as depth.
    //
    // This has to scale the sample pivot, not the sample delta. A pivot shift
    // of d changes the sampled point by d * (1 - 1/M), while a delta shift
    // changes it by d / M. Only the pivot form cancels the aperture's own
    // motion identically at every magnification, which is why the existing
    // scene-parallax control could never actually hold the image still.
    const float imageStillness = saturate(ScopeImageStillness);
    float2 aperturePivotPixels = currentAimPixels;
    // Lens Center moves the whole optical assembly, not just its mask. The
    // pivot is the fixed point of the magnification, so placing it at the
    // authored lens centre is what actually re-centres the sight picture --
    // moving the mask alone would only crop an image still zoomed about the
    // old point. Both solves share a base pixel, so their difference is the
    // exact screen displacement from lens (0,0) to lens lensUserOffset.
    if (lensOffsetSolveValid) {
        aperturePivotPixels += pixelsToLensOffset - pixelsToCenter;
    }
    if (physicalEyeTravelValid && imageStillness > 0.0f) {
        const float2 apertureMotionPixels =
            float2(SCOPE_EYE_OFFSET_X, SCOPE_EYE_OFFSET_Y) *
            saturate(SCOPE_PHYSICAL_EYEBOX_VALID) *
            currentProjectedRadius;
        // Published travel is the eye's displacement, which is the negation of
        // the optic's screen motion. Adding it therefore removes the aperture's
        // excursion from the pivot and leaves the image where it settled.
        aperturePivotPixels += apertureMotionPixels * imageStillness;
    }
    const float2 samplePivotUv = aperturePivotPixels * PixelSize;

    // Apparent-size stillness. Fore/aft motion barely moves the aperture's
    // centre but swings its projected radius by well over ten percent, so the
    // housing visibly looms while the image behind it stayed locked. If the
    // aperture grew by ratio r, dividing the sampled extent by r holds the
    // image world-static: extent = R / (M * r) = R_settled / M, independent of
    // how large the housing has become.
    const float apertureScaleRatio =
        ScopeApertureScaleRatio > 0.01f ?
            clamp(ScopeApertureScaleRatio, 0.25f, 4.0f) :
            1.0f;
    const float sampleMagnification =
        opticalMagnification *
        lerp(1.0f, apertureScaleRatio, imageStillness);
    float2 sampleDelta =
        (screenUv - samplePivotUv) / max(sampleMagnification, 0.0001f);
    const float radialPosition =
        saturate(length(normalizedLensPosition));
    const float fishEyeAmount =
        clamp(SCOPE_FISHEYE_STRENGTH, 0.0f, 2.0f) *
        activation *
        pow(
            radialPosition,
            clamp(SCOPE_FISHEYE_POWER, 0.5f, 6.0f));
    sampleDelta +=
        (screenUv - currentCenterUv) *
        fishEyeAmount /
        opticalMagnification;

    // Refraction is confined to the rim. It displaces the scene along the
    // physical lens radius without moving the optical center. The profile
    // strength is a fraction of the local lens radius, so the default remains
    // comparable across differently sized STS apertures.
    const float refractionWidth =
        clamp(SCOPE_EDGE_REFRACTION_WIDTH, 0.02f, 0.5f);
    const float edgeWeight = smoothstep(
        1.0f - refractionWidth,
        1.0f,
        radialPosition);
    const float refractionStrength =
        clamp(SCOPE_EDGE_REFRACTION_STRENGTH, 0.0f, 0.25f) *
        activation;
    const float2 radialUv = screenUv - currentCenterUv;
    sampleDelta +=
        radialUv *
        (refractionStrength * edgeWeight) /
        opticalMagnification;

    // A real optic's scene shifts slightly beneath the fixed housing as the
    // eye leaves the optical axis. The game thread publishes transient travel
    // in render-target X/Y, normalized by the projected aperture radius. Do
    // not rotate this value through ScopeFade's world-facing X/Z basis: doing
    // so made identical camera motion reverse with heading and pitch.
    float2 physicalEyeTravel = float2(0.0f, 0.0f);
    if (physicalEyeTravelValid) {
        physicalEyeTravel =
            float2(SCOPE_EYE_OFFSET_X, SCOPE_EYE_OFFSET_Y) *
            saturate(SCOPE_PHYSICAL_EYEBOX_VALID) *
            clamp(SCOPE_OPTICAL_LAG_STRENGTH, 0.0f, 4.0f);
        // The game-thread EMA already removes pose noise. Do not introduce a
        // second hard dead zone here: zeroing a nearly centered value makes
        // the optical image snap after it has smoothly approached center.
        const float travelLength = length(physicalEyeTravel);
        const float maximumTravel =
            clamp(SCOPE_EYEBOX_MAX_TRAVEL, 0.0f, 4.0f);
        if (travelLength > maximumTravel && travelLength > 0.00001f) {
            physicalEyeTravel *= maximumTravel / travelLength;
        }
        // The shadow may travel the full configured distance. Scene parallax
        // uses a smooth asymptotic limiter rather than the previous hard
        // one-radius clamp: small inertia remains responsive, while large
        // recoil can never displace the sample by an unbounded amount or
        // create a visible derivative discontinuity.
        const float2 depthTravel = physicalEyeTravel * sceneDepth;
        // One aperture radius is the largest scene shift that still keeps a
        // recognizable image behind the aperture. The shared soft limiter
        // reaches that bound asymptotically while leaving ordinary small
        // inertia essentially linear, which the previous 1/(1+2m) form did
        // not: it removed roughly 29% of a 0.2-radius shift and flattened the
        // very motion that sells optical depth.
        const float2 sceneTravel =
            ScopeShadowSoftLimitVector(depthTravel, 1.0f);
        const float2 eyeParallaxPixels =
            sceneTravel * currentProjectedRadius;
        // The scene counter-shifts beneath the fixed housing. Using the same
        // sign as measured eye motion made the image lead and overshoot the
        // weapon; subtracting it produces the expected optical lag.
        sampleDelta -=
            eyeParallaxPixels *
            PixelSize *
            (clamp(SCOPE_SCENE_PARALLAX_STRENGTH, 0.0f, 2.0f) /
             opticalMagnification);
    }

    // Breathing sway. The shooter's whole hold drifts, so the scene swims
    // beneath a housing and reticle that stay put -- which means translating
    // the sampled region, not the magnification pivot. Moving the pivot would
    // drag the zoom's fixed point around and take the reticle's alignment with
    // it.
    //
    // Dividing by magnification makes the amplitude apparent rather than
    // angular, so one authored value reads the same through a 4x and a 12x.
    // Physically the sway would grow with magnification, but that makes the
    // control unusable at the top of the range for no gain in feel.
    const float2 breathingLens = ScopeBreathingOffset();
    sampleDelta -=
        breathingLens *
        currentProjectedRadius *
        PixelSize /
        opticalMagnification;

    const float2 sampleUv = saturate(samplePivotUv + sampleDelta);
    const float2 sourcePixel = PixelSize;
    const float2 radialPixels = radialUv / max(sourcePixel, 0.000001f);
    const float radialPixelLength = length(radialPixels);
    const float2 radialPixelDirection =
        radialPixelLength > 0.0001f ?
            radialPixels / radialPixelLength :
            float2(0.0f, 0.0f);
    const float2 chromaticOffset =
        radialPixelDirection *
        sourcePixel *
        (clamp(SCOPE_EDGE_CHROMATIC_ABERRATION, 0.0f, 2.0f) *
         edgeWeight);

    // Keep the green channel at the geometric sample and move red/blue by
    // equal, opposite sub-pixel amounts. At zero aberration this is exactly
    // one ordinary texture sample.
    const float3 baseCenterSample =
        tBACKBUFFER.SampleLevel(gSamLinear, sampleUv, 0.0f).rgb;
    float3 centerSample = baseCenterSample;
    if (dot(chromaticOffset, chromaticOffset) > 0.00000001f) {
        centerSample.r =
            tBACKBUFFER.SampleLevel(
                gSamLinear,
                saturate(sampleUv + chromaticOffset),
                0.0f).r;
        centerSample.b =
            tBACKBUFFER.SampleLevel(
                gSamLinear,
                saturate(sampleUv - chromaticOffset),
                0.0f).b;
    }
    const float3 sampleLeft =
        tBACKBUFFER.SampleLevel(
            gSamLinear,
            saturate(sampleUv - float2(sourcePixel.x, 0.0f)),
            0.0f).rgb;
    const float3 sampleRight =
        tBACKBUFFER.SampleLevel(
            gSamLinear,
            saturate(sampleUv + float2(sourcePixel.x, 0.0f)),
            0.0f).rgb;
    const float3 sampleUp =
        tBACKBUFFER.SampleLevel(
            gSamLinear,
            saturate(sampleUv - float2(0.0f, sourcePixel.y)),
            0.0f).rgb;
    const float3 sampleDown =
        tBACKBUFFER.SampleLevel(
            gSamLinear,
            saturate(sampleUv + float2(0.0f, sourcePixel.y)),
            0.0f).rgb;
    const float3 sampleUpLeft =
        tBACKBUFFER.SampleLevel(
            gSamLinear,
            saturate(sampleUv - sourcePixel),
            0.0f).rgb;
    const float3 sampleUpRight =
        tBACKBUFFER.SampleLevel(
            gSamLinear,
            saturate(sampleUv + float2(sourcePixel.x, -sourcePixel.y)),
            0.0f).rgb;
    const float3 sampleDownLeft =
        tBACKBUFFER.SampleLevel(
            gSamLinear,
            saturate(sampleUv + float2(-sourcePixel.x, sourcePixel.y)),
            0.0f).rgb;
    const float3 sampleDownRight =
        tBACKBUFFER.SampleLevel(
            gSamLinear,
            saturate(sampleUv + sourcePixel),
            0.0f).rgb;

    const float3 differenceLeft = sampleLeft - centerSample;
    const float3 differenceRight = sampleRight - centerSample;
    const float3 differenceUp = sampleUp - centerSample;
    const float3 differenceDown = sampleDown - centerSample;
    const float3 differenceUpLeft = sampleUpLeft - centerSample;
    const float3 differenceUpRight = sampleUpRight - centerSample;
    const float3 differenceDownLeft = sampleDownLeft - centerSample;
    const float3 differenceDownRight = sampleDownRight - centerSample;
    const float weightLeft =
        exp2(-32.0f * dot(differenceLeft, differenceLeft));
    const float weightRight =
        exp2(-32.0f * dot(differenceRight, differenceRight));
    const float weightUp =
        exp2(-32.0f * dot(differenceUp, differenceUp));
    const float weightDown =
        exp2(-32.0f * dot(differenceDown, differenceDown));
    const float weightUpLeft =
        exp2(-32.0f * dot(differenceUpLeft, differenceUpLeft));
    const float weightUpRight =
        exp2(-32.0f * dot(differenceUpRight, differenceUpRight));
    const float weightDownLeft =
        exp2(-32.0f * dot(differenceDownLeft, differenceDownLeft));
    const float weightDownRight =
        exp2(-32.0f * dot(differenceDownRight, differenceDownRight));
    const float totalWeight =
        1.0f +
        weightLeft +
        weightRight +
        weightUp +
        weightDown +
        weightUpLeft +
        weightUpRight +
        weightDownLeft +
        weightDownRight;
    const float3 edgeAwareAverage =
        (centerSample +
         sampleLeft * weightLeft +
         sampleRight * weightRight +
         sampleUp * weightUp +
         sampleDown * weightDown +
         sampleUpLeft * weightUpLeft +
         sampleUpRight * weightUpRight +
         sampleDownLeft * weightDownLeft +
         sampleDownRight * weightDownRight) /
        totalWeight;

    const float smoothing = saturate(SCOPE_IMAGE_DENOISE);
    const float3 smoothed =
        lerp(centerSample, edgeAwareAverage, smoothing);
    const float3 sharpened =
        smoothed +
        (smoothed - edgeAwareAverage) *
            (2.0f * saturate(SCOPE_IMAGE_SHARPEN));
    const float3 localMinimum =
        min(
            centerSample,
            min(
                min(sampleLeft, sampleRight),
                min(
                    min(sampleUp, sampleDown),
                    min(
                        min(sampleUpLeft, sampleUpRight),
                        min(sampleDownLeft, sampleDownRight)))));
    const float3 localMaximum =
        max(
            centerSample,
            max(
                max(sampleLeft, sampleRight),
                max(
                    max(sampleUp, sampleDown),
                    max(
                        max(sampleUpLeft, sampleUpRight),
                        max(sampleDownLeft, sampleDownRight)))));
    float3 opticalColor =
        clamp(sharpened, localMinimum, localMaximum);

    // The physical ScopeFade aperture, its resting rim vignette, and the
    // moving exit pupil are three separate optical layers. Neither mask moves
    // or scales the authored ScopeFade boundary. Both are evaluated by the
    // shared contract in ScopeShadow.hlsli so the late reticle composite dims
    // by exactly the same amount at the same optic-local coordinate.
    //
    // The mask is evaluated in the coordinate the geometry shader publishes on
    // the ScopeFade vertices themselves. Do not substitute a frame rebuilt from
    // the game thread's published centre and basis.
    //
    // That basis is the optic's world X/Z axes projected to screen, and its two
    // column lengths do not stay equal: they foreshorten as the optic turns
    // relative to the camera, and by far more than the drawn glass does.
    // Inverting it as a matrix therefore hands the mask an anisotropic frame,
    // and the lit disc collapses into a slit whose narrow axis tracks whichever
    // way the camera is panning. The lens coordinate here is circular by
    // construction, exact under perspective, and continuous across every wedge
    // and the fabricated centre fan alike, which is everything the mask needs.
    //
    // The faceting that motivated abandoning it was never this coordinate. It
    // came from the *offsets* subtracted from it: a validity predicate built on
    // the per-pixel solve, which made adjacent wedges select different frames
    // outright, and a screen-centre axis term divided by the solved radius,
    // which gave each wedge its own disc. Both are gone. Anything added to this
    // coordinate must be uniform across the draw or vary smoothly with it.
    const float2 stableShadowCoordinates =
        normalizedLensPosition - lensUserOffset;
    // Display-X/Y travel becomes lens-local travel through the same exact
    // projective solve the coordinate above comes from, so the two are in one
    // frame by construction and no basis, radius, or convention has to be
    // guessed at to relate them.
    float2 eyeTravelLens = float2(0.0f, 0.0f);
    if (physicalEyeTravelValid) {
        const float2 eyeTravelPixels =
            physicalEyeTravel * currentProjectedRadius;
        const float reciprocalWAtCenter =
            reciprocalClipW +
            reciprocalWDx * pixelsToCenter.x +
            reciprocalWDy * pixelsToCenter.y;
        const float2 numeratorAtEye =
            numeratorDx * eyeTravelPixels.x +
            numeratorDy * eyeTravelPixels.y;
        const float reciprocalWAtEye =
            reciprocalWAtCenter +
            reciprocalWDx * eyeTravelPixels.x +
            reciprocalWDy * eyeTravelPixels.y;
        eyeTravelLens =
            abs(reciprocalWAtEye) > 0.000001f ?
                numeratorAtEye / reciprocalWAtEye :
                float2(0.0f, 0.0f);
    }
    // Positive relief moved the eyepiece farther away, so the lit disc
    // contracts slightly. The bound keeps ordinary breathing subtle and stops
    // a malformed pose from closing the pupil into a black spot.
    const float axialPupilScale = clamp(
        1.0f - axialEyeRelief * axialBreathing,
        0.96f,
        1.04f);
    // The image sits forward in the tube, so it subtends less than the rear
    // glass and a ring of tube wall shows around it. That ring is the depth
    // cue. The mask no longer reads triangle-local data, so this contour is
    // smooth at any radius -- previously a 0.45 floor put it inside the
    // fabricated centre fan, where it picked up the fan's coordinate seam and
    // came out visibly faceted. The floor is 0.6 rather than 0.45 so a fully
    // recessed image still reads as a sight picture and not a porthole.
    //
    // Lens Size then scales the result, which is the direct control over how
    // much of the scope's glass the sight picture fills. Above 1 the disc
    // exceeds the aperture and the drawn geometry crops it, so no tube ring is
    // visible at all -- that is the intended way to disable the ring.
    const float imageDiscRadius =
        axialPupilScale *
        lerp(1.0f, 0.6f, saturate(ScopeTubeDepth)) *
        clamp(SCOPE_LENS_SCALE, 0.25f, 2.0f);
    // Optical-tube parallax.
    //
    // Two circles at different depths separate when the eye leaves the optical
    // axis. The previous form measured that offset as the aperture's distance
    // from screen centre, which is very nearly zero in ADS -- the whole point of
    // aiming is to put the optic on the axis -- so the term collapsed exactly
    // where it was supposed to work and tube depth felt inert.
    //
    // The quantity that does move while aiming is the eye-box travel already
    // being measured: sway, recoil, and the weapon's own inertia carry the eye
    // off the axis constantly. Driving depth from that gives the image a second
    // helping of the same displacement the exit pupil gets, which is exactly
    // what a recessed image does -- it swings further than the rear glass.
    //
    // The aperture's own distance from screen centre used to be added here as a
    // second off-axis source. It is removed: it is within a pixel or two of
    // zero in ADS, which is the only time any of this is visible, and reaching
    // it meant dividing a screen-space vector by the solved aperture radius --
    // a triangle-local quantity, which gave every wedge its own recessed disc
    // and drew 24 radial spikes in from the rim. It bought nothing and cost the
    // one property the mask cannot do without.
    const float2 tubeParallaxLens =
        -eyeTravelLens * saturate(ScopeTubeDepth);

    // The image and the exit pupil sit at different depths, so breathing need
    // not move them together. Pupil Follow at 0 keeps the shadow perfectly
    // still while the scene swims; at 1 the pupil rides the same sway.
    const float2 breathingPupilLens =
        breathingLens * clamp(SCOPE_BREATH_PUPIL_FOLLOW, 0.0f, 2.0f);

    const ScopeShadowLayers shadow = EvaluateScopeShadow(
        stableShadowCoordinates,
        eyeTravelLens + tubeParallaxLens + breathingPupilLens,
        physicalEyeTravelValid || saturate(ScopeTubeDepth) > 0.0f ||
            dot(breathingPupilLens, breathingPupilLens) > 0.0f,
        SCOPE_EYEBOX_RADIUS,
        shadowDepth,
        imageDiscRadius,
        SCOPE_VIGNETTE_REACH,
        SCOPE_VIGNETTE_SHARPNESS);

    opticalColor *= lerp(
        1.0f,
        shadow.visibility,
        activation);

    return float4(opticalColor, 1.0f);
}
