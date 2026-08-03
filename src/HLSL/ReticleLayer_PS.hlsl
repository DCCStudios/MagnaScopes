#include "Triangle.hlsli"
#include "ScopeShadow.hlsli"

// The reticle is captured into a private transparent layer at its authored
// draw point, then composited after the late magnified ScopeFade replay. It
// must therefore perform only reticle operations. It never reuses scene
// magnification, fisheye, refraction, denoise, sharpen, or chromatic mapping.
// It does share the bounded physical eye translation and exit-pupil
// visibility so the reticle stays attached to the optical image and passes
// behind scope shadow without itself being warped.
//
// The authored reticle is captured twice with its original blend state: once
// over black (B), once over white (W). For an affine source blend, W - B is
// exactly the destination transmittance T. The production blend state uses
// ONE/SRC1_COLOR, so the final framebuffer receives B + destination*T without
// trusting the captured alpha or turning transparent black backing opaque.
struct ReticleCompositeOutput
{
    float4 sourceContribution : SV_Target0;
    float4 destinationTransmittance : SV_Target1;
};

ReticleCompositeOutput SampleAuthoredReticle(float2 uv)
{
    ReticleCompositeOutput output;
    const float4 blackCapture =
        tBACKBUFFER.SampleLevel(gSamLinear, uv, 0.0f);
    const float4 whiteCapture =
        ReticleTex.SampleLevel(gSamLinear, uv, 0.0f);
    output.sourceContribution = blackCapture;
    output.destinationTransmittance =
        saturate(whiteCapture - blackCapture);
    return output;
}

ReticleCompositeOutput main(VertexPosHTex input)
{
    const float2 pixelSize = PixelSize;
    const float2 outputPixel = input.posH.xy;

    // STS authors align the reticle independently of ScopeFade. The game
    // thread publishes that authored screen-space center in render-target
    // pixels, so scale the captured layer about this point instead of screen
    // center or the lens center. This is the screen-space equivalent of
    // scaling every reticle vertex about the reticle's own geometric center.
    const float2 reticleCenterPixel = float2(
        SCOPE_AIM_CENTER_X,
        SCOPE_AIM_CENTER_Y);
    const bool reticleCenterValid =
        all(reticleCenterPixel >= 0.0f) &&
        reticleCenterPixel.x <= BUFFER_WIDTH &&
        reticleCenterPixel.y <= BUFFER_HEIGHT &&
        SCOPE_LENS_RADIUS_X > 0.0f &&
        SCOPE_LENS_RADIUS_Y > 0.0f;
    if (!reticleCenterValid) {
        // Projection publication can be unavailable for one frame after an
        // equip, load, or device reset. Preserve the authored reticle rather
        // than scaling or clipping it against uninitialized zero constants.
        return SampleAuthoredReticle(input.tex);
    }
    // The physical ScopeFade center is published independently from the
    // authored reticle pivot. Reconstructing it from a reticle offset and a
    // differently timed basis made the clipping polygon drift at extreme
    // pitch, leaving only partial reticle arcs.
    const float2 basisX = float2(
        SCOPE_LENS_BASIS_XX,
        SCOPE_LENS_BASIS_XY);
    const float2 basisZ = float2(
        SCOPE_LENS_BASIS_ZX,
        SCOPE_LENS_BASIS_ZY);
    const float basisDeterminant =
        basisX.x * basisZ.y - basisX.y * basisZ.x;
    const bool projectedBasisValid =
        abs(basisDeterminant) > 0.0001f;
    // Reticle parallax and scope-shadow following consume display-X/Y eye
    // travel. They remain valid when the older game-thread basis is stale or
    // nearly collinear, so only coordinate inversion keeps the determinant
    // requirement below.
    const bool physicalEyeTravelValid =
        SCOPE_PHYSICAL_EYEBOX_VALID > 0.0001f;
    const float projectedRadius = max(
        0.5f * (length(basisX) + length(basisZ)),
        1.0f);

    const float2 lensCenterPixel = float2(
        SCOPE_LENS_CENTER_X,
        SCOPE_LENS_CENTER_Y);
    float2 lensCoordinates = float2(0.0f, 0.0f);
    if (projectedBasisValid) {
        const float2 displacement = outputPixel - lensCenterPixel;
        lensCoordinates = float2(
            (displacement.x * basisZ.y -
             displacement.y * basisZ.x) /
                basisDeterminant,
            (-displacement.x * basisX.y +
             displacement.y * basisX.x) /
                basisDeterminant);
    } else {
        // A newly selected scope can briefly lack the projected basis. The
        // axis-aligned radii are a fail-closed fallback that still prevents
        // the reticle layer from painting over the weapon housing.
        const float2 fallbackRadius = max(
            float2(SCOPE_LENS_RADIUS_X, SCOPE_LENS_RADIUS_Y),
            float2(1.0f, 1.0f));
        lensCoordinates =
            (outputPixel - lensCenterPixel) / fallbackRadius;
    }

    // Reproduce the scene shader's bounded transient eye motion exactly.
    // The plus sign is the inverse display-space mapping of the scene
    // shader's negative source-sample shift. Scene magnification is
    // deliberately absent: the reticle translates with the optical image
    // but keeps its independently configured scale.
    float2 physicalEyeTravel = float2(0.0f, 0.0f);
    float2 opticalTranslation = float2(0.0f, 0.0f);
    if (physicalEyeTravelValid) {
        physicalEyeTravel =
            float2(SCOPE_EYE_OFFSET_X, SCOPE_EYE_OFFSET_Y) *
            saturate(SCOPE_PHYSICAL_EYEBOX_VALID) *
            clamp(SCOPE_OPTICAL_LAG_STRENGTH, 0.0f, 4.0f);
        // The CPU eye-box output is already filtered. Keep the final tiny
        // motion continuous so the reticle cannot snap when the lens settles.
        const float travelLength = length(physicalEyeTravel);
        const float maximumTravel =
            clamp(SCOPE_EYEBOX_MAX_TRAVEL, 0.0f, 4.0f);
        if (travelLength > maximumTravel && travelLength > 0.00001f) {
            physicalEyeTravel *= maximumTravel / travelLength;
        }
        const float reticleDepth = clamp(ScopeSceneDepth, 0.0f, 4.0f);
        const float2 depthTravel = physicalEyeTravel * reticleDepth;
        // Must stay bit-for-bit the same limiter the scene replay applies, or
        // the reticle drifts off the magnified image during fast inertia.
        const float2 sceneTravel =
            ScopeShadowSoftLimitVector(depthTravel, 1.0f);
        opticalTranslation =
            sceneTravel * projectedRadius *
            clamp(SCOPE_SCENE_PARALLAX_STRENGTH, 0.0f, 2.0f) *
            clamp(SCOPE_RETICLE_PARALLAX_STRENGTH, 0.0f, 4.0f);
    }

    // MagnaScope profiles express reticle offset as thousandths of the
    // optic-local X/Z basis. This preserves that convention while following
    // a rolled, sheared, off-center, or inertia-driven STS scope.
    const float2 userOffset =
        basisX * (SCOPE_RETICLE_OFFSET_X / 1000.0f) +
        basisZ * (SCOPE_RETICLE_OFFSET_Y / 1000.0f);
    const float sizeScale =
        clamp(SCOPE_RETICLE_SIZE, 0.01f, 128.0f) / 4.0f;
    const float targetReticleScale =
        sizeScale *
        clamp(SCOPE_RETICLE_MAGNIFICATION, 0.25f, 8.0f);
    const float reticleScale = lerp(
        1.0f,
        targetReticleScale,
        saturate(SCOPE_FADE_ACTIVATION));
    const float2 reticleOutputPivot =
        reticleCenterPixel + userOffset + opticalTranslation;
    const float2 sourcePixel =
        reticleCenterPixel +
        (outputPixel - reticleOutputPivot) / reticleScale;
    const float2 sourceUv = sourcePixel * pixelSize;

    // Never let the clamp sampler smear a reticle touching a render-target
    // edge across the opposite side of the private layer.
    if (any(sourceUv < 0.0f) || any(sourceUv > 1.0f)) {
        discard;
    }

    ReticleCompositeOutput reticle = SampleAuthoredReticle(sourceUv);

    // Match the magnified lens's exit pupil at this output pixel. The late
    // reticle uses dual-source blending (B + destination*T), so fading it
    // beneath scope shadow must interpolate that complete blend operator
    // toward identity. Multiplying only B or T would darken or brighten the
    // already-composited optical scene.
    // Evaluate the shadow in the same optic-local frame the scene replay uses.
    // lensCoordinates above is the basis-inverted coordinate, so it carries
    // ScopeFade roll and foreshortening; the isotropic pixel-radius form this
    // replaced disagreed with the scene layer whenever the optic was not
    // square-on to the camera, leaving the reticle lit inside a dark crescent.
    const float2 shadowLensCoordinates = lensCoordinates;
    // Display-X/Y travel becomes optic-local travel through the same basis
    // inverse, matching the scene replay's derivative-frame conversion.
    float2 eyeTravelLocal = float2(0.0f, 0.0f);
    if (physicalEyeTravelValid && projectedBasisValid) {
        const float2 travelPixels = physicalEyeTravel * projectedRadius;
        eyeTravelLocal = float2(
            (travelPixels.x * basisZ.y - travelPixels.y * basisZ.x) /
                basisDeterminant,
            (-travelPixels.x * basisX.y + travelPixels.y * basisX.x) /
                basisDeterminant);
    } else if (physicalEyeTravelValid) {
        eyeTravelLocal = physicalEyeTravel;
    }
    const float axialPupilScale = clamp(
        1.0f -
            clamp(SCOPE_EYE_RELIEF_DELTA, -0.25f, 0.25f) *
                saturate(SCOPE_PHYSICAL_EYEBOX_VALID) *
                saturate(SCOPE_FADE_ACTIVATION) *
                clamp(ScopeShadowDepth, 0.0f, 4.0f),
        0.96f,
        1.04f);
    // Same optical-tube parallax the scene replay applies, so the reticle is
    // occluded by the identical recessed disc rather than a concentric one.
    const float2 opticalAxisPixels =
        0.5f * float2(BUFFER_WIDTH, BUFFER_HEIGHT);
    // Must match the scene replay's parallax exactly. The screen-centre offset
    // alone is near zero in ADS, so eye-box travel carries the depth cue; see
    // the matching comment in ScopeGeometryMagnify_PS.hlsl.
    const float2 tubeParallaxLocal =
        -(((lensCenterPixel - opticalAxisPixels) / projectedRadius) +
          eyeTravelLocal) *
        saturate(ScopeTubeDepth);

    const ScopeShadowLayers shadow = EvaluateScopeShadow(
        shadowLensCoordinates,
        eyeTravelLocal + tubeParallaxLocal,
        physicalEyeTravelValid || saturate(ScopeTubeDepth) > 0.0f,
        SCOPE_EYEBOX_RADIUS,
        clamp(ScopeShadowDepth, 0.0f, 4.0f),
        axialPupilScale * lerp(1.0f, 0.6f, saturate(ScopeTubeDepth)),
        SCOPE_VIGNETTE_REACH,
        SCOPE_VIGNETTE_SHARPNESS);
    const float pupilShadow = 1.0f - shadow.visibility;
    const float reticleShadowStrength =
        clamp(SCOPE_RETICLE_SHADOW_STRENGTH, 0.0f, 1.0f);
    const float reticleVisibility = lerp(
        1.0f,
        1.0f - pupilShadow,
        saturate(SCOPE_FADE_ACTIVATION) *
            reticleShadowStrength);
    reticle.sourceContribution *= reticleVisibility;
    reticle.destinationTransmittance = lerp(
        float4(1.0f, 1.0f, 1.0f, 1.0f),
        reticle.destinationTransmittance,
        reticleVisibility);
    return reticle;
}
