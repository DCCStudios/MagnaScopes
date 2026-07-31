struct VertexPosTex
{
    float3 posL : POSITION;
    float2 tex : TEXCOORD;
};

struct VertexPosHTex
{
    float4 posH : SV_POSITION;
    float2 tex : TEXCOORD;
};


cbuffer ResolutionConstantData : register(b4){
	float BUFFER_WIDTH;
	float BUFFER_HEIGHT;
	float SCOPE_FADE_MAGNIFICATION;
	float SCOPE_AIM_OFFSET_VALID;

	float SCOPE_AIM_OFFSET_X;
	float SCOPE_AIM_OFFSET_Y;
	float SCOPE_IMAGE_DENOISE;
	float SCOPE_IMAGE_SHARPEN;

	float SCOPE_FISHEYE_STRENGTH;
	float SCOPE_FISHEYE_POWER;
	float SCOPE_LENS_RADIUS_X;
	float SCOPE_LENS_RADIUS_Y;

	float SCOPE_AIM_CENTER_X;
	float SCOPE_AIM_CENTER_Y;
	float SCOPE_RETICLE_MAGNIFICATION;
	float SCOPE_FADE_ACTIVATION;

	float SCOPE_EDGE_REFRACTION_STRENGTH;
	float SCOPE_EDGE_REFRACTION_WIDTH;
	float SCOPE_EDGE_CHROMATIC_ABERRATION;
	float SCOPE_SCENE_PARALLAX_STRENGTH;

	float SCOPE_EYE_OFFSET_X;
	float SCOPE_EYE_OFFSET_Y;
	float SCOPE_OPTICAL_LAG_STRENGTH;
	float SCOPE_PHYSICAL_EYEBOX_VALID;

	float SCOPE_LENS_BASIS_XX;
	float SCOPE_LENS_BASIS_XY;
	float SCOPE_LENS_BASIS_ZX;
	float SCOPE_LENS_BASIS_ZY;

	float SCOPE_EYEBOX_RADIUS;
	float SCOPE_VIGNETTE_REACH;
	float SCOPE_VIGNETTE_SHARPNESS;
	float SCOPE_EYEBOX_MAX_TRAVEL;
};

cbuffer ScopeEffectData : register(b5)
{
	
	float camDepth;
	float GameFov;
	float ScopeEffect_Zoom;
	float parallax_Radius;

	float parallax_relativeFogRadius;
	float parallax_scopeSwayAmount;
	float parallax_maxTravel;
	float ReticleSize;

	float nvIntensity;
	float BaseWeaponPos;
	float MovePercentage;
	int EnableZMove;
	
	int isCircle;
	int EnableNV;
	int EnableMerge;
	float padding;

	float2 ScopeEffect_Size;
	float2 ScopeEffect_OriPositionOffset;

	float2 ScopeEffect_OriSize;
	float2 ScopeEffect_Offset;
	float3 eyeDirection;
	float baseAdjustFov = 0;

	float3 eyeDirectionLerp;
	float targetAdjustFov = 0;

	float3 eyeTranslationLerp;
	float padding3 = 0;

	float3 CurrWeaponPos;
	float padding4 = 0;

	float3 CurrRootPos;
	float padding5 = 0;

	float4x4 CameraRotation; 

	float2 FTS_ScreenPos;
	float2 Reticle_Offset;

	row_major float4x4 projMat;

	float4 ScopeEffect_Rect;

	// MW2019-style lens distortion: strength 0 disables it, power shapes
	// how sharply the bend ramps toward the lens edge.
	float FishEyeStrength;
	float FishEyePower;
	float2 padding6;
};


SamplerState gSamLinear : register(s0);
SamplerState gSamReticle : register(s1);
Texture2D tBACKBUFFER : register(t4);
Texture2D ReticleTex : register(t5);


float GetAspectRatio() { return BUFFER_WIDTH * rcp(BUFFER_HEIGHT); }
float2 GetPixelSize() { return float2(rcp(BUFFER_WIDTH), rcp(BUFFER_HEIGHT)); }
float2 GetScreenSize() { return float2(BUFFER_WIDTH, BUFFER_HEIGHT); }
#define AspectRatio GetAspectRatio()
#define PixelSize GetPixelSize()
#define ScreenSize GetScreenSize()
float2 aspect_ratio_correction(float2 tc)
{
	tc.x -= 0.5f;
	tc.x *= AspectRatio;
	tc.x += 0.5f;
	return tc;
}

float2 clampMagnitude(float2 v, float l)
{
	return normalize(v) * min(length(v), l);
}

float getparallax(float d, float2 ds, float dfov)
{
	return clamp(1 - pow(abs(rcp(parallax_Radius * ds.y) * (parallax_relativeFogRadius * d * ds.y)), parallax_scopeSwayAmount), 0, parallax_maxTravel);
}

float4 NVGEffect(float4 color, float2 texcoord)
{
	float lum = dot(color.rgb, float3(0.30f, 0.59f, 0.11f));
	float3 nvColor = float3(0, lum * (-log(lum) + 1), 0);

	float LinesOn = 2;
	float LinesOff = 2;
	float OffIntensity = 0.5f;

	float intensity = texcoord.y * BUFFER_HEIGHT % (LinesOn + LinesOff);
	bool larger = intensity < LinesOn;
	intensity = 1.0f - larger * OffIntensity;

	return float4(saturate(nvColor.xyz * intensity * nvIntensity), 1);
}
