#pragma once

#include <algorithm>
#include <cmath>

namespace MagnaScope::EyeBoxRecentering
{
	struct ScreenEyeOffset
	{
		float x{ 0.0F };
		float y{ 0.0F };
		bool valid{ false };
	};

	struct AxialEyeRelief
	{
		float normalizedDelta{ 0.0F };
		bool valid{ false };
	};

	// ScopeFade meshes lie in their local X/Z plane, making local Y the
	// aperture's optical normal. Measuring only that component after the camera
	// has been transformed into ScopeFade space prevents a pure pan, pitch, or
	// weapon-inertia translation from masquerading as fore/aft eye motion.
	[[nodiscard]] inline float CalculateEyeReliefDistance(
		float cameraApertureLocalY,
		float apertureLocalCenterY) noexcept
	{
		if (!std::isfinite(cameraApertureLocalY) ||
			!std::isfinite(apertureLocalCenterY)) {
			return 0.0F;
		}
		return std::abs(cameraApertureLocalY - apertureLocalCenterY);
	}

	// Extracts fore/aft displacement from an eye-to-ScopeFade-plane distance
	// measured along ScopeFade's local Y normal. ScopeFade itself lies in local
	// X/Z, so this scalar is geometrically orthogonal to lateral eye travel.
	// The result is a signed fraction of the settled eye-to-aperture distance;
	// positive means the aperture moved farther from the camera. A conservative
	// bound prevents a scene-graph discontinuity from making the optical image
	// or exit pupil breathe beyond a plausible transient range.
	[[nodiscard]] inline AxialEyeRelief CalculateAxialEyeRelief(
		float currentEyeReliefDistance,
		float settledEyeReliefDistance) noexcept
	{
		AxialEyeRelief result{};
		if (!std::isfinite(currentEyeReliefDistance) ||
			!std::isfinite(settledEyeReliefDistance) ||
			currentEyeReliefDistance <= 0.001F ||
			settledEyeReliefDistance <= 0.001F) {
			return result;
		}

		constexpr float kMaximumDepthFraction = 0.10F;
		result.normalizedDelta = std::clamp(
			(currentEyeReliefDistance - settledEyeReliefDistance) /
				settledEyeReliefDistance,
			-kMaximumDepthFraction,
			kMaximumDepthFraction);
		result.valid = std::isfinite(result.normalizedDelta);
		if (!result.valid) {
			result.normalizedDelta = 0.0F;
		}
		return result;
	}

	// Normalizes physical aperture motion in render-target pixels without ever
	// rotating it through the optic's world-facing X/Z axes. The eye moves
	// opposite the optic, so the returned sign deliberately counteracts the
	// measured screen motion. Keeping this value in display X/Y through the
	// shader is what makes identical mouse motion behave identically at every
	// compass heading and camera pitch.
	[[nodiscard]] inline ScreenEyeOffset CalculateScreenEyeOffset(
		float deltaX,
		float deltaY,
		float apertureRadiusPixels) noexcept
	{
		ScreenEyeOffset result{};
		if (!std::isfinite(deltaX) || !std::isfinite(deltaY) ||
			!std::isfinite(apertureRadiusPixels) ||
			apertureRadiusPixels <= 0.01F) {
			return result;
		}

		result.x = -deltaX / apertureRadiusPixels;
		result.y = -deltaY / apertureRadiusPixels;
		result.valid = std::isfinite(result.x) && std::isfinite(result.y);
		if (!result.valid) {
			result.x = 0.0F;
			result.y = 0.0F;
		}
		return result;
	}

	// Converts camera motion measured in ScopeFade's current local X/Z plane
	// into the display-space convention consumed by the optics shaders.  The
	// local coordinates are stable across world heading and camera pitch: a
	// rigid camera/weapon rotation leaves them unchanged, while weapon inertia,
	// sway, and recoil move them relative to their settled baseline.  Projecting
	// through the current aperture basis then lets the shader recover the same
	// local displacement without ever comparing vectors from different frames.
	[[nodiscard]] inline ScreenEyeOffset ProjectLocalEyeOffsetToScreen(
		float currentLocalX,
		float currentLocalZ,
		float settledLocalX,
		float settledLocalZ,
		float localRadius,
		float basisXX,
		float basisXY,
		float basisZX,
		float basisZY,
		float projectedRadiusPixels) noexcept
	{
		ScreenEyeOffset result{};
		if (!std::isfinite(currentLocalX) ||
			!std::isfinite(currentLocalZ) ||
			!std::isfinite(settledLocalX) ||
			!std::isfinite(settledLocalZ) ||
			!std::isfinite(localRadius) ||
			!std::isfinite(basisXX) ||
			!std::isfinite(basisXY) ||
			!std::isfinite(basisZX) ||
			!std::isfinite(basisZY) ||
			!std::isfinite(projectedRadiusPixels) ||
			localRadius <= 0.001F ||
			projectedRadiusPixels <= 0.01F) {
			return result;
		}

		const float localX =
			(currentLocalX - settledLocalX) / localRadius;
		const float localZ =
			(currentLocalZ - settledLocalZ) / localRadius;
		const float pixelX = localX * basisXX + localZ * basisZX;
		const float pixelY = localX * basisXY + localZ * basisZY;
		result.x = pixelX / projectedRadiusPixels;
		result.y = pixelY / projectedRadiusPixels;
		result.valid = std::isfinite(result.x) && std::isfinite(result.y);
		if (!result.valid) {
			result.x = 0.0F;
			result.y = 0.0F;
		}
		return result;
	}

	// Smoothly maps [0, inf) onto [0, limit). This is the exact C++ mirror of
	// ScopeShadowSoftLimit in src/HLSL/ScopeShadow.hlsli: the game thread and
	// the optics shaders must agree on how travel saturates, otherwise the
	// published value and the rendered crescent disagree during fast motion.
	// A hard clamp was previously used here, which introduced a derivative
	// discontinuity exactly when the shadow was most visible.
	[[nodiscard]] inline float SoftLimit(
		float value,
		float limit) noexcept
	{
		if (!std::isfinite(value) || !std::isfinite(limit)) {
			return 0.0F;
		}
		const float safeLimit = std::max(limit, 0.000001F);
		const float normalized = value / safeLimit;
		const float scaled =
			value / std::sqrt(1.0F + normalized * normalized);
		return std::isfinite(scaled) ? scaled : 0.0F;
	}

	// Applies SoftLimit to a two-dimensional travel vector without changing
	// its direction.
	inline void SoftLimitVector(
		float& x,
		float& y,
		float limit) noexcept
	{
		if (!std::isfinite(x) || !std::isfinite(y)) {
			x = 0.0F;
			y = 0.0F;
			return;
		}
		const float magnitude = std::sqrt(x * x + y * y);
		if (magnitude <= 0.000001F) {
			return;
		}
		const float scale = SoftLimit(magnitude, limit) / magnitude;
		x *= scale;
		y *= scale;
	}

	// Converts a per-frame camera-rotation screen impulse, already normalized
	// by the projected aperture radius, into eye-box travel.
	//
	// The raw ratio is not a usable eye displacement. Accumulated through the
	// angular-lag decay it settles at approximately rate * timeConstant, so a
	// brisk 90 deg/s pan through a magnified optic produced two to three
	// aperture radii of "eye" travel. A real shooter's eye lags the ocular by
	// a fraction of its diameter, and travel beyond roughly one radius carries
	// no additional optical information because the exit pupil has already
	// reached its bounded displacement.
	//
	// This gain places a brisk 90 deg/s ADS pan near three quarters of a radius.
	// Through the authored defaults (eye relief 1.0 over exit-pupil forgiveness
	// 1.55) that is roughly half a radius of pupil displacement, which is a
	// clearly visible one-sided crescent rather than a faint edge darkening.
	// Choosing a responsive value is safe only because ScopeShadow.hlsli now
	// soft-limits the displacement below the radius at which the aligned lens
	// centre would leave the lit disc; the raw ratio alone settles past two
	// radii and used to be bounded by nothing.
	inline constexpr float kAngularLagGain = 0.35F;

	// Lateral and vertical camera translation, converted to aperture radii and
	// fed to the same accumulator as the angular impulse above.
	//
	// Strafing produces no angular signal at all: the camera and everything it
	// sees translate together, so a distant reference point keeps its screen
	// position and the rotation term reads zero. Only weapon animation inertia
	// leaks through, which is far too little to register as lens lag.
	//
	// This gain is much smaller than the angular one because its input is much
	// larger. A walk is roughly a hundred game units per second against an
	// aperture radius near three, so an unscaled ratio would contribute half a
	// radius every frame and saturate instantly. At this value a walk is a
	// suggestion, a sprint is clearly visible, and Lens Lag still scales the
	// result the same way it scales a pan.
	inline constexpr float kTranslationLagGain = 0.05F;

	// Returns the frame-rate independent fraction used by a stateful optical
	// output to approach a new target. Unlike assigning the target directly,
	// this response never overshoots and never jumps when the measured eye pose
	// changes between valid frames.
	[[nodiscard]] inline float CalculateResponseAlpha(
		float deltaSeconds,
		float timeConstant) noexcept
	{
		if (!std::isfinite(deltaSeconds) ||
			!std::isfinite(timeConstant) ||
			deltaSeconds <= 0.0F ||
			timeConstant <= 0.0F) {
			return 0.0F;
		}

		return 1.0F -
			std::exp(
				-std::clamp(deltaSeconds, 0.0F, 0.05F) /
				timeConstant);
	}

	// Returns the fraction by which a settled eye/optic baseline should move
	// toward the current sample this frame. Motion is expressed in aperture
	// radii per second, making the response independent of scope size and
	// frame rate. Fast recoil/inertia freezes the baseline; once motion stops,
	// the baseline catches up and the pupil returns to the authored center.
	[[nodiscard]] inline float CalculateBlend(
		float normalizedVelocity,
		float deltaSeconds,
		float normalizedDisplacement = 0.0F) noexcept
	{
		if (!std::isfinite(normalizedVelocity) ||
			!std::isfinite(deltaSeconds) ||
			!std::isfinite(normalizedDisplacement) ||
			deltaSeconds <= 0.0F) {
			return 0.0F;
		}

		constexpr float kSettledVelocity = 0.15F;
		constexpr float kMovingVelocity = 1.25F;
		const float motionProgress = std::clamp(
			(normalizedVelocity - kSettledVelocity) /
				(kMovingVelocity - kSettledVelocity),
			0.0F,
			1.0F);
		const float easedMotion =
			motionProgress * motionProgress *
			(3.0F - 2.0F * motionProgress);
		// Use one response constant for every finite displacement. Accelerating
		// large offsets made the first settled frame absorb a visibly larger
		// portion of the motion, which presented as a snap after a sharp drag.
		// The displacement remains an input validation signal for callers, but it
		// must not change the response curve.
		(void)normalizedDisplacement;
		constexpr float kRecenteringTimeConstant = 0.18F;
		const float settledBlend =
			1.0F -
			std::exp(
				-std::clamp(deltaSeconds, 0.0F, 0.05F) /
				kRecenteringTimeConstant);
		return settledBlend * (1.0F - easedMotion);
	}
}
