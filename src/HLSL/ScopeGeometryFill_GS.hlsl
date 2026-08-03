// STS ScopeFade is a standardized 24-segment annulus made from 48 vertices
// and 48 triangles. The source positions are packed, so the authored inner
// ring is only approximately half the outer radius after quantization. Never
// use independently reconstructed per-primitive frames as pixel-shader data:
// their small differences become visible radial facets under magnification.

#include "Triangle.hlsli"

struct GeometryInput
{
    float4 position : SV_Position;
};

struct GeometryOutput
{
    float4 position : SV_Position;

    // Publish homogeneous lens coordinates with no perspective correction.
    // The rasterizer linearly interpolates (localX / clipW,
    // localZ / clipW, 1 / clipW) in display space. Dividing XY by Z recovers
    // the authored lens coordinate exactly, while XY itself remains affine
    // across the projected triangle. The pixel shader can therefore solve the
    // projective center without pretending that perspective-correct local
    // coordinates have one constant screen derivative.
    noperspective float3 lensProjective : TEXCOORD0;
};

float3 MakeProjectiveLensCoordinates(
    float4 clipPosition,
    float2 lensCoordinates)
{
    const float safeClipW =
        abs(clipPosition.w) > 0.000001f ?
            clipPosition.w :
            (clipPosition.w < 0.0f ? -0.000001f : 0.000001f);
    const float reciprocalW = rcp(safeClipW);
    return float3(lensCoordinates * reciprocalW, reciprocalW);
}

void AppendTriangle(
    inout TriangleStream<GeometryOutput> stream,
    GeometryInput first,
    GeometryInput second,
    GeometryInput third,
    float2 firstLensCoordinates,
    float2 secondLensCoordinates,
    float2 thirdLensCoordinates)
{
    GeometryOutput output;
    output.position = first.position;
    output.lensProjective = MakeProjectiveLensCoordinates(
        first.position,
        firstLensCoordinates);
    stream.Append(output);
    output.position = second.position;
    output.lensProjective = MakeProjectiveLensCoordinates(
        second.position,
        secondLensCoordinates);
    stream.Append(output);
    output.position = third.position;
    output.lensProjective = MakeProjectiveLensCoordinates(
        third.position,
        thirdLensCoordinates);
    stream.Append(output);
    stream.RestartStrip();
}

[maxvertexcount(6)]
void main(
    triangle GeometryInput input[3],
    uint primitiveID : SV_PrimitiveID,
    inout TriangleStream<GeometryOutput> stream)
{
    // Odd ScopeFade primitives are outerCurrent, innerCurrent, innerNext.
    // Even primitives are outerCurrent, innerNext, outerNext. Assign logical
    // coordinates from that topology instead of measuring the quantized input.
    // The wrap makes segment 23's next coordinate bit-identical to segment 0.
    const bool oddPrimitive = (primitiveID & 1U) != 0U;
    const uint segment = primitiveID >> 1U;
    const uint nextSegment = (segment + 1U) % 24U;
    const float angleStep = 6.28318530717958647692f / 24.0f;
    const float angle = (float)segment * angleStep;
    const float nextAngle = (float)nextSegment * angleStep;
    const float2 outerCurrentCoordinates = float2(sin(angle), cos(angle));
    const float2 outerNextCoordinates = float2(sin(nextAngle), cos(nextAngle));
    const float2 innerCurrentCoordinates = outerCurrentCoordinates * 0.5f;
    const float2 innerNextCoordinates = outerNextCoordinates * 0.5f;

    if (oddPrimitive) {
        AppendTriangle(
            stream,
            input[0],
            input[1],
            input[2],
            outerCurrentCoordinates,
            innerCurrentCoordinates,
            innerNextCoordinates);
    } else {
        AppendTriangle(
            stream,
            input[0],
            input[1],
            input[2],
            outerCurrentCoordinates,
            innerNextCoordinates,
            outerNextCoordinates);
        return;
    }

    // The odd primitive contains both inner-ring endpoints. Preserve the
    // established fill geometry and winding, but publish one logical origin
    // for every generated center. Overlapping sub-pixel center estimates now
    // sample identical optical coordinates instead of forming a visible star.
    //
    // Extrapolating 2*inner - outer is geometrically correct for an exact
    // half-radius inner ring, and clip space is affine in world space, so the
    // arithmetic is sound. What is not sound is doing it per primitive: the
    // packed inner vertex carries quantization error, the factor of two
    // doubles it, and each of the 24 wedges lands on a slightly different
    // apex. All 24 are then labelled lens coordinate zero, so each wedge
    // interpolates a slightly different screen-to-lens mapping and the seams
    // show as facets once the pixel shader magnifies the center.
    //
    // The game thread already projects the ScopeFade center for the pixel and
    // reticle shaders. Reusing it here costs nothing and makes every wedge
    // converge on one screen point, which is the only property that removes
    // the seam. Depth and w stay local: their per-wedge spread only perturbs
    // the recovered coordinate to second order, because the apex numerator is
    // exactly zero regardless of w.
    GeometryInput center;
    const float4 localApex = 2.0f * input[1].position - input[0].position;
    center.position = localApex;

    const float2 publishedCenterPixels =
        float2(SCOPE_LENS_CENTER_X, SCOPE_LENS_CENTER_Y);
    const float projectedRadius = max(
        0.5f * (length(float2(SCOPE_LENS_BASIS_XX, SCOPE_LENS_BASIS_XY)) +
                length(float2(SCOPE_LENS_BASIS_ZX, SCOPE_LENS_BASIS_ZY))),
        1.0f);
    const bool publishedCenterValid =
        BUFFER_WIDTH > 0.0f &&
        BUFFER_HEIGHT > 0.0f &&
        all(publishedCenterPixels >= 0.0f) &&
        publishedCenterPixels.x <= BUFFER_WIDTH &&
        publishedCenterPixels.y <= BUFFER_HEIGHT &&
        SCOPE_LENS_RADIUS_X > 0.0f &&
        SCOPE_LENS_RADIUS_Y > 0.0f &&
        abs(localApex.w) > 0.000001f;
    if (publishedCenterValid) {
        const float2 localApexPixels = float2(
            (localApex.x / localApex.w * 0.5f + 0.5f) * BUFFER_WIDTH,
            (0.5f - localApex.y / localApex.w * 0.5f) * BUFFER_HEIGHT);
        // Fail closed. A publication that is stale by a frame or measured
        // against different geometry would drag the shared apex off the
        // optical axis and leave a seam at the inner ring instead of at the
        // wedge boundaries, which is worse than the faceting it replaces.
        // Below a quarter radius the two agree to within quantization.
        if (length(publishedCenterPixels - localApexPixels) <=
            0.25f * projectedRadius) {
            const float2 publishedNdc = float2(
                2.0f * publishedCenterPixels.x / BUFFER_WIDTH - 1.0f,
                1.0f - 2.0f * publishedCenterPixels.y / BUFFER_HEIGHT);
            center.position = float4(
                publishedNdc * localApex.w,
                localApex.z,
                localApex.w);
        }
    }
    AppendTriangle(
        stream,
        center,
        input[2],
        input[1],
        float2(0.0f, 0.0f),
        innerNextCoordinates,
        innerCurrentCoordinates);
}
