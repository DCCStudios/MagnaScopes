#pragma once

#include <algorithm>
#include <cmath>

namespace MagnaScope::EyeBoxRecentering
{
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
