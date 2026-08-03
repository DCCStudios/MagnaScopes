// STS ScopeFade is a standardized 24-segment annulus made from 48 vertices
// and 48 triangles. The source positions are packed, so the authored inner
// ring is only approximately half the outer radius after quantization. Never
// use independently reconstructed per-primitive frames as pixel-shader data:
// their small differences become visible radial facets under magnification.

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
    // This apex is not an approximation of the center and must not be replaced
    // by a "better" one, including a center published by the game thread.
    //
    // Ask which screen point this wedge's own affine map sends to lens (0,0).
    // In barycentric terms a*dir + 0.5b*dir + 0.5c*dirNext = 0 with a+b+c = 1.
    // dir and dirNext are independent, so c = 0, and a + 0.5b = 0 with a+b = 1
    // gives a = -1, b = 2. Lens (0,0) is therefore at 2*inner - outer exactly.
    //
    // That makes the fabricated fan's projective frame identical to its parent
    // wedge's, so the mapping is continuous across the shared edge at radius
    // 0.5. Substituting any other point -- however well centered -- gives the
    // fan a different frame from the annulus and turns that edge into a visible
    // faceted circle partway out the lens, which is far worse than the faint
    // wedge-to-wedge seam that vertex quantization leaves at the center.
    GeometryInput center;
    center.position = 2.0f * input[1].position - input[0].position;
    AppendTriangle(
        stream,
        center,
        input[2],
        input[1],
        float2(0.0f, 0.0f),
        innerNextCoordinates,
        innerCurrentCoordinates);
}
