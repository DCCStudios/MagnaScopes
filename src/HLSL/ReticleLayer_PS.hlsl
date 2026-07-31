#include "Triangle.hlsli"

// The reticle is captured into a private transparent layer at its authored
// draw point, then composited after the late magnified ScopeFade replay. It
// must therefore perform only reticle operations. In particular, it must not
// reuse the scene magnification, fisheye, refraction, denoise, parallax, or
// chromatic-aberration mapping from ScopeGeometryMagnify_PS.
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
    const float reticleScale = lerp(
        1.0f,
        clamp(SCOPE_RETICLE_MAGNIFICATION, 0.25f, 8.0f),
        saturate(SCOPE_FADE_ACTIVATION));
    const float2 sourcePixel =
        reticleCenterPixel +
        (outputPixel - reticleCenterPixel) / reticleScale;
    const float2 sourceUv = sourcePixel * pixelSize;

    // Never let the clamp sampler smear a reticle touching a render-target
    // edge across the opposite side of the private layer.
    if (any(sourceUv < 0.0f) || any(sourceUv > 1.0f)) {
        discard;
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

    // ScopeFade is a 24-segment circular plane. cos(pi / 24) is the largest
    // circle guaranteed to lie inside every edge of that polygon. Clipping to
    // this conservative boundary keeps a scaled reticle off the optic housing
    // even when the authored aperture is rolled or sheared.
    static const float safeApertureRadius = 0.9914448614f;
    if (dot(lensCoordinates, lensCoordinates) >
        safeApertureRadius * safeApertureRadius) {
        discard;
    }

    return SampleAuthoredReticle(sourceUv);
}
