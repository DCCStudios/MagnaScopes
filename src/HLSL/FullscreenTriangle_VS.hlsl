// Fullscreen triangle from SV_VertexID: no vertex buffer, no input layout.
// Used by MagnaScope's own blit passes (the DRS subrect stretch).
struct Output
{
	float4 position : SV_Position;
	float2 uv : TEXCOORD0;
};

Output main(uint vertexId : SV_VertexID)
{
	Output output;
	// (0,0) (2,0) (0,2) in UV covers the whole viewport with one triangle.
	const float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
	output.uv = uv;
	output.position = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, 0.0f, 1.0f);
	return output;
}
