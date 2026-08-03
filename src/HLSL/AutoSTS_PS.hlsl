#include "Triangle.hlsli"

// Automatic STS scopes have no marker mesh or captured draw call. This
// screen space pass derives its lens mask from the projected STS scene node.
float4 main(float4 position : SV_Position, float2 texcoord : TEXCOORD0) : SV_Target
{
	const float2 pixelSize = PixelSize;
	const float aspect = AspectRatio;
	const float referenceScale = BUFFER_HEIGHT / 1080.0;

	// ScopeScreenPos is stored in physical pixels by the CPU. Rejected
	// projections fall back to screen center, matching STS's authored ADS
	// alignment without allowing a NaN or offscreen mask.
	const float2 projectedCenter = ScopeScreenPos * pixelSize;
	const bool validProjection =
		all(isfinite(projectedCenter)) &&
		all(projectedCenter >= float2(0.0, 0.0)) &&
		all(projectedCenter <= float2(1.0, 1.0));
	const float2 baseCenter = validProjection ? projectedCenter : float2(0.5, 0.5);
	const float2 scopeCenter =
		baseCenter + ScopeEffect_Offset * pixelSize * referenceScale;

	const float diameterPixels = max(ScopeEffect_Size.x * referenceScale, 1.0);
	const float radiusUV = diameterPixels * 0.5 / BUFFER_HEIGHT;
	const float2 centered = texcoord - scopeCenter;
	const float2 centeredAspect = centered * float2(aspect, 1.0);
	const float normalizedRadius =
		length(centeredAspect) / max(radiusUV, 0.0001);

	const float2 halfExtents =
		float2(diameterPixels * 0.5 / BUFFER_WIDTH, radiusUV);
	const bool insideCircle = normalizedRadius <= 1.0;
	const bool insideRectangle = all(abs(centered) <= halfExtents);
	const bool insideLens = isCircle != 0 ? insideCircle : insideRectangle;

	const float zoom = max(ScopeEffect_Zoom, 1.0);
	const float fishEye =
		1.0 + max(FishEyeStrength, 0.0) *
		pow(saturate(normalizedRadius), max(FishEyePower, 0.5));
	float2 sourceCoord =
		scopeCenter +
		centered / zoom * fishEye +
		ScopeEffect_OriPositionOffset * pixelSize * referenceScale;
	sourceCoord = saturate(sourceCoord);

	// Chromatic aberration is zero at the center and increases toward the rim.
	const float aberration =
		0.0015 * saturate(normalizedRadius) * (1080.0 / BUFFER_HEIGHT);
	float4 color = tBACKBUFFER.Sample(gSamLinear, sourceCoord);
	color.r = tBACKBUFFER.Sample(
		gSamLinear, saturate(sourceCoord - float2(aberration, 0.0))).r;
	color.b = tBACKBUFFER.Sample(
		gSamLinear, saturate(sourceCoord + float2(aberration, 0.0))).b;

	// Lens displacement from screen center represents off-axis eye position.
	// It shifts the exit pupil and produces eye-box shadow without depending
	// on the malformed legacy camera-matrix calculation.
	const float2 opticalOffset =
		(scopeCenter - float2(0.5, 0.5)) * float2(aspect, 1.0);
	const float2 pupilCenter =
		scopeCenter - opticalOffset * saturate(parallax_maxTravel);
	const float pupilRadius =
		length((texcoord - pupilCenter) * float2(aspect, 1.0)) /
		max(radiusUV, 0.0001);
	const float vignetteStart =
		saturate(1.0 - 1.0 / max(parallax_relativeFogRadius, 1.01));
	const float vignette =
		1.0 - smoothstep(vignetteStart, 1.0, normalizedRadius);
	const float eyeBox =
		1.0 - smoothstep(
			saturate(1.0 / max(parallax_Radius, 1.01)),
			1.0,
			pupilRadius);
	color.rgb *= saturate(vignette * eyeBox);

	const float4 nightVision = EnableNV * NVGEffect(color, texcoord);
	color = nightVision * nightVision.a + color * (1.0 - nightVision.a);

	// STS supplies its own world-space reticle, so automatic profiles do not
	// add the optional marker texture and cannot create a doubled reticle.
	color.a = (EnableMerge != 0 && insideLens) ? 1.0 : 0.0;
	return color;
}
