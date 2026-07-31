#include "Triangle.hlsli"

struct ScopeGeometryPixel
{
    float4 position : SV_Position;
    float2 lensCoordinates : TEXCOORD0;
};

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
    const float publishedBasisDeterminant =
        publishedLensBasisX.x * publishedLensBasisZ.y -
        publishedLensBasisX.y * publishedLensBasisZ.x;
    const bool publishedBasisValid =
        abs(publishedBasisDeterminant) > 0.0001f;
    const bool physicalBasisValid =
        publishedBasisValid && SCOPE_PHYSICAL_EYEBOX_VALID > 0.0001f;

    const float2 authoredAimOffset =
        SCOPE_AIM_OFFSET_VALID > 0.5f ?
            float2(SCOPE_AIM_OFFSET_X, SCOPE_AIM_OFFSET_Y) :
            float2(0.0f, 0.0f);
    const float2 publishedAimPixels =
        float2(SCOPE_AIM_CENTER_X, SCOPE_AIM_CENTER_Y);
    // Aim center and basis are draw-wide constants. Keeping the sampling pivot
    // uniform is essential: even a continuous interpolator cannot turn the
    // quantized, slightly non-concentric source rings into one exact homography.
    // Per-triangle extrapolation would therefore recreate the visible wedges.
    const float2 centerPixels =
        publishedAimPixels -
        publishedLensBasisX * authoredAimOffset.x -
        publishedLensBasisZ * authoredAimOffset.y;
    const float2 centerUv = centerPixels * PixelSize;
    const float2 samplePivotUv = publishedAimPixels * PixelSize;
    const float2 normalizedLensPosition = input.lensCoordinates;
    float2 sampleDelta =
        (screenUv - samplePivotUv) / magnification;
    const float radialPosition =
        saturate(length(normalizedLensPosition));
    const float fishEyeAmount =
        clamp(SCOPE_FISHEYE_STRENGTH, 0.0f, 2.0f) *
        activation *
        pow(
            radialPosition,
            clamp(SCOPE_FISHEYE_POWER, 0.5f, 6.0f));
    sampleDelta +=
        (screenUv - centerUv) * fishEyeAmount / magnification;

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
    const float2 radialUv = screenUv - centerUv;
    sampleDelta +=
        radialUv * (refractionStrength * edgeWeight) / magnification;

    // A real optic's scene shifts slightly beneath the fixed housing as the
    // eye leaves the optical axis. The game thread measures relative motion
    // in ScopeFade-local X/Z and continuously recenters its settled baseline.
    // Do not derive travel from the aperture's absolute distance from screen
    // center: that would turn ordinary camera pitch/yaw into permanent scope
    // shadow and can move the image completely outside a stationary lens.
    float2 physicalEyeTravel = float2(0.0f, 0.0f);
    if (physicalBasisValid && SCOPE_PHYSICAL_EYEBOX_VALID > 0.0001f) {
        physicalEyeTravel =
            float2(SCOPE_EYE_OFFSET_X, SCOPE_EYE_OFFSET_Y) *
            saturate(SCOPE_PHYSICAL_EYEBOX_VALID) *
            clamp(SCOPE_OPTICAL_LAG_STRENGTH, 0.0f, 4.0f);
        // Ignore sub-pixel pose noise around the optical axis. Recoil,
        // inertia, and deliberate weapon movement exceed this dead zone.
        const float eyeTravelLength = length(physicalEyeTravel);
        if (eyeTravelLength < 0.0125f) {
            physicalEyeTravel = float2(0.0f, 0.0f);
        }
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
        const float sceneTravelLength = length(physicalEyeTravel);
        const float2 sceneTravel =
            physicalEyeTravel /
            (1.0f + 2.0f * sceneTravelLength);
        const float2 eyeParallaxPixels =
            publishedLensBasisX * sceneTravel.x +
            publishedLensBasisZ * sceneTravel.y;
        // The scene counter-shifts beneath the fixed housing. Using the same
        // sign as measured eye motion made the image lead and overshoot the
        // weapon; subtracting it produces the expected optical lag.
        sampleDelta -=
            eyeParallaxPixels *
            PixelSize *
            (clamp(SCOPE_SCENE_PARALLAX_STRENGTH, 0.0f, 2.0f) /
             magnification);
    }

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

    // Scope shadow is the overlap between the physical ScopeFade aperture and
    // a laterally moving exit pupil. An aligned eye has a pupil slightly
    // larger than the authored aperture, so the center and rim remain clear.
    // Moving the weapon shifts the pupil and reveals one dark crescent. There
    // is no symmetric full-lens multiplier and no axial eye-relief toggle.
    float pupilShadow = 0.0f;
    if (physicalBasisValid) {
        // normalizedLensPosition is solved in ScopeFade's exact projected
        // local frame. It automatically fits authored aperture size, roll,
        // shear, foreshortening, and off-center alignment.
        const float2 stableShadowCoordinates = normalizedLensPosition;
        // eyeLocal is cameraLocal minus its settled baseline, so the exit
        // pupil follows that displacement. The magnified scene moves in the
        // opposite direction above, producing a directional crescent instead
        // of the previous centered dark disk.
        const float2 pupilCenter = physicalEyeTravel;
        const float pupilRadius = clamp(
            SCOPE_EYEBOX_RADIUS * 0.55f,
            0.75f,
            2.0f);
        const float pupilFeather = clamp(
            rcp(max(SCOPE_VIGNETTE_REACH, 1.01f)),
            0.035f,
            0.18f);
        const float pupilDistance =
            length(stableShadowCoordinates - pupilCenter);
        pupilShadow = smoothstep(
            pupilRadius - pupilFeather,
            pupilRadius + pupilFeather,
            pupilDistance);
        pupilShadow = pow(
            saturate(pupilShadow),
            clamp(SCOPE_VIGNETTE_SHARPNESS / 3.0f, 0.35f, 6.0f));
    }

    opticalColor *= lerp(
        1.0f,
        1.0f - pupilShadow,
        activation *
            saturate(SCOPE_PHYSICAL_EYEBOX_VALID));

    return float4(opticalColor, 1.0f);
}
