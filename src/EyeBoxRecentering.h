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
		// Large finite offsets are ordinary rapid camera or weapon motion, not
		// lifecycle discontinuities. Once that motion settles, shorten the time
		// constant continuously so the pupil catches up quickly without ever
		// jumping to the center on a single frame.
		constexpr float kNearTimeConstant = 0.28F;
		constexpr float kFarTimeConstant = 0.12F;
		const float distanceProgress = std::clamp(
			(std::abs(normalizedDisplacement) - 0.75F) / (3.0F - 0.75F),
			0.0F,
			1.0F);
		const float easedDistance =
			distanceProgress * distanceProgress *
			(3.0F - 2.0F * distanceProgress);
		const float recenteringTimeConstant =
			kNearTimeConstant +
			(kFarTimeConstant - kNearTimeConstant) * easedDistance;
		const float settledBlend =
			1.0F -
			std::exp(
				-std::clamp(deltaSeconds, 0.0F, 0.05F) /
					recenteringTimeConstant);
		return settledBlend * (1.0F - easedMotion);
	}
}
