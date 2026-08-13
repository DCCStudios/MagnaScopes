// STS ScopeFade is a standardized 24-segment annulus made from 48 vertices
// and 48 triangles. Never use independently reconstructed per-primitive
// frames as pixel-shader data: their small differences become visible radial
// facets under magnification.
//
// The inner ring is NOT assumed to sit at half the outer radius. Authored
// rings do not: the measured corpus reads 0.497, and the packed vertex format
// quantizes on top of that. The CPU measures the true ratio from the mesh's
// own vertices and publishes it in b4; both the inner lens coordinates and
// the fabricated centre fan are derived from it below. When this was a
// hardcoded 0.5, each wedge's fabricated apex landed 2*(0.497 - 0.5) of the
// outer radius away from the true centre -- in a different radial direction
// per wedge, so the 24 apexes formed a visible circle: fan triangles cracked
// and overlapped between mismatched apexes, and every wedge sampled the scene
// through a slightly different frame. Under magnification that is a radial
// star of seams meeting at the lens centre.
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
    // The measured ratio, uniform across the whole draw because it is one
    // published scalar -- which is exactly what keeps every wedge's derived
    // frame and fabricated apex mutually consistent.
    const float innerRatio =
        clamp(SCOPE_APERTURE_INNER_RATIO, 0.05f, 0.95f);
    const float2 outerCurrentCoordinates = float2(sin(angle), cos(angle));
    const float2 outerNextCoordinates = float2(sin(nextAngle), cos(nextAngle));
    const float2 innerCurrentCoordinates =
        outerCurrentCoordinates * innerRatio;
    const float2 innerNextCoordinates = outerNextCoordinates * innerRatio;

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
    // The inner vertex carries coordinate rho*dir and the outer carries dir,
    // so walking the outer->inner ray to coordinate zero takes parameter
    // t = 1/(1 - rho): apex = O + (I - O)/(1 - rho). In barycentric terms
    // a*dir + b*rho*dir + c*rho*dirNext = 0 with a+b+c = 1 gives c = 0,
    // a = -rho/(1-rho), b = 1/(1-rho) -- and the coordinate the fan assigns
    // there, a*dir + b*rho*dir, is exactly zero, so the fabricated fan's
    // projective frame remains identical to its parent wedge's and the
    // mapping stays continuous across the shared edge at radius rho. With
    // rho hardcoded to 0.5 this reduced to 2*inner - outer, which put the
    // apex off centre by the full difference between 0.5 and the authored
    // ratio, per wedge, in the wedge's own radial direction.
    //
    // Substituting any other point -- however well centered -- gives the
    // fan a different frame from the annulus and turns that edge into a
    // visible faceted circle partway out the lens, which is far worse than
    // the faint wedge-to-wedge seam that vertex quantization leaves at the
    // center.
    GeometryInput center;
    center.position =
        input[0].position +
        (input[1].position - input[0].position) / (1.0f - innerRatio);
    AppendTriangle(
        stream,
        center,
        input[2],
        input[1],
        float2(0.0f, 0.0f),
        innerNextCoordinates,
        innerCurrentCoordinates);
}
