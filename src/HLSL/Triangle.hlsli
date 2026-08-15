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

	float SCOPE_RETICLE_SIZE;
	float SCOPE_RETICLE_OFFSET_X;
	float SCOPE_RETICLE_OFFSET_Y;
	float SCOPE_EYE_RELIEF_DELTA;
	float SCOPE_RETICLE_SHADOW_STRENGTH;
	float SCOPE_RETICLE_PARALLAX_STRENGTH;
	float SCOPE_LENS_CENTER_X;
	float SCOPE_LENS_CENTER_Y;

	// User placement of the optical assembly inside the authored housing.
	// STS publishes where the ScopeFade mesh is, which is not always where the
	// sight picture should sit in a given scope model. The offset is in
	// aperture radii along the optic's own X/Z axes, so it rolls with the
	// weapon; the scale multiplies the lit-image radius.
	float SCOPE_LENS_OFFSET_X;
	float SCOPE_LENS_OFFSET_Y;
	float SCOPE_LENS_SCALE;

	// Breathing sway. The phase is accumulated on the game thread rather than
	// derived from a clock here, so changing the rate bends the curve forward
	// from where it already was instead of jumping the image to wherever a
	// freshly scaled absolute time happens to land.
	float SCOPE_BREATH_PHASE;

	float SCOPE_BREATH_SWAY;
	float SCOPE_BREATH_DRIFT;
	float SCOPE_BREATH_FIGURE;
	float SCOPE_BREATH_HOLD;

	float SCOPE_BREATH_PUPIL_FOLLOW;
	// Viewport the reticle layer was captured with over the viewport this
	// composite renders at; 1.0 when they agree. Under dynamic resolution the
	// weapon pass rasterizes into a top-left sub-rectangle, the game upscales
	// its own subrect to the output, and the private layer keeps subrect
	// coordinates -- so the layer must be sampled through this ratio or the
	// reticle appears uniformly shrunk toward the top-left corner.
	float SCOPE_RETICLE_CAPTURE_SCALE_X;
	float SCOPE_RETICLE_CAPTURE_SCALE_Y;
	// Measured inner-rim over outer-rim ratio of the active aperture annulus.
	// The fill geometry shader assigns inner-ring lens coordinates and
	// extrapolates the centre-fan apex from this. Authored ScopeFade rings do
	// not sit at exactly half -- the measured corpus reads 0.497 -- and
	// assuming 0.5 planted each wedge's fabricated apex on a circle around
	// the true centre rather than one point.
	float SCOPE_APERTURE_INNER_RATIO;

	// Custom reticle. Negative selects the authored 3D reticle captured into
	// the private layer; zero or above selects the bound texture at t6. The
	// scale is in aperture radii.
	//
	// This row, ConstBufferData in hooking.h, and the static_assert beside it
	// are three hand-maintained copies of one layout. The assert only checks
	// the C++ side, so drift here is silent constant corruption.
	float SCOPE_CUSTOM_RETICLE_INDEX;
	float SCOPE_CUSTOM_RETICLE_SCALE;
	float SCOPE_RESERVED_RETICLE_0;
	float SCOPE_RESERVED_RETICLE_1;
};

// Breathing displacement in aperture radii, shared so the magnified scene and
// the reticle's exit-pupil shadow cannot drift out of step.
//
// The shaping exponent flattens the turning points as Hold rises, which is what
// makes the drift dwell at the extremes the way a real breath pauses, rather
// than sweeping through them at constant speed like a plain sine.
float2 ScopeBreathingOffset()
{
    const float sway = SCOPE_BREATH_SWAY;
    const float drift = SCOPE_BREATH_DRIFT;
    if (abs(sway) + abs(drift) < 0.00001f) {
        return float2(0.0f, 0.0f);
    }
    const float twoPi = 6.28318530717958647692f;
    const float verticalPhase = SCOPE_BREATH_PHASE;
    const float horizontalPhase =
        verticalPhase + clamp(SCOPE_BREATH_FIGURE, 0.0f, 1.0f) * twoPi;
    const float shaping =
        1.0f / (1.0f + 1.5f * saturate(SCOPE_BREATH_HOLD));
    const float verticalRaw = sin(verticalPhase);
    const float horizontalRaw = sin(horizontalPhase);
    const float vertical =
        sign(verticalRaw) * pow(abs(verticalRaw), shaping);
    const float horizontal =
        sign(horizontalRaw) * pow(abs(horizontalRaw), shaping);
    return float2(horizontal * drift, vertical * sway);
}

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

	float2 ScopeScreenPos;
	float2 Reticle_Offset;

	row_major float4x4 projMat;

	float4 ScopeEffect_Rect;

	// MW2019-style lens distortion: strength 0 disables it, power shapes
	// how sharply the bend ramps toward the lens edge.
	float FishEyeStrength;
	float FishEyePower;
	float ScopeSceneDepth;
	float ScopeShadowDepth;

	// How much of the aperture's own screen motion the magnified image
	// declines to follow. 0 locks the image to the housing exactly as before;
	// 1 leaves it world-static so the housing slides over a still image, which
	// is what conveys a long optical tube. This scales the sample pivot rather
	// than the sample delta, so the perceived stillness is identical at every
	// magnification -- a delta-space shift cannot do that, because the pivot's
	// own contribution carries a (1 - 1/M) factor the delta term does not.
	float ScopeImageStillness;
	// Fore/aft apparent-size breathing. Kept separate from ScopeSceneDepth so
	// lateral parallax can be tuned without making the image appear to move
	// closer and farther. Zero by default: camera yaw must never read as depth.
	float ScopeAxialBreathing;
	// Current projected aperture radius over its settled radius. Moving
	// toward or away from a target changes the housing's apparent size far
	// more than it changes its screen position, so this -- not the centre
	// excursion -- is what image stillness has to compensate.
	float ScopeApertureScaleRatio;
	// How far the magnified image sits toward the front of the tube. 0 puts
	// it at the rear aperture so it fills the glass. Higher values recess it,
	// leaving a ring of tube wall between the aperture edge and the image
	// disc -- the empty space that reads as optical depth and that the exit
	// pupil then slides across as the eye moves off axis.
	float ScopeTubeDepth;
};


SamplerState gSamLinear : register(s0);
SamplerState gSamReticle : register(s1);
Texture2D tBACKBUFFER : register(t4);
Texture2D ReticleTex : register(t5);
// User-supplied reticle texture for the reticle composite. Unbound samples
// transparent black in D3D11, and SCOPE_CUSTOM_RETICLE_INDEX is held negative
// until a texture is actually resident, so an unbound slot is never read.
Texture2D CustomReticleTex : register(t6);


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
