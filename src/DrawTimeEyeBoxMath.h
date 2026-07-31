#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace MagnaScope::DrawTimeEyeBox
{
	struct Vec2
	{
		float x{ 0.0F };
		float y{ 0.0F };

		[[nodiscard]] constexpr Vec2 operator+(const Vec2& rhs) const noexcept
		{
			return { x + rhs.x, y + rhs.y };
		}

		[[nodiscard]] constexpr Vec2 operator-(const Vec2& rhs) const noexcept
		{
			return { x - rhs.x, y - rhs.y };
		}

		[[nodiscard]] constexpr Vec2 operator*(float scalar) const noexcept
		{
			return { x * scalar, y * scalar };
		}
	};

	struct ClipPoint
	{
		float x{ 0.0F };
		float y{ 0.0F };
		float z{ 0.0F };
		float w{ 1.0F };

		[[nodiscard]] constexpr ClipPoint operator+(
			const ClipPoint& rhs) const noexcept
		{
			return { x + rhs.x, y + rhs.y, z + rhs.z, w + rhs.w };
		}

		[[nodiscard]] constexpr ClipPoint operator-(
			const ClipPoint& rhs) const noexcept
		{
			return { x - rhs.x, y - rhs.y, z - rhs.z, w - rhs.w };
		}

		[[nodiscard]] constexpr ClipPoint operator*(
			float scalar) const noexcept
		{
			return { x * scalar, y * scalar, z * scalar, w * scalar };
		}

		[[nodiscard]] constexpr ClipPoint operator/(
			float scalar) const noexcept
		{
			return { x / scalar, y / scalar, z / scalar, w / scalar };
		}
	};

	struct LensFrame
	{
		ClipPoint centerClip{};
		ClipPoint columnXClip{};
		ClipPoint columnZClip{};
		Vec2 centerPixels{};
		Vec2 basisX{};
		Vec2 basisZ{};
		float determinant{ 0.0F };
		float width{ 0.0F };
		float height{ 0.0F };
		bool valid{ false };
	};

	[[nodiscard]] inline Vec2 ClipToPixels(
		const ClipPoint& clip,
		float width,
		float height) noexcept
	{
		if (!std::isfinite(clip.w) || std::abs(clip.w) <= 0.00001F ||
			!std::isfinite(width) || !std::isfinite(height) ||
			width <= 0.0F || height <= 0.0F) {
			return {};
		}

		const float inverseW = 1.0F / clip.w;
		return {
			(clip.x * inverseW * 0.5F + 0.5F) * width,
			(0.5F - clip.y * inverseW * 0.5F) * height
		};
	}

	[[nodiscard]] inline LensFrame BuildLensFrame(
		const ClipPoint& centerClip,
		const ClipPoint& outerCurrentClip,
		const ClipPoint& outerNextClip,
		Vec2 localOuterCurrentDirection,
		Vec2 localOuterNextDirection,
		float width,
		float height) noexcept
	{
		LensFrame result{};
		result.centerClip = centerClip;
		result.width = width;
		result.height = height;
		result.centerPixels =
			ClipToPixels(centerClip, width, height);
		const float currentLengthSquared =
			localOuterCurrentDirection.x *
				localOuterCurrentDirection.x +
			localOuterCurrentDirection.y *
				localOuterCurrentDirection.y;
		const float nextLengthSquared =
			localOuterNextDirection.x *
				localOuterNextDirection.x +
			localOuterNextDirection.y *
				localOuterNextDirection.y;
		if (!std::isfinite(currentLengthSquared) ||
			!std::isfinite(nextLengthSquared) ||
			currentLengthSquared <= 0.000001F ||
			nextLengthSquared <= 0.000001F) {
			return result;
		}
		const float currentInverseLength =
			1.0F / std::sqrt(currentLengthSquared);
		const float nextInverseLength =
			1.0F / std::sqrt(nextLengthSquared);
		localOuterCurrentDirection =
			localOuterCurrentDirection * currentInverseLength;
		localOuterNextDirection =
			localOuterNextDirection * nextInverseLength;
		const float directionDeterminant =
			localOuterCurrentDirection.x *
				localOuterNextDirection.y -
			localOuterCurrentDirection.y *
				localOuterNextDirection.x;
		if (!std::isfinite(directionDeterminant) ||
			std::abs(directionDeterminant) <= 0.00001F) {
			return result;
		}

		const ClipPoint currentRadial =
			outerCurrentClip - centerClip;
		const ClipPoint nextRadial =
			outerNextClip - centerClip;
		result.columnXClip =
			(currentRadial * localOuterNextDirection.y -
			 nextRadial * localOuterCurrentDirection.y) /
			directionDeterminant;
		result.columnZClip =
			(currentRadial * -localOuterNextDirection.x +
			 nextRadial * localOuterCurrentDirection.x) /
			directionDeterminant;

		const auto projectLocal = [&](Vec2 local) {
			return ClipToPixels(
				result.centerClip +
					result.columnXClip * local.x +
					result.columnZClip * local.y,
				width,
				height);
		};
		result.basisX =
			projectLocal({ 1.0F, 0.0F }) - result.centerPixels;
		result.basisZ =
			projectLocal({ 0.0F, 1.0F }) - result.centerPixels;
		result.determinant =
			result.basisX.x * result.basisZ.y -
			result.basisX.y * result.basisZ.x;

		const float basisXLengthSquared =
			result.basisX.x * result.basisX.x +
			result.basisX.y * result.basisX.y;
		const float basisZLengthSquared =
			result.basisZ.x * result.basisZ.x +
			result.basisZ.y * result.basisZ.y;
		result.valid =
			std::isfinite(result.centerPixels.x) &&
			std::isfinite(result.centerPixels.y) &&
			std::isfinite(result.determinant) &&
			basisXLengthSquared > 0.0001F &&
			basisZLengthSquared > 0.0001F &&
			std::abs(result.determinant) > 0.0001F;
		return result;
	}

	[[nodiscard]] inline Vec2 SolveLensCoordinates(
		Vec2 targetPixels,
		const LensFrame& frame) noexcept
	{
		if (!frame.valid) {
			return {};
		}
		const Vec2 targetNdc{
			targetPixels.x * (2.0F / frame.width) - 1.0F,
			1.0F - targetPixels.y * (2.0F / frame.height)
		};
		const Vec2 columnX{
			frame.columnXClip.x -
				targetNdc.x * frame.columnXClip.w,
			frame.columnXClip.y -
				targetNdc.y * frame.columnXClip.w
		};
		const Vec2 columnZ{
			frame.columnZClip.x -
				targetNdc.x * frame.columnZClip.w,
			frame.columnZClip.y -
				targetNdc.y * frame.columnZClip.w
		};
		const Vec2 constant{
			frame.centerClip.x -
				targetNdc.x * frame.centerClip.w,
			frame.centerClip.y -
				targetNdc.y * frame.centerClip.w
		};
		const float determinant =
			columnX.x * columnZ.y -
			columnX.y * columnZ.x;
		if (!std::isfinite(determinant) ||
			std::abs(determinant) <= 0.00001F) {
			return {};
		}
		return {
			(-constant.x * columnZ.y +
			 constant.y * columnZ.x) /
				determinant,
			(-columnX.x * constant.y +
			 columnX.y * constant.x) /
				determinant
		};
	}

	[[nodiscard]] inline Vec2 ProjectLensCoordinates(
		Vec2 lensCoordinates,
		const LensFrame& frame) noexcept
	{
		if (!frame.valid) {
			return {};
		}
		return ClipToPixels(
			frame.centerClip +
				frame.columnXClip * lensCoordinates.x +
				frame.columnZClip * lensCoordinates.y,
			frame.width,
			frame.height);
	}

	[[nodiscard]] inline Vec2 LensCoordinatesToPixels(
		Vec2 lensCoordinates,
		const LensFrame& frame) noexcept
	{
		if (!frame.valid) {
			return {};
		}
		return
			ProjectLensCoordinates(lensCoordinates, frame) -
			frame.centerPixels;
	}

	[[nodiscard]] inline Vec2 CalculateEyeTravel(
		const LensFrame& frame,
		Vec2 authoredAimOffsetLocal,
		Vec2 cameraAxisPixels,
		float activation,
		float maximumTravel) noexcept
	{
		if (!frame.valid) {
			return {};
		}

		const Vec2 cameraAxisLocal =
			SolveLensCoordinates(cameraAxisPixels, frame);
		Vec2 travel =
			cameraAxisLocal - authoredAimOffsetLocal;
		const float lengthSquared =
			travel.x * travel.x + travel.y * travel.y;
		const float boundedMaximum =
			std::clamp(maximumTravel, 0.0F, 4.0F);
		if (lengthSquared > boundedMaximum * boundedMaximum &&
			lengthSquared > 0.000001F) {
			const float scale =
				boundedMaximum / std::sqrt(lengthSquared);
			travel = travel * scale;
		}
		return travel * std::clamp(activation, 0.0F, 1.0F);
	}
}
