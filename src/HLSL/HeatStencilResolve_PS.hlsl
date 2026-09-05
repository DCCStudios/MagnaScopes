// Resolves the heat-mask stencil bit into the R8 mask, once per frame.
//
// Actor draws are marked in the scene's stencil buffer during their own draw
// (a clone of the game's depth-stencil state with one extra write bit, so the
// hardware depth test provides exact occlusion). Nothing is re-issued and no
// extra render target is bound mid-scene: that per-actor target switching is
// what made ENB misjudge the frame under dynamic resolution.
//
// Under dynamic resolution the stencil holds the scene in its top-left
// subrect; the mask is full-window, so the lookup is scaled by the ratio.
Texture2D<uint2> tStencil : register(t0);  // X24_TYPELESS_G8_UINT: stencil in .g

cbuffer ResolveParams : register(b0)
{
	float2 UvScale;     // dynamic-resolution ratio (1,1 = full frame)
	uint   MatchValue;  // exact stencil value written for actors
	uint   MatchMask;   // bits compared (0xFF: the whole byte)
};

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
	uint width = 0;
	uint height = 0;
	tStencil.GetDimensions(width, height);
	const float2 scaled = uv * UvScale;
	int2 texel = int2(scaled * float2(width, height));
	texel = clamp(texel, int2(0, 0), int2(int(width) - 1, int(height) - 1));
	const uint stencil = tStencil.Load(int3(texel, 0)).g;
	// Exact match: the game writes whole stencil bytes with REPLACE, and one
	// of its passes writes 0xFF, so testing a single bit lit the entire view.
	const float marked = ((stencil & MatchMask) == (MatchValue & MatchMask)) ? 1.0f : 0.0f;
	return float4(marked, marked, marked, marked);
}
