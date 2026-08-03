#include "Triangle.hlsli"

float4 main(float4 vpos : SV_Position, float2 texcoord : TEXCOORD0) : SV_Target
{
	const float2 screenSize = float2(BUFFER_WIDTH, BUFFER_HEIGHT);
	const float referenceScale = BUFFER_HEIGHT / 1080.0;

	// ScopeScreenPos is the projected mesh anchor in pixels. Old profiles that
	// produce no usable anchor retain the historical screen-center behavior.
	float2 projectedCenter = ScopeScreenPos * PixelSize;
	const bool validProjectedCenter =
		all(projectedCenter > float2(0.001, 0.001)) &&
		all(projectedCenter < float2(0.999, 0.999));
	float2 scopeCenter = validProjectedCenter ? projectedCenter : float2(0.5, 0.5);

	const float4 viewDirection = normalize(mul(float4(eyeDirection, 1), CameraRotation));
	float2 viewOffset = viewDirection.xy * rcp(max(camDepth, 0.01));
	viewOffset.y *= -AspectRatio;
	scopeCenter += viewOffset + ScopeEffect_Offset * PixelSize * referenceScale;

	const float distanceWeapon = 0.5 * distance(CurrWeaponPos, CurrRootPos);
	const float zMove = abs(sqrt(abs(BaseWeaponPos)) - distanceWeapon) + 1.0;
	float zScale = lerp(1.0, rcp(zMove), EnableZMove * MovePercentage);
	zScale = clamp(zScale, 0.5, 1.5);

	const float diameterPixels = max(ScopeEffect_Size.x, 1.0) * referenceScale * rcp(zScale);
	const float radiusUV = diameterPixels * 0.5 / BUFFER_HEIGHT;
	float2 centeredUV = texcoord - scopeCenter;
	float2 circularDelta = centeredUV * float2(AspectRatio, 1.0);
	const bool insideCircle = dot(circularDelta, circularDelta) < radiusUV * radiusUV;
	const float2 halfExtents = float2(diameterPixels * 0.5 / BUFFER_WIDTH, radiusUV);
	const bool insideRectangle = all(abs(centeredUV) <= halfExtents);
	const bool isRender = isCircle ? insideCircle : insideRectangle;

	const float zoom = max(ScopeEffect_Zoom, 1.0);

	// MW2019-style fisheye: push the sample point outward as it approaches
	// the lens edge, which optically compresses and bends the image there
	// while the center of the lens stays flat. rNorm is 0 at the scope
	// center and 1 at the edge of the circle.
	const float rNorm = saturate(length(circularDelta) * rcp(max(radiusUV, 0.0001)));
	const float fishEye = 1.0 + max(FishEyeStrength, 0.0) * pow(rNorm, max(FishEyePower, 0.5));

	float2 sourceCoord =
		scopeCenter +
		centeredUV * rcp(zoom) * fishEye +
		ScopeEffect_OriPositionOffset * PixelSize * referenceScale;
	sourceCoord = saturate(sourceCoord);

	float4 eyeVelocity = mul(float4(eyeDirectionLerp, 1), CameraRotation);
	float2 parallaxCenter = scopeCenter + clampMagnitude(eyeVelocity.xy, 2) * PixelSize;
	const float parallaxDistance = distance(
		aspect_ratio_correction(texcoord),
		aspect_ratio_correction(parallaxCenter));
	const float centerDistance = distance(
		aspect_ratio_correction(texcoord),
		aspect_ratio_correction(scopeCenter));

	const float chromaticOffset = 0.008 * (1080.0 / BUFFER_HEIGHT);
	float4 color = tBACKBUFFER.Sample(gSamLinear, sourceCoord);
	color.r = tBACKBUFFER.Sample(
		gSamLinear,
		sourceCoord + float2(-chromaticOffset * centerDistance, 0)).r;
	color.b = tBACKBUFFER.Sample(
		gSamLinear,
		sourceCoord + float2(chromaticOffset * centerDistance, 0)).b;

	const float4 nightVision = EnableNV * NVGEffect(color, texcoord);
	color = nightVision * nightVision.a + color * (1 - nightVision.a);
	color.rgb *= getparallax(parallaxDistance, float2(referenceScale, referenceScale), 1.0);

	const float2 reticleCoord =
		float2(0.5, 0.5) +
		centeredUV * float2(AspectRatio, 1.0) * 16.0 / max(ReticleSize, 0.01);
	const float4 reticle = ReticleTex.Sample(gSamLinear, reticleCoord);
	color = reticle * reticle.a + color * (1 - reticle.a);

	color.a *= EnableMerge * isRender;
	return color;
}

