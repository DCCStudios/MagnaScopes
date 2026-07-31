#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

namespace MagnaScope::ReticleVertexScaling
{
	inline constexpr std::uint64_t kFullPrecisionVertexFlag =
		static_cast<std::uint64_t>(0x400U) << 44U;

	struct Float3
	{
		float x = 0.0F;
		float y = 0.0F;
		float z = 0.0F;
	};

	struct PreparedVertices
	{
		std::vector<std::byte> authoredBytes;
		Float3 centroid{};
		std::uint32_t vertexCount = 0;
		std::uint32_t stride = 0;
		bool fullPrecision = false;
	};

	// ScopeFade magnifies the complete late color source, which already
	// contains the STS reticle. Pre-scaling the authored reticle by this factor
	// makes the final on-screen reticle scale independent of scene zoom.
	[[nodiscard]] inline float CalculateAuthoredPreScale(
		float requestedFinalScale,
		float requestedSceneMagnification,
		float activation) noexcept
	{
		if (!std::isfinite(requestedFinalScale) ||
			!std::isfinite(requestedSceneMagnification) ||
			!std::isfinite(activation)) {
			return 1.0F;
		}
		const float boundedActivation =
			std::clamp(activation, 0.0F, 1.0F);
		const float desiredFinalScale = std::lerp(
			1.0F,
			std::clamp(requestedFinalScale, 0.25F, 8.0F),
			boundedActivation);
		const float effectiveSceneMagnification = std::lerp(
			1.0F,
			std::clamp(requestedSceneMagnification, 1.0F, 15.0F),
			boundedActivation);
		return desiredFinalScale /
			std::max(effectiveSceneMagnification, 0.0001F);
	}

	inline float HalfToFloat(std::uint16_t value) noexcept
	{
		const std::uint32_t sign =
			static_cast<std::uint32_t>(value & 0x8000U) << 16U;
		std::uint32_t exponent = (value >> 10U) & 0x1FU;
		std::uint32_t mantissa = value & 0x03FFU;
		std::uint32_t result = 0;

		if (exponent == 0U) {
			if (mantissa == 0U) {
				result = sign;
			} else {
				// Normalize the fp16 subnormal before rebiasing it to fp32.
				std::int32_t unbiasedExponent = -14;
				while ((mantissa & 0x0400U) == 0U) {
					mantissa <<= 1U;
					--unbiasedExponent;
				}
				mantissa &= 0x03FFU;
				result =
					sign |
					static_cast<std::uint32_t>(
						unbiasedExponent + 127)
						<< 23U |
					mantissa << 13U;
			}
		} else if (exponent == 0x1FU) {
			result = sign | 0x7F800000U | mantissa << 13U;
		} else {
			exponent += 127U - 15U;
			result = sign | exponent << 23U | mantissa << 13U;
		}
		return std::bit_cast<float>(result);
	}

	inline bool FloatToHalf(float value, std::uint16_t& result) noexcept
	{
		if (!std::isfinite(value) ||
			std::abs(value) > 65504.0F) {
			return false;
		}

		const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
		const std::uint16_t sign =
			static_cast<std::uint16_t>((bits >> 16U) & 0x8000U);
		const std::uint32_t mantissa = bits & 0x007FFFFFU;
		const std::int32_t exponent =
			static_cast<std::int32_t>((bits >> 23U) & 0xFFU) - 127;

		if (exponent > 15) {
			return false;
		}
		if (exponent < -24) {
			result = sign;
			return true;
		}
		if (exponent < -14) {
			const std::uint32_t shift =
				static_cast<std::uint32_t>(-exponent - 14);
			std::uint32_t halfMantissa =
				(mantissa | 0x00800000U) >> (shift + 13U);
			const std::uint32_t remainderMask =
				(1U << (shift + 13U)) - 1U;
			const std::uint32_t remainder = mantissa & remainderMask;
			const std::uint32_t halfway = 1U << (shift + 12U);
			if (remainder > halfway ||
				(remainder == halfway && (halfMantissa & 1U) != 0U)) {
				++halfMantissa;
			}
			result = static_cast<std::uint16_t>(sign | halfMantissa);
			return true;
		}

		std::uint32_t halfExponent =
			static_cast<std::uint32_t>(exponent + 15) << 10U;
		std::uint32_t halfMantissa = mantissa >> 13U;
		const std::uint32_t remainder = mantissa & 0x1FFFU;
		if (remainder > 0x1000U ||
			(remainder == 0x1000U && (halfMantissa & 1U) != 0U)) {
			++halfMantissa;
			if (halfMantissa == 0x400U) {
				halfMantissa = 0U;
				halfExponent += 0x400U;
				if (halfExponent >= 0x7C00U) {
					return false;
				}
			}
		}
		result = static_cast<std::uint16_t>(
			sign | halfExponent | halfMantissa);
		return true;
	}

	inline bool ReadPosition(
		std::span<const std::byte> bytes,
		std::size_t offset,
		bool fullPrecision,
		Float3& position) noexcept
	{
		const std::size_t positionSize =
			fullPrecision ? sizeof(float) * 3U : sizeof(std::uint16_t) * 3U;
		if (offset > bytes.size() ||
			positionSize > bytes.size() - offset) {
			return false;
		}

		if (fullPrecision) {
			std::memcpy(&position.x, bytes.data() + offset, sizeof(float));
			std::memcpy(
				&position.y,
				bytes.data() + offset + sizeof(float),
				sizeof(float));
			std::memcpy(
				&position.z,
				bytes.data() + offset + sizeof(float) * 2U,
				sizeof(float));
		} else {
			std::uint16_t packed[3]{};
			std::memcpy(packed, bytes.data() + offset, sizeof(packed));
			position = {
				HalfToFloat(packed[0]),
				HalfToFloat(packed[1]),
				HalfToFloat(packed[2])
			};
		}
		return std::isfinite(position.x) &&
		       std::isfinite(position.y) &&
		       std::isfinite(position.z);
	}

	inline bool WritePosition(
		std::span<std::byte> bytes,
		std::size_t offset,
		bool fullPrecision,
		const Float3& position) noexcept
	{
		const std::size_t positionSize =
			fullPrecision ? sizeof(float) * 3U : sizeof(std::uint16_t) * 3U;
		if (offset > bytes.size() ||
			positionSize > bytes.size() - offset ||
			!std::isfinite(position.x) ||
			!std::isfinite(position.y) ||
			!std::isfinite(position.z)) {
			return false;
		}

		if (fullPrecision) {
			std::memcpy(bytes.data() + offset, &position.x, sizeof(float));
			std::memcpy(
				bytes.data() + offset + sizeof(float),
				&position.y,
				sizeof(float));
			std::memcpy(
				bytes.data() + offset + sizeof(float) * 2U,
				&position.z,
				sizeof(float));
			return true;
		}

		std::uint16_t packed[3]{};
		if (!FloatToHalf(position.x, packed[0]) ||
			!FloatToHalf(position.y, packed[1]) ||
			!FloatToHalf(position.z, packed[2])) {
			return false;
		}
		std::memcpy(bytes.data() + offset, packed, sizeof(packed));
		return true;
	}

	inline bool Prepare(
		std::span<const std::byte> bytes,
		std::uint32_t vertexCount,
		std::uint32_t stride,
		std::uint64_t vertexDescriptor,
		PreparedVertices& result)
	{
		const bool fullPrecision =
			(vertexDescriptor & kFullPrecisionVertexFlag) != 0U;
		const std::size_t positionSize =
			fullPrecision ? sizeof(float) * 3U : sizeof(std::uint16_t) * 3U;
		if (vertexCount == 0U ||
			stride < positionSize ||
			vertexCount >
				std::numeric_limits<std::size_t>::max() / stride) {
			return false;
		}
		const std::size_t requiredBytes =
			static_cast<std::size_t>(vertexCount) * stride;
		if (bytes.size() < requiredBytes) {
			return false;
		}

		double sumX = 0.0;
		double sumY = 0.0;
		double sumZ = 0.0;
		for (std::uint32_t index = 0; index < vertexCount; ++index) {
			Float3 position{};
			if (!ReadPosition(
					bytes,
					static_cast<std::size_t>(index) * stride,
					fullPrecision,
					position)) {
				return false;
			}
			sumX += position.x;
			sumY += position.y;
			sumZ += position.z;
		}

		const double divisor = static_cast<double>(vertexCount);
		const Float3 centroid{
			static_cast<float>(sumX / divisor),
			static_cast<float>(sumY / divisor),
			static_cast<float>(sumZ / divisor)
		};
		if (!std::isfinite(centroid.x) ||
			!std::isfinite(centroid.y) ||
			!std::isfinite(centroid.z)) {
			return false;
		}

		PreparedVertices prepared{};
		prepared.authoredBytes.assign(
			bytes.begin(),
			bytes.begin() + static_cast<std::ptrdiff_t>(requiredBytes));
		prepared.centroid = centroid;
		prepared.vertexCount = vertexCount;
		prepared.stride = stride;
		prepared.fullPrecision = fullPrecision;
		result = std::move(prepared);
		return true;
	}

	inline bool Scale(
		const PreparedVertices& prepared,
		float factor,
		std::vector<std::byte>& output)
	{
		if (!std::isfinite(factor) ||
			factor <= 0.0F ||
			prepared.vertexCount == 0U ||
			prepared.stride == 0U) {
			return false;
		}

		output = prepared.authoredBytes;
		if (std::abs(factor - 1.0F) <= 0.000001F) {
			return true;
		}

		for (std::uint32_t index = 0;
			 index < prepared.vertexCount;
			 ++index) {
			const std::size_t offset =
				static_cast<std::size_t>(index) * prepared.stride;
			Float3 position{};
			if (!ReadPosition(
					prepared.authoredBytes,
					offset,
					prepared.fullPrecision,
					position)) {
				return false;
			}
			const Float3 scaled{
				prepared.centroid.x +
					(position.x - prepared.centroid.x) * factor,
				prepared.centroid.y +
					(position.y - prepared.centroid.y) * factor,
				prepared.centroid.z +
					(position.z - prepared.centroid.z) * factor
			};
			if (!WritePosition(
					output,
					offset,
					prepared.fullPrecision,
					scaled)) {
				return false;
			}
		}
		return true;
	}

	inline bool ValidateIndices(
		std::span<const std::byte> bytes,
		std::uint32_t indexCount,
		std::uint32_t indexElementSize,
		std::uint32_t vertexCount) noexcept
	{
		if (indexCount == 0U ||
			vertexCount == 0U ||
			(indexElementSize != sizeof(std::uint16_t) &&
				indexElementSize != sizeof(std::uint32_t)) ||
			indexCount >
				std::numeric_limits<std::size_t>::max() /
					indexElementSize) {
			return false;
		}
		const std::size_t requiredBytes =
			static_cast<std::size_t>(indexCount) * indexElementSize;
		if (bytes.size() < requiredBytes) {
			return false;
		}

		for (std::uint32_t index = 0; index < indexCount; ++index) {
			std::uint32_t value = 0U;
			const auto* address =
				bytes.data() +
				static_cast<std::size_t>(index) * indexElementSize;
			if (indexElementSize == sizeof(std::uint16_t)) {
				std::uint16_t compact = 0U;
				std::memcpy(&compact, address, sizeof(compact));
				value = compact;
			} else {
				std::memcpy(&value, address, sizeof(value));
			}
			if (value >= vertexCount) {
				return false;
			}
		}
		return true;
	}
}
