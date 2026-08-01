#include "Triangle.hlsli"

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
    // Recover the physical lens center from the authored reticle-minus-lens
    // offset. The projected X/Z basis follows a translated, rolled, or
    // sheared STS optic without assuming that its manually aligned reticle is
    // at screen center.
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

    float2 lensCenterPixel = reticleCenterPixel;
    float2 lensCoordinates = float2(0.0f, 0.0f);
    if (projectedBasisValid) {
        const float2 authoredOffset =
            SCOPE_AIM_OFFSET_VALID > 0.5f ?
                float2(SCOPE_AIM_OFFSET_X, SCOPE_AIM_OFFSET_Y) :
                float2(0.0f, 0.0f);
        lensCenterPixel -=
            basisX * authoredOffset.x +
            basisZ * authoredOffset.y;
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
    if (projectedBasisValid && SCOPE_PHYSICAL_EYEBOX_VALID > 0.0001f) {
        physicalEyeTravel =
            float2(SCOPE_EYE_OFFSET_X, SCOPE_EYE_OFFSET_Y) *
            saturate(SCOPE_PHYSICAL_EYEBOX_VALID) *
            clamp(SCOPE_OPTICAL_LAG_STRENGTH, 0.0f, 4.0f);
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
        const float2 sceneTravel =
            physicalEyeTravel /
            (1.0f + 2.0f * length(physicalEyeTravel));
        opticalTranslation =
            (basisX * sceneTravel.x + basisZ * sceneTravel.y) *
            clamp(SCOPE_SCENE_PARALLAX_STRENGTH, 0.0f, 2.0f);
    }

    // Existing FTS profiles express reticle offset as thousandths of the
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

    // ScopeFade is a 24-segment circular plane. cos(pi / 24) is the largest
    // circle guaranteed to lie inside every edge of that polygon. Clipping to
    // this conservative boundary keeps a scaled reticle off the optic housing
    // even when the authored aperture is rolled or sheared.
    static const float safeApertureRadius = 0.9914448614f;
    if (dot(lensCoordinates, lensCoordinates) >
        safeApertureRadius * safeApertureRadius) {
        discard;
    }

    ReticleCompositeOutput reticle = SampleAuthoredReticle(sourceUv);

    // Match the magnified lens's exit pupil at this output pixel. The late
    // reticle uses dual-source blending (B + destination*T), so fading it
    // beneath scope shadow must interpolate that complete blend operator
    // toward identity. Multiplying only B or T would darken or brighten the
    // already-composited optical scene.
    float pupilShadow = 0.0f;
    if (projectedBasisValid) {
        const float pupilRadius = clamp(
            SCOPE_EYEBOX_RADIUS * 0.55f,
            0.75f,
            2.0f);
        const float pupilFeather = clamp(
            rcp(max(SCOPE_VIGNETTE_REACH, 1.01f)),
            0.035f,
            0.18f);
        const float pupilDistance =
            length(lensCoordinates - physicalEyeTravel);
        pupilShadow = smoothstep(
            pupilRadius - pupilFeather,
            pupilRadius + pupilFeather,
            pupilDistance);
        pupilShadow = pow(
            saturate(pupilShadow),
            clamp(SCOPE_VIGNETTE_SHARPNESS / 3.0f, 0.35f, 6.0f));
    }
    const float reticleVisibility = lerp(
        1.0f,
        1.0f - pupilShadow,
        saturate(SCOPE_FADE_ACTIVATION) *
            saturate(SCOPE_PHYSICAL_EYEBOX_VALID));
    reticle.sourceContribution *= reticleVisibility;
    reticle.destinationTransmittance = lerp(
        float4(1.0f, 1.0f, 1.0f, 1.0f),
        reticle.destinationTransmittance,
        reticleVisibility);
    return reticle;
}
