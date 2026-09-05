// Flat white fill for the actor heat mask, depth-occluded against the scene.
//
// The re-issued draw keeps the game's own vertex shader and constants, so
// SV_Position.z is the same [0,1] depth the scene pass wrote (FO4 is
// non-reversed: near = 0). Comparing it against the engine's main depth,
// sampled at this pixel's own normalized screen position, reproduces a
// LESS_EQUAL depth test without binding a DSV -- a DSV legally has to match
// the render target's dimensions, and the scene depth's render resolution
// does not match the full-window mask under the engine's mixed-resolution
// passes. Occluded pixels are clipped and never mark the mask, so actors
// behind cover do not glow.
//
// Takes only SV_Position so it links with any character vertex shader.

Texture2D<float> tSceneDepth : register(t0);

cbuffer MaskParams : register(b0)
{
	float2 RcpMaskSize;   // 1 / mask (window) dimensions
	// Dynamic-resolution ratio of the scene depth: under a DRS-driven
	// upscaler the depth buffer only holds the scene in its top-left
	// [0,ratio] subrect, so a full-frame UV has to be scaled into it.
	// (1,1) when no dynamic resolution is active.
	float2 DepthUvScale;
};

float4 main(float4 pos : SV_Position) : SV_Target
{
	const float2 uv = pos.xy * RcpMaskSize * DepthUvScale;
	uint depthWidth = 0;
	uint depthHeight = 0;
	tSceneDepth.GetDimensions(depthWidth, depthHeight);
	const int2 texel = int2(uv * float2(depthWidth, depthHeight));
	float sceneDepth = tSceneDepth.Load(int3(texel, 0));
	// Fallout draws the first-person weapon into a reserved near band
	// (rawDepth <= 0.01 -- the same viewmodel depth contract ShaderEngine's
	// viewmodel DOF/AO rely on). The weapon covers most of the magnifier's
	// source region, so honoring that band as an occluder blanked every
	// actor beyond a couple of meters while a corner past the gun's
	// silhouette worked perfectly. The player's own weapon is never a
	// meaningful thermal occluder; world geometry cannot legitimately land
	// this close (~15 cm), so treat the band as empty sky.
	if (sceneDepth <= 0.01f) {
		sceneDepth = 1.0f;
	}
	// Visible when this pixel is at or in front of the scene surface. The
	// epsilon absorbs the re-rasterization's rounding against the original
	// pass (the same geometry drawn twice lands within it).
	clip(sceneDepth + 1e-5f - pos.z);
	return float4(1.0f, 1.0f, 1.0f, 1.0f);
}
