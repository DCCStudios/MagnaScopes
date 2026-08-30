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
	// Reconstruction filter for the magnified sample: 0 bilinear, 1
	// Catmull-Rom bicubic, 2 Lanczos-2. Claimed from the first reserved
	// slot, so the layout (and the C++ static_assert) is unchanged.
	float SCOPE_MAGNIFICATION_FILTER;
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

	// Vision modes. EnableNV and nvIntensity already exist earlier in this
	// buffer (reused as the NV toggle and gain); these two rows extend NV with
	// realism controls and add the thermal-mode controls. Appended as whole
	// 16-byte rows. This block mirrors ScopeEffectShaderData in hooking.h and
	// its static_assert -- change all three together or the buffer corrupts.
	int   EnableThermal;
	float nvNoise;
	float nvBloom;
	int   nvTint;           // 0 = green phosphor, 1 = white phosphor

	int   thermalPalette;   // 0 = white-hot, 1 = black-hot, 2 = ironbow
	float thermalContrast;
	float thermalEdge;
	float visionTime;       // seconds, animates NV scintillation
};

// Vision sources, published by the game thread each frame while a thermal or
// night-vision scope is active. Each entry is one projected object; two
// channels drive the two modes:
//   sourceGeo[i]   : xy = normalized screen pos [0,1], z = radius (norm-Y),
//                    w = thermal strength (warm bodies + fire).
//   sourceLight[i] : x = light strength (NV bloom on any emitter),
//                    y = warmth [0,1], zw reserved.
// sourceCount is how many of the 32 slots are live. b6 is a MagnaScope-owned
// register; when the buffer is unbound or sourceCount is 0 both fields sum to
// nothing, so an absent publication fails safe rather than crashing.
cbuffer HeatSourceData : register(b6)
{
	int   sourceCount;
	float3 _srcPad;
	float4 sourceGeo[32];
	float4 sourceLight[32];
};


SamplerState gSamLinear : register(s0);
SamplerState gSamReticle : register(s1);
Texture2D tBACKBUFFER : register(t4);
Texture2D ReticleTex : register(t5);
// User-supplied reticle texture for the reticle composite. Unbound samples
// transparent black in D3D11, and SCOPE_CUSTOM_RETICLE_INDEX is held negative
// until a texture is actually resident, so an unbound slot is never read.
Texture2D CustomReticleTex : register(t6);
// Heat mask (Stage 2): R8 actor-silhouette coverage, rendered from the actual
// character geometry and depth-occluded. Sampled in source-UV space so it
// tracks the magnified image.
Texture2D tHeatMask : register(t7);


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

// ============================================================================
// MagnaScope vision modes for the MAGNIFY pipeline (ScopeGeometryMagnify_PS).
// These are separate from the legacy NVGEffect above so the legacy screen-space
// shaders keep rendering exactly as before. All functions are pure (parameters
// passed explicitly) except MS_SampleHeatField, which reads the b6 heat buffer.
//
// Two coordinate spaces are in play and must not be confused:
//   outputUv  - the pixel being written (sensor/display space). NV grain lives
//               here because it is a property of the intensifier tube, not the
//               scene.
//   sourceUv  - the un-magnified scene coordinate this output pixel samples
//               from (the magnify shader's sampleUv). Heat blobs and thermal
//               edges live here so they line up with the magnified sight
//               picture without any magnification math: a blob authored in
//               source space is magnified together with the object it marks.
// ============================================================================

float MS_Hash21(float2 p)
{
	p = frac(p * float2(123.34f, 345.45f));
	p += dot(p, p + 34.345f);
	return frac(p.x * p.y);
}

// Realistic image-intensifier night vision. Amplifies available light with a
// saturating photon-gain curve, lifts shadows, blooms bright sources toward the
// phosphor ceiling, adds scintillation grain that is loudest where the signal
// is weakest (as a real tube is), and tints to a phosphor colour.
float3 MS_ApplyNightVision(
	float3 color,
	float2 outputUv,
	float gain,
	float noiseAmount,
	float bloom,
	float tintMode,
	float timeSeconds,
	float lightField)
{
	const float lum = dot(color, float3(0.2126f, 0.7152f, 0.0722f));
	const float g = max(gain, 0.0f);
	// Saturating gain: lots of amplification near black, compressive up top so
	// bright sources roll into the tube's ceiling instead of hard-clipping.
	float amplified = saturate(1.0f - exp(-lum * g));
	amplified = pow(amplified, 0.75f);  // lift shadows / midtones (base >= 0)
	// Emitter bloom: real light sources (lamps, torches, fire, muzzle flashes),
	// projected as light-field blobs, drive the tube toward its ceiling so they
	// glow through night vision the way a bright source overloads an intensifier.
	// Evaluated in source space by the caller, so the glow tracks the magnified
	// image. saturate keeps a stack of overlapping emitters from ringing.
	amplified = saturate(amplified + saturate(lightField));
	// Tonal bloom: bright pixels overshoot toward the ceiling (a spatial spread
	// would have to resample the un-magnified source and misalign, so the halo
	// is kept tonal here).
	const float halo = saturate(amplified - 0.6f) * bloom;
	amplified = saturate(amplified + halo);
	// Scintillation grain, animated so it shimmers; louder in the dark.
	const float n = MS_Hash21(
		outputUv * float2(BUFFER_WIDTH, BUFFER_HEIGHT) +
		frac(timeSeconds) * 311.7f);
	const float grain = noiseAmount * (0.15f + 0.85f * (1.0f - amplified));
	amplified = saturate(amplified + (n - 0.5f) * grain);
	const float3 greenPhosphor = float3(0.10f, 1.00f, 0.20f);
	const float3 whitePhosphor = float3(0.86f, 0.93f, 1.00f);
	const float3 tint = lerp(greenPhosphor, whitePhosphor, saturate(tintMode));
	return amplified * tint;
}

// Accumulated thermal + light strength at a source-space coordinate from the
// published vision sources. Each blob is a round Gaussian in screen pixels
// (centre normalized per-axis, radius normalized by height), so it stays
// circular at any aspect ratio. Returns float2(thermal, light). sourceCount 0
// (or an unbound b6) returns 0 -> cold and unlit.
float2 MS_SampleVisionField(float2 sourceUv)
{
	float2 field = float2(0.0f, 0.0f);  // x = thermal, y = light
	const float2 screen = float2(BUFFER_WIDTH, BUFFER_HEIGHT);
	[loop]
	// min(sourceCount, 32) is defense-in-depth: the CPU side already clamps the
	// published count to kMaxHeatSources in both the writer and the snapshot
	// reader, but bounding the loop here means a bad count can never index past
	// the 32-slot arrays even if that contract is ever broken.
	for (int i = 0; i < min(sourceCount, 32); ++i) {
		const float4 g = sourceGeo[i];
		const float2 deltaPixels = (sourceUv - g.xy) * screen;
		const float radiusPixels = max(g.z * BUFFER_HEIGHT, 1.0f);
		const float falloff =
			exp(-dot(deltaPixels, deltaPixels) /
				(radiusPixels * radiusPixels));
		field.x += falloff * g.w;               // thermal strength
		field.y += falloff * sourceLight[i].x;  // light strength (NV bloom)
	}
	return field;
}

float3 MS_ThermalPalette(float t, int palette)
{
	t = saturate(t);
	if (palette == 1) {
		return (1.0f - t).xxx;  // black-hot
	}
	if (palette == 2) {
		// Red-hot: cold near-black warming through deep red, orange and yellow
		// to a white core. Cool scene stays dark so warm bodies read as fire.
		float3 c = lerp(
			float3(0.02f, 0.0f, 0.02f), float3(0.55f, 0.0f, 0.0f),
			saturate(t / 0.40f));
		c = lerp(c, float3(1.00f, 0.35f, 0.0f), saturate((t - 0.40f) / 0.30f));
		c = lerp(c, float3(1.00f, 0.90f, 0.20f), saturate((t - 0.70f) / 0.20f));
		c = lerp(c, float3(1.00f, 1.00f, 1.00f), saturate((t - 0.90f) / 0.10f));
		return c;
	}
	if (palette == 3) {
		// Rainbow: blue cold -> cyan -> green -> yellow -> red hot. The whole
		// scene is tinted by temperature, which reads unmistakably as thermal
		// even where the only signal is the dim ambient gradient.
		float3 c = lerp(
			float3(0.0f, 0.0f, 0.55f), float3(0.0f, 0.65f, 1.0f),
			saturate(t / 0.25f));
		c = lerp(c, float3(0.0f, 1.0f, 0.30f), saturate((t - 0.25f) / 0.25f));
		c = lerp(c, float3(1.0f, 1.0f, 0.0f), saturate((t - 0.50f) / 0.25f));
		c = lerp(c, float3(1.0f, 0.0f, 0.0f), saturate((t - 0.75f) / 0.25f));
		return c;
	}
	return t.xxx;  // white-hot (default)
}

// Route B pseudo-thermal. A dim "cold" world from compressed scene luminance,
// crisp silhouette edges, and the real heat sources on top, mapped through a
// thermal palette. Not physically accurate (a bright lamp reads warm), but the
// warm blobs come from genuine NPC / heat positions rather than luminance.
float3 MS_ApplyThermal(
	float3 color,
	float2 sourceUv,
	int palette,
	float contrast,
	float edgeStrength,
	float heat)
{
	// Value model from STALKER 3DSS/HeatVision (thermal_utils.h): the scene
	// never goes black -- it sits in a narrow cold band and warm objects read
	// hot. Without G-buffer normals we approximate scene structure from the
	// backbuffer luminance, compressed hard into the cold band so a bright
	// surface reads only slightly warmer (not "bright = hot") and a pitch-dark
	// night still shows a flat cold field instead of the black we used to get.
	const float COLD_MIN = 0.24f;
	const float COLD_MAX = 0.40f;
	const float HOT = 0.85f;
	const float3 luma = float3(0.2126f, 0.7152f, 0.0722f);
	const float sceneLum = max(dot(color, luma), 0.0f);
	// sqrt lifts near-black night detail without letting bright day surfaces
	// dominate; the result only spans the cold band.
	const float structure = saturate(sqrt(sceneLum) * 0.9f);
	float cold = lerp(COLD_MIN, COLD_MAX, structure);
	// Subtle silhouette edge from source-space luma, so it tracks the image.
	const float2 px = PixelSize;
	const float lumRight = dot(
		tBACKBUFFER.SampleLevel(
			gSamLinear, saturate(sourceUv + float2(px.x, 0.0f)), 0.0f).rgb,
		luma);
	const float lumDown = dot(
		tBACKBUFFER.SampleLevel(
			gSamLinear, saturate(sourceUv + float2(0.0f, px.y)), 0.0f).rgb,
		luma);
	cold += (abs(sceneLum - lumRight) + abs(sceneLum - lumDown)) * edgeStrength;
	// Actor bodies come from the heat mask: their true, depth-occluded
	// silhouettes rendered from the actual character geometry (Stage 3
	// replaces the old actor circles). The blob field remains the source for
	// fire, placed lights and the sun. Bilinear sampling softens the body
	// edge by a pixel; 1.4 lands a body at HOT with a push toward white.
	const float bodyHeat =
		tHeatMask.SampleLevel(gSamLinear, sourceUv, 0.0f).r * 1.4f;
	const float heatSignal = max(heat, bodyHeat);
	// Warm sources drive the pixel from the cold band up to HOT; a strong
	// core (heatSignal > 1) pushes on toward white so a body or fire glows.
	float t = lerp(cold, HOT, saturate(heatSignal));
	t = lerp(t, 1.0f, saturate(heatSignal - 1.0f));
	// Contrast pivots around the cold midpoint, so raising it separates hot
	// from cold instead of darkening the whole field toward black.
	const float pivot = 0.30f;
	t = saturate((t - pivot) * max(contrast, 0.0001f) + pivot);
	return MS_ThermalPalette(t, palette);
}

// Selects the active vision mode and blends it in by ADS activation so 1x is
// untouched and the ramp has no pop. Thermal wins if both are somehow set.
float3 MS_ApplyVisionMode(
	float3 color,
	float2 outputUv,
	float2 sourceUv,
	int enableThermal,
	int enableNightVision,
	float gain,
	float noiseAmount,
	float bloom,
	float tintMode,
	int thermalPalette,
	float thermalContrast,
	float thermalEdge,
	float timeSeconds,
	float activation)
{
	// Single exit with an initialized result, so the compiler never sees a
	// path that could return an uninitialized value (fxc X4000).
	float3 result = color;
	// The vision field is sampled inside whichever branch runs, so both-off
	// stays a true pass-through that pays for no blob loop.
	if (enableThermal != 0) {
		const float2 visionField = MS_SampleVisionField(sourceUv);
		result = lerp(
			color,
			MS_ApplyThermal(
				color, sourceUv, thermalPalette, thermalContrast, thermalEdge,
				visionField.x),
			activation);
	} else if (enableNightVision != 0) {
		const float2 visionField = MS_SampleVisionField(sourceUv);
		result = lerp(
			color,
			MS_ApplyNightVision(
				color, outputUv, gain, noiseAmount, bloom, tintMode,
				timeSeconds, visionField.y),
			activation);
	}
	return result;
}
