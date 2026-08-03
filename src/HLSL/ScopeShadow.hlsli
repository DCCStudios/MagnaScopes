#ifndef MAGNASCOPE_SCOPE_SHADOW_HLSLI
#define MAGNASCOPE_SCOPE_SHADOW_HLSLI

// One optical-shadow contract shared by the magnified scene replay and the
// late reticle composite. Both layers must dim by the same amount at the same
// optic-local coordinate, otherwise the reticle floats over a crescent that no
// longer exists or disappears where the scene is still bright.
//
// The physical model matches the parameters real scope shaders expose (see
// 3D Shader Scopes' s3ds_eye_relief / s3ds_exit_pupil):
//
//   * ScopeFade is the fixed physical aperture. Nothing here moves or scales
//     it; lens coordinates always have radius 1 at the authored boundary.
//   * The resting rim is a fixed radial vignette that exists only in the outer
//     band. It is the tube wall seen at rest and is authored per optic.
//   * The exit pupil is a separate disc of light. Eye displacement moves that
//     disc, and the part of the aperture that falls outside it becomes a
//     one-sided crescent. Eye relief multiplies the displacement; exit-pupil
//     forgiveness divides it, exactly as a wide exit pupil tolerates more head
//     movement than a narrow one.
//
// Two invariants are structural rather than tuned, and are asserted by
// ScopeGeometryFillShaderTest:
//
//   1. The resting rim is exactly zero for every radius at or inside rimStart,
//      and rimStart is never below 0.75. The aligned centre cannot be dimmed
//      by the rim at any Vignette Reach.
//   2. The exit pupil is displaced by a soft-limited amount that can approach
//      but never reach the distance at which the aligned lens centre leaves
//      the pupil disc. The crescent is therefore exactly zero at the lens
//      centre for *every* published eye travel.
//
// Invariant 2 is what the previous build lacked. Eye travel is bounded only by
// Eye Box Max Travel, whose default permits four aperture radii, and the pupil
// radius is approximately one. An unbounded displacement therefore pushed the
// entire aperture outside the pupil disc, and the shader multiplied the whole
// optical image by zero. That produced the reported "shadow darkens the whole
// lens" defect and made every shadow control appear either dead or global.

struct ScopeShadowLayers
{
    // Fixed tube rim. Zero inside rimStart at every setting.
    float restingRim;
    // Displaced exit pupil. Zero at the aligned lens centre at every setting.
    float movingCrescent;
    // 1 - max(rim, crescent). Multiply optical colour by this, never by the
    // individual layers, so the two shadows cannot compound into a black lens.
    float visibility;
};

// Smoothly maps [0, inf) onto [0, limit) while staying almost exactly linear
// for magnitudes well below the limit. Unlike a hard clamp this has no
// derivative discontinuity, so a fast pan cannot produce a visible kink, and
// unlike value / (1 + k * value) it does not noticeably compress ordinary
// small motion (0.20 -> 0.193 at limit 0.75, versus 0.143 for the old form).
float ScopeShadowSoftLimit(float value, float limit)
{
    const float safeLimit = max(limit, 0.000001f);
    const float normalized = value / safeLimit;
    return value * rsqrt(1.0f + normalized * normalized);
}

float2 ScopeShadowSoftLimitVector(float2 value, float limit)
{
    const float magnitude = length(value);
    if (magnitude <= 0.000001f) {
        return float2(0.0f, 0.0f);
    }
    return value * (ScopeShadowSoftLimit(magnitude, limit) / magnitude);
}

// Maps a display-space pixel vector into optic-local lens units through the
// game thread's published aperture basis.
//
// Every quantity handed to EvaluateScopeShadow must live in the same frame.
// The lens coordinate is basis-inverted, so it rotates and foreshortens with
// the optic; a display-space offset added to it does not, and the mismatch
// makes the pupil and recessed image appear to spin around the lens as the
// camera pans. Both shaders route every offset through this one function so
// that class of bug cannot reappear in only one of them.
//
// The basis already carries the aperture's pixel scale, so the result is
// normalized to aperture radii without a separate radius division.
float2 ScopeShadowInvertLensBasis(
    float2 displayPixels,
    float2 basisX,
    float2 basisZ,
    float basisDeterminant)
{
    if (abs(basisDeterminant) <= 0.0001f) {
        return float2(0.0f, 0.0f);
    }
    return float2(
        (displayPixels.x * basisZ.y - displayPixels.y * basisZ.x) /
            basisDeterminant,
        (-displayPixels.x * basisX.y + displayPixels.y * basisX.x) /
            basisDeterminant);
}

// Normalized [0, 1] forms of the authored 0..20 editor ranges.
float ScopeShadowVignetteReach(float authoredReach)
{
    return saturate(authoredReach * (1.0f / 20.0f));
}

float ScopeShadowVignetteHardness(float authoredSharpness)
{
    return saturate(authoredSharpness * (1.0f / 20.0f));
}

// The rim always begins in the outer quarter of the aperture, so an aligned
// centre is never part of the resting shadow.
float ScopeShadowRimStart(float hardnessNormalizedReach)
{
    return 1.0f - 0.25f * hardnessNormalizedReach;
}

float ScopeShadowPupilFeather(float vignetteHardness)
{
    return lerp(0.16f, 0.035f, vignetteHardness);
}

// The largest exit-pupil displacement that still leaves the aligned lens
// centre strictly inside the lit disc. Deriving it from the same radius and
// feather the mask uses keeps invariant 2 true even if either is retuned.
float ScopeShadowMaximumPupilOffset(float pupilRadius, float pupilFeather)
{
    return max(pupilRadius - pupilFeather - 0.02f, 0.0f);
}

// lensCoordinates: optic-local, radius 1 at the authored ScopeFade boundary.
// eyeTravel:       optic-local eye displacement in aperture radii.
// exitPupilForgiveness: SCOPE_EYEBOX_RADIUS. Larger tolerates more travel.
// eyeRelief:       ScopeShadowDepth. Multiplies the displacement.
// pupilRadius:     axial-breathing-scaled lit-disc radius, nominally 1.
ScopeShadowLayers EvaluateScopeShadow(
    float2 lensCoordinates,
    float2 eyeTravel,
    bool eyeTravelValid,
    float exitPupilForgiveness,
    float eyeRelief,
    float pupilRadius,
    float authoredVignetteReach,
    float authoredVignetteSharpness)
{
    ScopeShadowLayers layers;

    const float reach = ScopeShadowVignetteReach(authoredVignetteReach);
    const float hardness =
        ScopeShadowVignetteHardness(authoredVignetteSharpness);
    const float shaping = lerp(0.75f, 2.25f, hardness);
    const float radialPosition = length(lensCoordinates);

    const float rimStart = ScopeShadowRimStart(reach);
    const float rimWidth = lerp(0.24f, 0.035f, hardness);
    layers.restingRim = pow(
        saturate(
            smoothstep(
                rimStart,
                min(1.0f, rimStart + rimWidth),
                radialPosition)),
        shaping);

    layers.movingCrescent = 0.0f;
    if (eyeTravelValid) {
        // A malformed profile must not be able to close the lit disc into a
        // black spot, so the pupil keeps a sane minimum radius.
        const float safePupilRadius = max(pupilRadius, 0.25f);
        const float pupilFeather = ScopeShadowPupilFeather(hardness);
        // Exit-pupil forgiveness. This is the control the scene shader used to
        // ignore entirely, which is why adjusting it changed only the reticle.
        const float forgiveness = max(exitPupilForgiveness, 0.10f);
        const float2 rawOffset =
            eyeTravel * (max(eyeRelief, 0.0f) / forgiveness);
        const float2 pupilCenter = ScopeShadowSoftLimitVector(
            rawOffset,
            ScopeShadowMaximumPupilOffset(safePupilRadius, pupilFeather));

        const float pupilDistance =
            length(lensCoordinates - pupilCenter);
        layers.movingCrescent = pow(
            saturate(
                smoothstep(
                    safePupilRadius - pupilFeather,
                    safePupilRadius + pupilFeather,
                    pupilDistance)),
            shaping);
    }

    layers.visibility =
        saturate(1.0f - max(layers.restingRim, layers.movingCrescent));
    return layers;
}

#endif
