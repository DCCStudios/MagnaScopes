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
    // A newly selected scope can briefly lack the projected basis, which would
    // leave projectedRadius clamped at its one-pixel floor and shadow the whole
    // layer. The axis-aligned published radii are the fail-closed fallback.
    const float shadowRadius =
        projectedBasisValid ?
            projectedRadius :
            max(
                0.5f * (SCOPE_LENS_RADIUS_X + SCOPE_LENS_RADIUS_Y),
                1.0f);

    // Transient eye motion, used from here on only by the exit pupil, exactly
    // as in the scene replay.
    float2 physicalEyeTravel = float2(0.0f, 0.0f);
    if (physicalEyeTravelValid) {
        physicalEyeTravel =
            float2(SCOPE_EYE_OFFSET_X, SCOPE_EYE_OFFSET_Y) *
            saturate(SCOPE_PHYSICAL_EYEBOX_VALID) *
            clamp(SCOPE_OPTICAL_LAG_STRENGTH, 0.0f, 4.0f);
        // The CPU eye-box output is already filtered. Keep the final tiny
        // motion continuous so the pupil cannot snap when the lens settles.
        const float travelLength = length(physicalEyeTravel);
        const float maximumTravel =
            clamp(SCOPE_EYEBOX_MAX_TRAVEL, 0.0f, 4.0f);
        if (travelLength > maximumTravel && travelLength > 0.00001f) {
            physicalEyeTravel *= maximumTravel / travelLength;
        }
    }

    // How far the reticle follows the lagging image. It is fixed to the
    // weapon, so the physical answer is "not at all" and Reticle Parallax
    // Strength defaults low; this exists because an authored reticle that
    // detaches completely from the sight picture can read as a HUD overlay.
    //
    // It must be derived from the same raw aperture motion and the same Image
    // Lag the scene replay pivots by, unscaled by Optical Lag Strength and
    // unlimited by the soft limiter, or the reticle tracks a curve the image
    // is not on. Reproducing the retired delta path's chain of gains here is
    // what previously made these two disagree during fast inertia.
    float2 opticalTranslation = float2(0.0f, 0.0f);
    if (physicalEyeTravelValid) {
        const float2 apertureMotionPixels =
            float2(SCOPE_EYE_OFFSET_X, SCOPE_EYE_OFFSET_Y) *
            saturate(SCOPE_PHYSICAL_EYEBOX_VALID) *
            projectedRadius;
        opticalTranslation =
            apertureMotionPixels *
            saturate(ScopeImageStillness) *
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
    // The shadow is evaluated in an isotropic screen-space frame.
    //
    // The published basis is not a similarity transform: its two columns
    // foreshorten independently as the optic turns relative to the camera, so
    // inverting it squashes a circular pupil into a slit. The scene replay
    // evaluates its mask in the ScopeFade geometry's own lens coordinate, which
    // is circular by construction, and this has to match it. Both layers are
    // radially symmetric, so what they need to agree on is the pupil's size and
    // screen-space displacement, not the optic's roll -- and an isotropic frame
    // reproduces both.
    //
    // The authored Lens Center displaces the sight picture and its pupil. The
    // reticle keeps its own independent Reticle Offset, so this is applied only
    // to the shadow coordinate: the reticle must pass behind the same crescent
    // the scene shows, without being dragged off its own alignment.
    const float2 shadowLensCoordinates =
        (outputPixel - lensCenterPixel) / shadowRadius -
        float2(SCOPE_LENS_OFFSET_X, SCOPE_LENS_OFFSET_Y);
    // Published travel is already normalized in aperture radii, so in that same
    // isotropic frame it needs no conversion at all.
    float2 eyeTravelLocal = float2(0.0f, 0.0f);
    if (physicalEyeTravelValid) {
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
    // Must match the scene replay's parallax exactly. Eye-box travel is the
    // whole of it; the aperture's offset from screen centre was dropped there
    // and must stay dropped here, or the reticle's pupil would sit a fraction
    // of a radius away from the scene's. See the matching comment in
    // ScopeGeometryMagnify_PS.hlsl.
    const float2 tubeParallaxLocal =
        -eyeTravelLocal * saturate(ScopeTubeDepth);

    // The reticle is fixed to the weapon, so breathing must not translate it --
    // the scene swims underneath while the crosshair holds. Its exit pupil does
    // follow, by exactly the amount the scene replay applies, or the reticle
    // would stay lit inside a crescent the scene had already darkened.
    const float2 breathingPupilLocal =
        ScopeBreathingOffset() *
        clamp(SCOPE_BREATH_PUPIL_FOLLOW, 0.0f, 2.0f);

    const ScopeShadowLayers shadow = EvaluateScopeShadow(
        shadowLensCoordinates,
        eyeTravelLocal + tubeParallaxLocal + breathingPupilLocal,
        physicalEyeTravelValid || saturate(ScopeTubeDepth) > 0.0f ||
            dot(breathingPupilLocal, breathingPupilLocal) > 0.0f,
        SCOPE_EYEBOX_RADIUS,
        clamp(ScopeShadowDepth, 0.0f, 4.0f),
        axialPupilScale *
            lerp(1.0f, 0.6f, saturate(ScopeTubeDepth)) *
            clamp(SCOPE_LENS_SCALE, 0.25f, 2.0f),
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
