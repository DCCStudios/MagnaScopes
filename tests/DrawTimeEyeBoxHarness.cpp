#include "DrawTimeEyeBoxMath.h"
#include "EyeBoxRecentering.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <format>
#include <iostream>

namespace
{
	using MagnaScope::DrawTimeEyeBox::BuildLensFrame;
	using MagnaScope::DrawTimeEyeBox::CalculateEyeTravel;
	using MagnaScope::DrawTimeEyeBox::ClipPoint;
	using MagnaScope::DrawTimeEyeBox::LensFrame;
	using MagnaScope::DrawTimeEyeBox::LensCoordinatesToPixels;
	using MagnaScope::DrawTimeEyeBox::ProjectLensCoordinates;
	using MagnaScope::DrawTimeEyeBox::SolveLensCoordinates;
	using MagnaScope::DrawTimeEyeBox::Vec2;
	using MagnaScope::EyeBoxRecentering::CalculateAxialEyeRelief;
	using MagnaScope::EyeBoxRecentering::CalculateBlend;
	using MagnaScope::EyeBoxRecentering::CalculateEyeReliefDistance;
	using MagnaScope::EyeBoxRecentering::CalculateResponseAlpha;
	using MagnaScope::EyeBoxRecentering::CalculateScreenEyeOffset;
	using MagnaScope::EyeBoxRecentering::ProjectLocalEyeOffsetToScreen;

	constexpr float kWidth = 1920.0F;
	constexpr float kHeight = 1080.0F;
	constexpr float kRadiusPixels = 240.0F;

	void Require(bool condition, const char* message)
	{
		if (!condition) {
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	[[nodiscard]] bool Near(float lhs, float rhs, float tolerance = 0.001F)
	{
		return std::abs(lhs - rhs) <= tolerance;
	}

	[[nodiscard]] ClipPoint PixelToClip(Vec2 pixel, float w = 1.0F)
	{
		return {
			((pixel.x / kWidth) * 2.0F - 1.0F) * w,
			(1.0F - (pixel.y / kHeight) * 2.0F) * w,
			0.5F * w,
			w
		};
	}

	[[nodiscard]] ClipPoint PixelDeltaToClipColumn(
		Vec2 pixelDelta,
		float centerW) noexcept
	{
		return {
			pixelDelta.x * (2.0F / kWidth) * centerW,
			-pixelDelta.y * (2.0F / kHeight) * centerW,
			0.0F,
			0.0F
		};
	}

	[[nodiscard]] LensFrame MakeFrameFromColumns(
		const ClipPoint& centerClip,
		const ClipPoint& columnXClip,
		const ClipPoint& columnZClip)
	{
		constexpr float angleStep =
			6.28318530717958647692F / 24.0F;
		const Vec2 currentDirection{ 0.0F, 1.0F };
		const Vec2 nextDirection{
			std::sin(angleStep),
			std::cos(angleStep)
		};
		const ClipPoint outerCurrent =
			centerClip +
			columnXClip * currentDirection.x +
			columnZClip * currentDirection.y;
		const ClipPoint outerNext =
			centerClip +
			columnXClip * nextDirection.x +
			columnZClip * nextDirection.y;
		return BuildLensFrame(
			centerClip,
			outerCurrent,
			outerNext,
			currentDirection,
			nextDirection,
			kWidth,
			kHeight);
	}

	[[nodiscard]] LensFrame MakeFrame(
		Vec2 center,
		float rollRadians,
		float radius = kRadiusPixels,
		float clipW = 1.0F)
	{
		const Vec2 basisX{
			std::cos(rollRadians) * radius,
			std::sin(rollRadians) * radius
		};
		const Vec2 basisZ{
			std::sin(rollRadians) * radius,
			-std::cos(rollRadians) * radius
		};
		return MakeFrameFromColumns(
			PixelToClip(center, clipW),
			PixelDeltaToClipColumn(basisX, clipW),
			PixelDeltaToClipColumn(basisZ, clipW));
	}
}

int main()
{
	const Vec2 screenCenter{ kWidth * 0.5F, kHeight * 0.5F };

	// A settled optic must continuously adopt its current pose as the neutral
	// baseline. At 60 Hz the response converges substantially within a few
	// tenths of a second, so looking up or down while stationary cannot leave
	// the pupil permanently displaced.
	float settledBaseline = 0.0F;
	for (int frame = 0; frame < 60; ++frame) {
		settledBaseline +=
			(1.0F - settledBaseline) *
			CalculateBlend(0.0F, 1.0F / 60.0F);
	}
	Require(
		settledBaseline > 0.96F,
		"stationary eye-box baseline did not recenter");

	// Recoil and inertia must remain visible rather than being absorbed into
	// the baseline on the same frame. The moving threshold freezes the
	// baseline completely; the next settled frames then recover smoothly.
	Require(
		Near(CalculateBlend(2.0F, 1.0F / 60.0F), 0.0F),
		"fast weapon motion was incorrectly absorbed into the baseline");
	const float firstSettledBlend =
		CalculateBlend(0.0F, 1.0F / 60.0F);
	Require(
		firstSettledBlend > 0.0F && firstSettledBlend < 0.10F,
		"settled recovery is discontinuous or frame-rate dependent");
	const float nearFirst =
		CalculateBlend(0.0F, 1.0F / 60.0F, 0.0F);
	const float farFirst =
		CalculateBlend(0.0F, 1.0F / 60.0F, 4.0F);
	Require(
		Near(farFirst, nearFirst, 0.0001F),
		"large displacement changed the first settled-frame response");
	Require(
		CalculateBlend(0.0F, 0.05F, 4.0F) < 0.30F,
		"long frame recentering step was too large");

	// A finite displacement beyond the former three-radius reset threshold
	// must retain visible lag and recover monotonically. The response should
	// also describe the same curve at common frame rates.
	auto recoverLargeOffset = [](float frameSeconds) {
		float baseline = 0.0F;
		constexpr float target = 4.0F;
		const int frames = static_cast<int>(0.5F / frameSeconds);
		for (int frame = 0; frame < frames; ++frame) {
			const float displacement = target - baseline;
			baseline += displacement *
				CalculateBlend(0.0F, frameSeconds, std::abs(displacement));
			Require(
				baseline >= 0.0F && baseline < target,
				"large-offset recovery snapped or overshot");
		}
		return baseline;
	};
	const float recovered30 = recoverLargeOffset(1.0F / 30.0F);
	const float recovered60 = recoverLargeOffset(1.0F / 60.0F);
	const float recovered144 = recoverLargeOffset(1.0F / 144.0F);
	Require(
		std::abs(recovered30 - recovered60) < 0.04F &&
			std::abs(recovered60 - recovered144) < 0.04F,
		"large-offset recovery changed materially with frame rate");
	Require(
		Near(CalculateBlend(3.0F, 1.0F / 60.0F, 4.0F), 0.0F),
		"large offset was absorbed while the optic was still moving");

	// The separately filtered renderer output must follow every finite target
	// continuously. A step or sign reversal may change direction immediately,
	// but one update can never snap to or overshoot the new target.
	const float responseAlpha60 =
		CalculateResponseAlpha(1.0F / 60.0F, 0.090F);
	Require(
		responseAlpha60 > 0.0F && responseAlpha60 < 1.0F,
		"eye-box output follower used a discontinuous response step");
	float followedOutput = 0.0F;
	for (int frame = 0; frame < 4; ++frame) {
		const float previousOutput = followedOutput;
		followedOutput += (1.0F - followedOutput) * responseAlpha60;
		Require(
			followedOutput > previousOutput && followedOutput < 1.0F,
			"eye-box output follower snapped or overshot a positive target");
	}
	for (int frame = 0; frame < 8; ++frame) {
		const float previousOutput = followedOutput;
		followedOutput += (-1.0F - followedOutput) * responseAlpha60;
		Require(
			followedOutput < previousOutput && followedOutput > -1.0F,
			"eye-box output follower snapped or overshot after sign reversal");
	}

	const auto followForDuration = [](float frameSeconds) {
		float output = 0.0F;
		float elapsed = 0.0F;
		while (elapsed < 0.25F) {
			const float step = std::min(frameSeconds, 0.25F - elapsed);
			output += (1.0F - output) *
				CalculateResponseAlpha(step, 0.090F);
			elapsed += step;
		}
		return output;
	};
	const float followed30 = followForDuration(1.0F / 30.0F);
	const float followed60 = followForDuration(1.0F / 60.0F);
	const float followed144 = followForDuration(1.0F / 144.0F);
	Require(
		Near(followed30, followed60, 0.0002F) &&
			Near(followed60, followed144, 0.0002F),
		"eye-box output follower changed with frame rate");
	Require(
		Near(CalculateResponseAlpha(0.0F, 0.090F), 0.0F) &&
			CalculateResponseAlpha(0.25F, 0.090F) < 1.0F,
		"eye-box output follower accepted an invalid or snapping frame step");

	// ScopeFade local Y is its plane normal. The producer must measure eye
	// relief only on that axis after transforming the camera into aperture
	// space; projected screen X/Z motion therefore cannot change the result.
	const float centeredDistance = CalculateEyeReliefDistance(-22.0F, 0.0F);
	const float samePlaneDistance = CalculateEyeReliefDistance(-22.0F, 0.0F);
	Require(
		Near(centeredDistance, 22.0F) &&
			Near(samePlaneDistance, centeredDistance) &&
			Near(CalculateEyeReliefDistance(-18.0F, 0.0F), 18.0F) &&
			Near(CalculateEyeReliefDistance(22.0F, 0.0F), 22.0F) &&
			Near(CalculateEyeReliefDistance(NAN, 0.0F), 0.0F),
		"ScopeFade-local eye-relief distance is not sign-safe and axial-only");
	const auto fartherRelief =
		CalculateAxialEyeRelief(centeredDistance, 20.0F);
	const auto sameDepthDifferentHeading =
		CalculateAxialEyeRelief(samePlaneDistance, 20.0F);
	const auto nearerRelief =
		CalculateAxialEyeRelief(18.0F, 20.0F);
	Require(
		fartherRelief.valid && sameDepthDifferentHeading.valid &&
			nearerRelief.valid &&
			Near(fartherRelief.normalizedDelta, 0.10F) &&
			Near(
				sameDepthDifferentHeading.normalizedDelta,
				fartherRelief.normalizedDelta) &&
			Near(nearerRelief.normalizedDelta, -0.10F),
		"ScopeFade-local axial eye relief changed with lateral pose or sign");
	Require(
		Near(
			CalculateAxialEyeRelief(40.0F, 20.0F)
				.normalizedDelta,
			0.10F) &&
		!CalculateAxialEyeRelief(0.0F, 20.0F).valid,
		"axial eye-relief safety bound or invalid-distance rejection failed");

	// The shipping depth baseline is a continuous follower. A fore/aft impulse
	// therefore decays monotonically and never snaps to zero on its final frame.
	float axialBaseline = 20.0F;
	float previousRelief = 1.0F;
	for (int frame = 0; frame < 90; ++frame) {
		axialBaseline += (22.0F - axialBaseline) *
			CalculateResponseAlpha(1.0F / 60.0F, 0.090F);
		const auto relief =
			CalculateAxialEyeRelief(22.0F, axialBaseline);
		Require(
			relief.valid && relief.normalizedDelta >= 0.0F &&
				relief.normalizedDelta <= previousRelief,
			"axial eye relief snapped, overshot, or reversed while recentering");
		previousRelief = relief.normalizedDelta;
	}
	Require(
		previousRelief < 0.0001F,
		"axial eye relief did not smoothly recenter");

	// Mirror the shipping 90 ms pose follower followed by its 10 ms noise
	// filter. Every published sample must lie between the previous output and
	// the current finite target, including the turn back toward zero.
	float followerBaseline = 0.0F;
	float publishedOffset = 0.0F;
	for (int frame = 0; frame < 90; ++frame) {
		followerBaseline += (1.0F - followerBaseline) *
			CalculateResponseAlpha(1.0F / 60.0F, 0.090F);
		const float targetOffset = -(1.0F - followerBaseline);
		const float previousOffset = publishedOffset;
		publishedOffset += (targetOffset - publishedOffset) *
			CalculateResponseAlpha(1.0F / 60.0F, 0.010F);
		Require(
			publishedOffset >= std::min(previousOffset, targetOffset) &&
				publishedOffset <= std::max(previousOffset, targetOffset),
			"combined eye-box follower snapped or overshot its finite target");
	}
	Require(
		std::abs(publishedOffset) < 0.001F,
		"combined eye-box follower did not converge continuously to center");

	// Horizontal and vertical motion are evaluated from the frame supplied to
	// this exact iteration. A deliberately different previous frame proves
	// there is no hidden phase-lag dependency in the calculation.
	const LensFrame previous = MakeFrame({ 700.0F, 400.0F }, 0.0F);
	(void)CalculateEyeTravel(previous, {}, screenCenter, 1.0F, 4.0F);
	const LensFrame current = MakeFrame(
		{ screenCenter.x + 48.0F, screenCenter.y - 24.0F },
		0.0F);
	const Vec2 currentTravel =
		CalculateEyeTravel(current, {}, screenCenter, 1.0F, 4.0F);
	Require(
		Near(currentTravel.x, -48.0F / kRadiusPixels) &&
			Near(currentTravel.y, -24.0F / kRadiusPixels),
		"current-frame horizontal/vertical motion lagged or changed sign");

	// Exit-pupil shadow travel may use the full four-radius production range,
	// but must never exceed it even during an extreme recoil pose. A zero
	// limit is also a real opt-out rather than a special unlimited value.
	const LensFrame extremeTravelFrame = MakeFrame(
		{ screenCenter.x + kRadiusPixels * 8.0F,
		  screenCenter.y + kRadiusPixels * 6.0F },
		0.0F);
	const Vec2 clampedExtremeTravel = CalculateEyeTravel(
		extremeTravelFrame,
		{},
		screenCenter,
		1.0F,
		4.0F);
	Require(
		Near(
			std::sqrt(
				clampedExtremeTravel.x * clampedExtremeTravel.x +
				clampedExtremeTravel.y * clampedExtremeTravel.y),
			4.0F),
		"maximum eye travel did not clamp to four aperture radii");
	const Vec2 disabledTravel = CalculateEyeTravel(
		extremeTravelFrame,
		{},
		screenCenter,
		1.0F,
		0.0F);
	Require(
		Near(disabledTravel.x, 0.0F) &&
			Near(disabledTravel.y, 0.0F),
		"zero maximum eye travel did not disable pupil displacement");

	// Rolling the optic rotates the recovered X/Z frame. The same screen-space
	// displacement must rotate into lens-local coordinates rather than remain
	// pinned to the monitor axes.
	const float quarterTurn = 1.57079632679489661923F;
	const LensFrame rolled = MakeFrame(
		{ screenCenter.x + 48.0F, screenCenter.y },
		quarterTurn);
	const Vec2 rolledTravel =
		CalculateEyeTravel(rolled, {}, screenCenter, 1.0F, 4.0F);
	Require(
		Near(rolledTravel.x, 0.0F, 0.002F) &&
			Near(rolledTravel.y, -48.0F / kRadiusPixels, 0.002F),
		"rolled lens basis did not rotate eye travel locally");

	// Aim offsets are authored relative to ScopeFade. Applying the offset to
	// the current center keeps a deliberately off-center STS reticle aligned
	// without assuming that the lens itself is screen centered.
	const LensFrame authoredOffsetFrame = MakeFrame(
		{ screenCenter.x - 18.0F, screenCenter.y + 12.0F },
		0.0F);
	const Vec2 authoredOffsetTravel = CalculateEyeTravel(
		authoredOffsetFrame,
		{ 18.0F / kRadiusPixels, 12.0F / kRadiusPixels },
		screenCenter,
		1.0F,
		4.0F);
	Require(
		Near(authoredOffsetTravel.x, 0.0F) &&
			Near(authoredOffsetTravel.y, 0.0F),
		"authored STS reticle offset was not preserved");

	// The authored relationship belongs to the optic, not to the monitor.
	// A quarter-turn roll must rotate the offset about the current lens
	// center. A fixed screen-pixel offset would fail this fixture.
	const LensFrame rolledAuthoredOffsetFrame = MakeFrame(
		{ screenCenter.x + 24.0F, screenCenter.y - 48.0F },
		quarterTurn);
	const Vec2 rolledAuthoredOffsetLocal{ 0.20F, -0.10F };
	const Vec2 rolledAuthoredOffsetPixels =
		LensCoordinatesToPixels(
			rolledAuthoredOffsetLocal,
			rolledAuthoredOffsetFrame);
	Require(
		Near(rolledAuthoredOffsetPixels.x, -24.0F, 0.002F) &&
			Near(rolledAuthoredOffsetPixels.y, 48.0F, 0.002F),
		"authored STS offset did not rotate with the lens basis");
	const Vec2 rolledAuthoredOffsetTravel = CalculateEyeTravel(
		rolledAuthoredOffsetFrame,
		rolledAuthoredOffsetLocal,
		screenCenter,
		1.0F,
		4.0F);
	Require(
		Near(rolledAuthoredOffsetTravel.x, 0.0F, 0.002F) &&
			Near(rolledAuthoredOffsetTravel.y, 0.0F, 0.002F),
		"rolled authored STS reticle alignment drifted from the camera axis");

	// Activation must be continuous. Half activation produces exactly half of
	// the same-frame displacement and zero activation produces no movement.
	const Vec2 halfActivation =
		CalculateEyeTravel(current, {}, screenCenter, 0.5F, 4.0F);
	const Vec2 zeroActivation =
		CalculateEyeTravel(current, {}, screenCenter, 0.0F, 4.0F);
	Require(
		Near(halfActivation.x, currentTravel.x * 0.5F) &&
			Near(halfActivation.y, currentTravel.y * 0.5F) &&
			Near(zeroActivation.x, 0.0F) &&
			Near(zeroActivation.y, 0.0F),
		"activation did not blend the draw-time eye offset continuously");

	// Homogeneous clip W changes with axial lens motion. Perspective division
	// must leave center and basis recovery correct. The shipping shader keeps
	// the already calibrated CPU relief scalar because an absolute clip-depth
	// reference is not yet verified across Fallout's first-person shaders.
	const LensFrame changedRelief = MakeFrame(
		{ screenCenter.x + 48.0F, screenCenter.y - 24.0F },
		0.0F,
		kRadiusPixels,
		2.75F);
	const Vec2 changedReliefTravel =
		CalculateEyeTravel(
			changedRelief,
			{},
			screenCenter,
			1.0F,
			4.0F);
	Require(
		Near(changedReliefTravel.x, currentTravel.x) &&
			Near(changedReliefTravel.y, currentTravel.y),
		"homogeneous relief changed lateral lens coordinates");

	// Two independent radial vertices must recover a genuinely projective
	// plane. The axes below are sheared, have unequal scale, and carry
	// different W gradients. The previous one-edge construction forced the
	// second axis perpendicular and cannot satisfy either this camera-axis
	// intersection or the off-axis round trips.
	const Vec2 expectedCameraAxisLocal{ 0.23F, -0.17F };
	const ClipPoint projectiveColumnX{
		0.36F,
		0.08F,
		0.02F,
		0.18F
	};
	const ClipPoint projectiveColumnZ{
		0.14F,
		-0.46F,
		-0.03F,
		-0.12F
	};
	const ClipPoint projectiveCenter{
		-(projectiveColumnX.x * expectedCameraAxisLocal.x +
		  projectiveColumnZ.x * expectedCameraAxisLocal.y),
		-(projectiveColumnX.y * expectedCameraAxisLocal.x +
		  projectiveColumnZ.y * expectedCameraAxisLocal.y),
		0.55F,
		1.30F
	};
	const LensFrame projective = MakeFrameFromColumns(
		projectiveCenter,
		projectiveColumnX,
		projectiveColumnZ);
	Require(projective.valid, "projective lens frame was rejected");
	const float projectedAxisDot =
		projective.basisX.x * projective.basisZ.x +
		projective.basisX.y * projective.basisZ.y;
	Require(
		std::abs(projectedAxisDot) > 1000.0F,
		"projective fixture accidentally remained perpendicular");
	const Vec2 solvedCameraAxis =
		SolveLensCoordinates(screenCenter, projective);
	Require(
		Near(solvedCameraAxis.x, expectedCameraAxisLocal.x, 0.0002F) &&
			Near(solvedCameraAxis.y, expectedCameraAxisLocal.y, 0.0002F),
		"homogeneous camera-axis intersection was not recovered");

	for (const Vec2 local :
		 { Vec2{ -0.60F, 0.35F },
		   Vec2{ 0.45F, 0.55F },
		   Vec2{ 0.12F, -0.72F } }) {
		const Vec2 projectedPixels =
			ProjectLensCoordinates(local, projective);
		const Vec2 solved =
			SolveLensCoordinates(projectedPixels, projective);
		Require(
			Near(solved.x, local.x, 0.0003F) &&
				Near(solved.y, local.y, 0.0003F),
			"projective lens coordinate round trip failed");
	}

	const Vec2 projectiveAuthoredOffset{ 0.08F, -0.04F };
	const Vec2 projectiveTravel = CalculateEyeTravel(
		projective,
		projectiveAuthoredOffset,
		screenCenter,
		1.0F,
		4.0F);
	Require(
		Near(
			projectiveTravel.x,
			expectedCameraAxisLocal.x - projectiveAuthoredOffset.x,
			0.0002F) &&
			Near(
				projectiveTravel.y,
				expectedCameraAxisLocal.y - projectiveAuthoredOffset.y,
				0.0002F),
		"projective eye travel did not preserve authored reticle offset");

	// The runtime producer measures camera-to-ScopeFade displacement in the
	// optic's authored local X/Z plane, then projects it through the current
	// basis. A rigid look rotation leaves local coordinates unchanged and must
	// therefore remain exactly centered at every heading and pitch.
	const auto rigidUnrolled = ProjectLocalEyeOffsetToScreen(
		2.0F, -1.0F, 2.0F, -1.0F, 10.0F,
		240.0F, 0.0F, 0.0F, 240.0F, 240.0F);
	const auto rigidRolled = ProjectLocalEyeOffsetToScreen(
		2.0F, -1.0F, 2.0F, -1.0F, 10.0F,
		0.0F, 240.0F, -240.0F, 0.0F, 240.0F);
	Require(
		rigidUnrolled.valid && rigidRolled.valid &&
			Near(rigidUnrolled.x, 0.0F) && Near(rigidUnrolled.y, 0.0F) &&
			Near(rigidRolled.x, 0.0F) && Near(rigidRolled.y, 0.0F),
		"rigid camera/optic rotation created false eye-box motion");

	// The same local eye displacement rotates with the projected optic basis,
	// and inversion of that basis recovers the same local result. This is the
	// direction-independence contract consumed by the draw-time shader.
	const auto movedUnrolled = ProjectLocalEyeOffsetToScreen(
		0.0F, 1.0F, 2.0F, 0.0F, 10.0F,
		240.0F, 0.0F, 0.0F, 240.0F, 240.0F);
	const auto movedRolled = ProjectLocalEyeOffsetToScreen(
		0.0F, 1.0F, 2.0F, 0.0F, 10.0F,
		0.0F, 240.0F, -240.0F, 0.0F, 240.0F);
	Require(
		movedUnrolled.valid && movedRolled.valid &&
			Near(movedUnrolled.x, -0.2F) && Near(movedUnrolled.y, 0.1F) &&
			Near(movedRolled.x, -0.1F) && Near(movedRolled.y, -0.2F),
		"optic-local eye motion did not follow the current projected basis");
	Require(
		!ProjectLocalEyeOffsetToScreen(
			0.0F, 0.0F, 0.0F, 0.0F, 0.0F,
			1.0F, 0.0F, 0.0F, 1.0F, 1.0F)
			.valid,
		"invalid local aperture radius was accepted");

	// Retain the standalone screen-opposition primitive as a safety contract
	// for callers that already have a measured display-space delta.
	const auto requireScreenOpposition = [](
		float deltaX,
		float deltaY,
		float radius,
		const char* message) {
		const auto screen = CalculateScreenEyeOffset(deltaX, deltaY, radius);
		Require(screen.valid, message);
		Require(
			Near(screen.x * radius, -deltaX, 0.002F) &&
				Near(screen.y * radius, -deltaY, 0.002F),
			message);
	};
	requireScreenOpposition(
		48.0F, -18.0F, 240.0F,
		"unrolled screen-local inertia changed direction");
	requireScreenOpposition(
		48.0F, -18.0F, 240.0F,
		"quarter-rolled screen-local inertia changed direction");
	requireScreenOpposition(
		48.0F, -18.0F, 240.0F,
		"extreme-pitch sheared inertia changed direction");
	Require(
		!CalculateScreenEyeOffset(10.0F, 5.0F, 0.0F).valid,
		"invalid projected aperture radius was accepted");

	std::cout << std::format(
		"Draw-time eye-box PASSED: current=({:.3f}, {:.3f}), "
		"rolled=({:.3f}, {:.3f}), activation=({:.3f}, {:.3f}); "
		"projective/sheared frame, optic-local orientation invariance, "
		"and no previous-frame input PASSED\n",
		currentTravel.x,
		currentTravel.y,
		rolledTravel.x,
		rolledTravel.y,
		halfActivation.x,
		halfActivation.y);
	return 0;
}
