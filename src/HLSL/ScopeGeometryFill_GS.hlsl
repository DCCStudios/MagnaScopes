// STS ScopeFade is a standardized 24-segment annulus made from 48 vertices
// and 48 triangles. Each odd primitive is ordered as outerCurrent,
// innerCurrent, innerNext. The first two positions lie on the same radial
// line, with the inner ring at half the outer radius. The object-space center
// is therefore:
//
//     center = 2 * innerCurrent - outerCurrent
//
// A vertex shader transform is linear in homogeneous clip space, so the same
// relation reconstructs the exact current-frame center after Fallout 4's
// authored scope transform. This avoids a CPU-projected center and its
// one-frame motion lag.

struct GeometryVertex
{
    float4 position : SV_Position;
};

void AppendTriangle(
    inout TriangleStream<GeometryVertex> stream,
    GeometryVertex first,
    GeometryVertex second,
    GeometryVertex third)
{
    stream.Append(first);
    stream.Append(second);
    stream.Append(third);
    stream.RestartStrip();
}

[maxvertexcount(6)]
void main(
    triangle GeometryVertex input[3],
    uint primitiveID : SV_PrimitiveID,
    inout TriangleStream<GeometryVertex> stream)
{
    // Preserve ScopeFade's authored annulus. Its existing rasterizer, depth,
    // blend, target, and draw order remain active around this shader.
    AppendTriangle(stream, input[0], input[1], input[2]);

    if ((primitiveID & 1U) == 0U)
    {
        return;
    }

    GeometryVertex center;
    center.position = 2.0f * input[1].position - input[0].position;

    // Reverse the inner edge relative to the odd annulus triangle so the fill
    // keeps the authored front-face winding at ScopeFade's physical depth.
    AppendTriangle(stream, center, input[2], input[1]);
}
