// Stretches the dynamic-resolution SUBRECT of a captured frame to the whole
// target, producing a full-frame scene source whose UV space matches the
// magnify shader's sourceUv.
//
// Under a DRS-driven upscaler (the jarari Upscaling frame-generation proxy),
// the game renders the scene into the top-left [0,ratio] portion of full-size
// targets and the upscaler consumes that subrect. MagnaScope captures the
// same pre-upsample frame buffer; this pass maps full-frame UV back into the
// subrect so everything downstream can keep sampling in full-frame terms.
Texture2D tSource : register(t0);
SamplerState sLinear : register(s0);

cbuffer StretchParams : register(b0)
{
	float2 UvScale;       // dynamic width/height ratio (1,1 = no subrect)
	float2 JitterOffset;  // un-jitter shift in full-frame uv (see hooking.cpp)
};

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
	// Clamp inside the subrect so the linear filter never bleeds in the
	// garbage that lives outside the rendered region of the source.
	uint width = 0;
	uint height = 0;
	tSource.GetDimensions(width, height);
	const float2 halfTexel = 0.5f / float2(max(width, 1u), max(height, 1u));
	// The upscaler jitters the projection each frame; a 4x magnifier turns
	// that into a visible wobble, so the lookup is shifted back by it.
	const float2 unjittered = saturate(uv + JitterOffset);
	const float2 scaled = min(unjittered * UvScale, UvScale - halfTexel);
	return float4(tSource.Sample(sLinear, scaled).rgb, 1.0f);
}
