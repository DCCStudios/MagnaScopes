#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <iostream>
#include <stdexcept>
#include <vector>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include "../src/ReticleVertexScaling.h"

using Microsoft::WRL::ComPtr;

namespace
{
	constexpr std::uint32_t kWidth = 128;
	constexpr std::uint32_t kHeight = 128;
	constexpr std::uint32_t kSegments = 24;
	constexpr float kCenterNdcX = 0.20F;
	constexpr float kCenterNdcY = -0.10F;

	struct Vertex
	{
		float x;
		float y;
		float z;
		float w{ 1.0F };
	};

	struct alignas(16) ResolutionConstants
	{
		float width = 0.0F;
		float height = 0.0F;
		float magnification = 1.0F;
		float aimOffsetValid = 0.0F;

		float aimOffsetX = 0.0F;
		float aimOffsetY = 0.0F;
		float imageDenoise = 0.0F;
		float imageSharpen = 0.0F;

		float fishEyeStrength = 0.0F;
		float fishEyePower = 2.0F;
		float lensRadiusX = 40.0F;
		float lensRadiusY = 40.0F;

		float aimCenterX = 0.0F;
		float aimCenterY = 0.0F;
		float reticleMagnification = 1.0F;
		float activationProgress = 1.0F;

		float edgeRefractionStrength = 0.0F;
		float edgeRefractionWidth = 0.15F;
		float edgeChromaticAberration = 0.0F;
		float sceneParallaxStrength = 0.0F;

		float eyeOffsetX = 0.0F;
		float eyeOffsetY = 0.0F;
		float opticalLagStrength = 1.0F;
		float physicalEyeBoxValid = 0.0F;

		float lensBasisXX = 0.0F;
		float lensBasisXY = 0.0F;
		float lensBasisZX = 0.0F;
		float lensBasisZY = 0.0F;

		float eyeBoxRadius = 2.0F;
		float vignetteReach = 9.0F;
		float vignetteSharpness = 3.0F;
		float eyeBoxMaxTravel = 4.0F;

		float reticleSize = 4.0F;
		float reticleOffsetX = 0.0F;
		float reticleOffsetY = 0.0F;
		float reticlePadding = 0.0F;
	};
	static_assert(sizeof(ResolutionConstants) == 144);

	void Check(HRESULT result, std::string_view operation)
	{
		if (FAILED(result)) {
			throw std::runtime_error(
				std::format("{} failed: 0x{:08X}",
					operation,
					static_cast<std::uint32_t>(result)));
		}
	}

	std::vector<std::byte> ReadBinary(const std::filesystem::path& path)
	{
		FILE* file = nullptr;
		if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file) {
			throw std::runtime_error(
				std::format("missing shader: {}", path.string()));
		}
		_fseeki64(file, 0, SEEK_END);
		const auto size = _ftelli64(file);
		_fseeki64(file, 0, SEEK_SET);
		std::vector<std::byte> bytes(static_cast<std::size_t>(size));
		if (fread(bytes.data(), 1, bytes.size(), file) != bytes.size()) {
			fclose(file);
			throw std::runtime_error(
				std::format("cannot read shader: {}", path.string()));
		}
		fclose(file);
		return bytes;
	}

	std::array<std::uint8_t, 4> Pixel(
		const D3D11_MAPPED_SUBRESOURCE& mapped,
		std::uint32_t x,
		std::uint32_t y)
	{
		const auto* row = static_cast<const std::uint8_t*>(mapped.pData) +
		                  y * mapped.RowPitch;
		return { row[x * 4], row[x * 4 + 1], row[x * 4 + 2], row[x * 4 + 3] };
	}

	void VerifyPackedReticleScaling()
	{
		using namespace MagnaScope::ReticleVertexScaling;
		const float preScale4x =
			CalculateAuthoredPreScale(1.0F, 4.0F, 1.0F);
		if (std::abs(preScale4x - 0.25F) > 0.0001F ||
			std::abs(preScale4x * 4.0F - 1.0F) > 0.0001F) {
			throw std::runtime_error(
				"reticle inverse scene-magnification contract failed");
		}
		const float halfActivated =
			CalculateAuthoredPreScale(1.0F, 4.0F, 0.5F);
		if (std::abs(halfActivated * 2.5F - 1.0F) > 0.0001F) {
			throw std::runtime_error(
				"reticle activation interpolation contract failed");
		}

		constexpr std::uint32_t floatStride = 24U;
		constexpr std::array<Float3, 4> floatPositions{
			Float3{ -2.0F, -1.0F, 0.5F },
			Float3{ 4.0F, -1.0F, 0.5F },
			Float3{ 4.0F, 3.0F, 0.5F },
			Float3{ -2.0F, 3.0F, 0.5F }
		};
		std::vector<std::byte> floatBytes(
			floatPositions.size() * floatStride,
			std::byte{ 0 });
		for (std::size_t index = 0; index < floatPositions.size(); ++index) {
			const auto offset = index * floatStride;
			if (!WritePosition(
					floatBytes,
					offset,
					true,
					floatPositions[index])) {
				throw std::runtime_error("cannot write full-precision fixture");
			}
			for (std::size_t byte = 12U; byte < floatStride; ++byte) {
				floatBytes[offset + byte] =
					static_cast<std::byte>(0x40U + index + byte);
			}
		}

		PreparedVertices preparedFloat{};
		if (!Prepare(
				floatBytes,
				static_cast<std::uint32_t>(floatPositions.size()),
				floatStride,
				kFullPrecisionVertexFlag,
				preparedFloat)) {
			throw std::runtime_error(
				"full-precision reticle preparation failed");
		}
		if (std::abs(preparedFloat.centroid.x - 1.0F) > 0.0001F ||
			std::abs(preparedFloat.centroid.y - 1.0F) > 0.0001F ||
			std::abs(preparedFloat.centroid.z - 0.5F) > 0.0001F) {
			throw std::runtime_error(
				"full-precision reticle centroid is incorrect");
		}

		std::vector<std::byte> identityBytes;
		if (!Scale(preparedFloat, 1.0F, identityBytes) ||
			identityBytes != floatBytes) {
			throw std::runtime_error(
				"1x reticle scaling must remain byte-identical");
		}

		std::vector<std::byte> scaledBytes;
		if (!Scale(preparedFloat, 2.0F, scaledBytes)) {
			throw std::runtime_error(
				"2x full-precision reticle scaling failed");
		}
		for (std::size_t index = 0; index < floatPositions.size(); ++index) {
			const auto offset = index * floatStride;
			Float3 scaled{};
			if (!ReadPosition(scaledBytes, offset, true, scaled)) {
				throw std::runtime_error(
					"cannot decode scaled full-precision position");
			}
			const Float3 expected{
				1.0F + (floatPositions[index].x - 1.0F) * 2.0F,
				1.0F + (floatPositions[index].y - 1.0F) * 2.0F,
				0.5F + (floatPositions[index].z - 0.5F) * 2.0F
			};
			if (std::abs(scaled.x - expected.x) > 0.0001F ||
				std::abs(scaled.y - expected.y) > 0.0001F ||
				std::abs(scaled.z - expected.z) > 0.0001F ||
				!std::equal(
					floatBytes.begin() +
						static_cast<std::ptrdiff_t>(offset + 12U),
					floatBytes.begin() +
						static_cast<std::ptrdiff_t>(
							offset + floatStride),
					scaledBytes.begin() +
						static_cast<std::ptrdiff_t>(offset + 12U))) {
				throw std::runtime_error(
					"full-precision scaling changed non-position attributes");
			}
		}

		// Recoil changes only the later world transform. Rebuilding every
		// local scale from immutable authored bytes prevents cumulative drift
		// when the influence changes rapidly during semi-auto or automatic fire.
		for (const float recoilScale : { 4.0F, 1.25F, 3.5F, 2.0F, 4.0F }) {
			std::vector<std::byte> recoilBytes;
			if (!Scale(preparedFloat, recoilScale, recoilBytes)) {
				throw std::runtime_error(
					"recoil scale sequence failed");
			}
			double sumX = 0.0;
			double sumY = 0.0;
			double sumZ = 0.0;
			for (std::size_t index = 0;
				 index < floatPositions.size();
				 ++index) {
				Float3 position{};
				if (!ReadPosition(
						recoilBytes,
						index * floatStride,
						true,
						position)) {
					throw std::runtime_error(
						"cannot decode recoil sequence position");
				}
				sumX += position.x;
				sumY += position.y;
				sumZ += position.z;
			}
			if (std::abs(
					static_cast<float>(
						sumX / floatPositions.size()) -
					preparedFloat.centroid.x) > 0.0001F ||
				std::abs(
					static_cast<float>(
						sumY / floatPositions.size()) -
					preparedFloat.centroid.y) > 0.0001F ||
				std::abs(
					static_cast<float>(
						sumZ / floatPositions.size()) -
					preparedFloat.centroid.z) > 0.0001F) {
				throw std::runtime_error(
					"recoil sequence moved the local vertex centroid");
			}
		}

		constexpr std::uint32_t halfStride = 16U;
		std::vector<std::byte> halfBytes(
			floatPositions.size() * halfStride,
			std::byte{ 0x5A });
		for (std::size_t index = 0; index < floatPositions.size(); ++index) {
			if (!WritePosition(
					halfBytes,
					index * halfStride,
					false,
					floatPositions[index])) {
				throw std::runtime_error("cannot write fp16 fixture");
			}
		}
		PreparedVertices preparedHalf{};
		if (!Prepare(
				halfBytes,
				static_cast<std::uint32_t>(floatPositions.size()),
				halfStride,
				0U,
				preparedHalf)) {
			throw std::runtime_error("fp16 reticle preparation failed");
		}
		std::vector<std::byte> scaledHalf;
		if (!Scale(preparedHalf, 2.0F, scaledHalf)) {
			throw std::runtime_error("fp16 reticle scaling failed");
		}
		for (std::size_t index = 0; index < floatPositions.size(); ++index) {
			const auto offset = index * halfStride;
			if (!std::equal(
					halfBytes.begin() +
						static_cast<std::ptrdiff_t>(offset + 6U),
					halfBytes.begin() +
						static_cast<std::ptrdiff_t>(offset + halfStride),
					scaledHalf.begin() +
						static_cast<std::ptrdiff_t>(offset + 6U))) {
				throw std::runtime_error(
					"fp16 scaling changed non-position attributes");
			}
		}

		const std::array<std::uint16_t, 6> indices16{
			0U, 1U, 2U, 0U, 2U, 3U
		};
		const std::array<std::uint32_t, 6> indices32{
			0U, 1U, 2U, 0U, 2U, 3U
		};
		if (!ValidateIndices(
				std::as_bytes(std::span(indices16)),
				static_cast<std::uint32_t>(indices16.size()),
				sizeof(std::uint16_t),
				static_cast<std::uint32_t>(floatPositions.size())) ||
			!ValidateIndices(
				std::as_bytes(std::span(indices32)),
				static_cast<std::uint32_t>(indices32.size()),
				sizeof(std::uint32_t),
				static_cast<std::uint32_t>(floatPositions.size()))) {
			throw std::runtime_error(
				"valid packed reticle indices were rejected");
		}
		auto invalidIndices = indices16;
		invalidIndices.back() = 4U;
		if (ValidateIndices(
				std::as_bytes(std::span(invalidIndices)),
				static_cast<std::uint32_t>(invalidIndices.size()),
				sizeof(std::uint16_t),
				static_cast<std::uint32_t>(floatPositions.size()))) {
			throw std::runtime_error(
				"out-of-range reticle index was accepted");
		}
	}
}

int wmain(int argc, wchar_t** argv)
try {
	VerifyPackedReticleScaling();

	const auto project = argc > 1 ?
	                         std::filesystem::path(argv[1]) :
	                         std::filesystem::current_path();
	const auto shaderDirectory =
		project / "Compile" / "Shaders" / "MagnaScope";
	const auto geometryShaderBytes =
		ReadBinary(shaderDirectory / "ScopeGeometryFill_GS.cso");
	const auto pixelShaderBytes =
		ReadBinary(shaderDirectory / "ScopeGeometryProbe_PS.cso");
	const auto magnifyShaderBytes =
		ReadBinary(shaderDirectory / "ScopeGeometryMagnify_PS.cso");

	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> context;
	D3D_FEATURE_LEVEL featureLevel{};
	Check(
		D3D11CreateDevice(
			nullptr,
			D3D_DRIVER_TYPE_WARP,
			nullptr,
			0,
			nullptr,
			0,
			D3D11_SDK_VERSION,
			device.GetAddressOf(),
			&featureLevel,
			context.GetAddressOf()),
		"D3D11CreateDevice(WARP)");

	static constexpr char vertexShaderSource[] = R"(
struct Input
{
    float4 position : POSITION;
};
struct Output
{
    float4 position : SV_Position;
};
Output main(Input input)
{
    Output output;
    output.position = input.position;
    return output;
}
)";
	ComPtr<ID3DBlob> vertexBlob;
	ComPtr<ID3DBlob> errors;
	Check(
		D3DCompile(
			vertexShaderSource,
			sizeof(vertexShaderSource),
			"ScopeGeometryFillShaderHarness",
			nullptr,
			nullptr,
			"main",
			"vs_5_0",
			D3DCOMPILE_OPTIMIZATION_LEVEL3,
			0,
			vertexBlob.GetAddressOf(),
			errors.GetAddressOf()),
		"D3DCompile(test VS)");

	ComPtr<ID3D11VertexShader> vertexShader;
	ComPtr<ID3D11GeometryShader> geometryShader;
	ComPtr<ID3D11PixelShader> pixelShader;
	ComPtr<ID3D11PixelShader> magnifyShader;
	Check(
		device->CreateVertexShader(
			vertexBlob->GetBufferPointer(),
			vertexBlob->GetBufferSize(),
			nullptr,
			vertexShader.GetAddressOf()),
		"CreateVertexShader");
	Check(
		device->CreateGeometryShader(
			geometryShaderBytes.data(),
			geometryShaderBytes.size(),
			nullptr,
			geometryShader.GetAddressOf()),
		"CreateGeometryShader");
	Check(
		device->CreatePixelShader(
			pixelShaderBytes.data(),
			pixelShaderBytes.size(),
			nullptr,
			pixelShader.GetAddressOf()),
		"CreatePixelShader");
	Check(
		device->CreatePixelShader(
			magnifyShaderBytes.data(),
			magnifyShaderBytes.size(),
			nullptr,
			magnifyShader.GetAddressOf()),
		"CreatePixelShader(magnify)");

	const D3D11_INPUT_ELEMENT_DESC inputElement{
		"POSITION",
		0,
		DXGI_FORMAT_R32G32B32A32_FLOAT,
		0,
		0,
		D3D11_INPUT_PER_VERTEX_DATA,
		0
	};
	ComPtr<ID3D11InputLayout> inputLayout;
	Check(
		device->CreateInputLayout(
			&inputElement,
			1,
			vertexBlob->GetBufferPointer(),
			vertexBlob->GetBufferSize(),
			inputLayout.GetAddressOf()),
		"CreateInputLayout");

	std::array<Vertex, kSegments * 2> vertices{};
	std::array<std::uint16_t, kSegments * 6> indices{};
	for (std::uint32_t segment = 0; segment < kSegments; ++segment) {
		const float angle =
			static_cast<float>(segment) *
			6.28318530717958647692F /
			static_cast<float>(kSegments);
		const float sine = std::sin(angle);
		const float cosine = std::cos(angle);
		vertices[segment * 2] = {
			kCenterNdcX + sine * 0.80F,
			kCenterNdcY + cosine * 0.80F,
			0.5F
		};
		vertices[segment * 2 + 1] = {
			kCenterNdcX + sine * 0.40F,
			kCenterNdcY + cosine * 0.40F,
			0.5F
		};

		const auto next = (segment + 1) % kSegments;
		const std::uint16_t outerCurrent =
			static_cast<std::uint16_t>(segment * 2);
		const std::uint16_t innerCurrent =
			static_cast<std::uint16_t>(segment * 2 + 1);
		const std::uint16_t outerNext =
			static_cast<std::uint16_t>(next * 2);
		const std::uint16_t innerNext =
			static_cast<std::uint16_t>(next * 2 + 1);
		const std::size_t offset = segment * 6;
		// This is the exact alternating index order found in every extracted
		// STS ScopeFade:0 shape. The geometry shader fills the center from the
		// odd primitive without replacing the game's input assembly.
		indices[offset] = outerCurrent;
		indices[offset + 1] = innerNext;
		indices[offset + 2] = outerNext;
		indices[offset + 3] = outerCurrent;
		indices[offset + 4] = innerCurrent;
		indices[offset + 5] = innerNext;
	}

	D3D11_BUFFER_DESC vertexDescription{};
	vertexDescription.ByteWidth = sizeof(vertices);
	// The shipping ScopeFade buffer is immutable from MagnaScope's point of
	// view, but this harness deliberately submits several authored poses
	// through the same draw path. A dynamic fixture lets the test prove that
	// the pixel shader consumes the vertices from the current draw rather
	// than a CPU value published by an earlier animation update.
	vertexDescription.Usage = D3D11_USAGE_DYNAMIC;
	vertexDescription.BindFlags = D3D11_BIND_VERTEX_BUFFER;
	vertexDescription.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	const D3D11_SUBRESOURCE_DATA vertexData{ vertices.data() };
	ComPtr<ID3D11Buffer> vertexBuffer;
	Check(
		device->CreateBuffer(
			&vertexDescription,
			&vertexData,
			vertexBuffer.GetAddressOf()),
		"CreateBuffer(VB)");

	D3D11_BUFFER_DESC indexDescription{};
	indexDescription.ByteWidth = sizeof(indices);
	indexDescription.Usage = D3D11_USAGE_IMMUTABLE;
	indexDescription.BindFlags = D3D11_BIND_INDEX_BUFFER;
	const D3D11_SUBRESOURCE_DATA indexData{ indices.data() };
	ComPtr<ID3D11Buffer> indexBuffer;
	Check(
		device->CreateBuffer(
			&indexDescription,
			&indexData,
			indexBuffer.GetAddressOf()),
		"CreateBuffer(IB)");

	D3D11_TEXTURE2D_DESC textureDescription{};
	textureDescription.Width = kWidth;
	textureDescription.Height = kHeight;
	textureDescription.MipLevels = 1;
	textureDescription.ArraySize = 1;
	textureDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	textureDescription.SampleDesc.Count = 1;
	textureDescription.Usage = D3D11_USAGE_DEFAULT;
	textureDescription.BindFlags = D3D11_BIND_RENDER_TARGET;
	ComPtr<ID3D11Texture2D> renderTarget;
	Check(
		device->CreateTexture2D(
			&textureDescription,
			nullptr,
			renderTarget.GetAddressOf()),
		"CreateTexture2D(RT)");
	ComPtr<ID3D11RenderTargetView> renderTargetView;
	Check(
		device->CreateRenderTargetView(
			renderTarget.Get(),
			nullptr,
			renderTargetView.GetAddressOf()),
		"CreateRenderTargetView");

	D3D11_RASTERIZER_DESC rasterizerDescription{};
	rasterizerDescription.FillMode = D3D11_FILL_SOLID;
	rasterizerDescription.CullMode = D3D11_CULL_NONE;
	rasterizerDescription.DepthClipEnable = TRUE;
	ComPtr<ID3D11RasterizerState> rasterizerState;
	Check(
		device->CreateRasterizerState(
			&rasterizerDescription,
			rasterizerState.GetAddressOf()),
		"CreateRasterizerState");

	const float clear[4]{ 0.0F, 0.0F, 0.0F, 1.0F };
	context->ClearRenderTargetView(renderTargetView.Get(), clear);
	ID3D11RenderTargetView* target = renderTargetView.Get();
	context->OMSetRenderTargets(1, &target, nullptr);
	const D3D11_VIEWPORT viewport{
		0.0F,
		0.0F,
		static_cast<float>(kWidth),
		static_cast<float>(kHeight),
		0.0F,
		1.0F
	};
	context->RSSetViewports(1, &viewport);
	context->RSSetState(rasterizerState.Get());
	const UINT stride = sizeof(Vertex);
	const UINT offset = 0;
	ID3D11Buffer* boundVertexBuffer = vertexBuffer.Get();
	context->IASetInputLayout(inputLayout.Get());
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->IASetVertexBuffers(0, 1, &boundVertexBuffer, &stride, &offset);
	context->IASetIndexBuffer(indexBuffer.Get(), DXGI_FORMAT_R16_UINT, 0);
	context->VSSetShader(vertexShader.Get(), nullptr, 0);
	context->GSSetShader(geometryShader.Get(), nullptr, 0);
	context->PSSetShader(pixelShader.Get(), nullptr, 0);
	context->DrawIndexed(static_cast<UINT>(indices.size()), 0, 0);

	const auto submitLensPose =
		[&](float translationNdcX, float translationNdcY) {
			std::array<Vertex, kSegments * 2> posedVertices = vertices;
			for (auto& vertex : posedVertices) {
				vertex.x += translationNdcX * vertex.w;
				vertex.y += translationNdcY * vertex.w;
			}
			D3D11_MAPPED_SUBRESOURCE mappedVertices{};
			Check(
				context->Map(
					vertexBuffer.Get(),
					0,
					D3D11_MAP_WRITE_DISCARD,
					0,
					&mappedVertices),
				"Map(dynamic ScopeFade fixture)");
			std::memcpy(
				mappedVertices.pData,
				posedVertices.data(),
				sizeof(posedVertices));
			context->Unmap(vertexBuffer.Get(), 0);
		};

	const auto submitProjectiveLensPose =
		[&](
			const Vertex& centerClip,
			const Vertex& columnXClip,
			const Vertex& columnZClip) {
			std::array<Vertex, kSegments * 2> posedVertices{};
			for (std::uint32_t segment = 0;
				 segment < kSegments;
				 ++segment) {
				const float angle =
					static_cast<float>(segment) *
					6.28318530717958647692F /
					static_cast<float>(kSegments);
				const float localX = std::sin(angle);
				const float localZ = std::cos(angle);
				const auto project = [&](float radius) {
					return Vertex{
						centerClip.x +
							columnXClip.x * localX * radius +
							columnZClip.x * localZ * radius,
						centerClip.y +
							columnXClip.y * localX * radius +
							columnZClip.y * localZ * radius,
						centerClip.z +
							columnXClip.z * localX * radius +
							columnZClip.z * localZ * radius,
						centerClip.w +
							columnXClip.w * localX * radius +
							columnZClip.w * localZ * radius
					};
				};
				posedVertices[segment * 2] = project(1.0F);
				posedVertices[segment * 2 + 1] = project(0.5F);
			}
			D3D11_MAPPED_SUBRESOURCE mappedVertices{};
			Check(
				context->Map(
					vertexBuffer.Get(),
					0,
					D3D11_MAP_WRITE_DISCARD,
					0,
					&mappedVertices),
				"Map(projective ScopeFade fixture)");
			std::memcpy(
				mappedVertices.pData,
				posedVertices.data(),
				sizeof(posedVertices));
			context->Unmap(vertexBuffer.Get(), 0);
		};

	textureDescription.Usage = D3D11_USAGE_STAGING;
	textureDescription.BindFlags = 0;
	textureDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	ComPtr<ID3D11Texture2D> staging;
	Check(
		device->CreateTexture2D(
			&textureDescription,
			nullptr,
			staging.GetAddressOf()),
		"CreateTexture2D(staging)");
	context->CopyResource(staging.Get(), renderTarget.Get());
	D3D11_MAPPED_SUBRESOURCE mapped{};
	Check(
		context->Map(
			staging.Get(),
			0,
			D3D11_MAP_READ,
			0,
			&mapped),
		"Map(staging)");

	const auto center = Pixel(mapped, 77, 70);
	const auto annulus = Pixel(mapped, 77, 22);
	const auto outside = Pixel(mapped, 4, 4);
	context->Unmap(staging.Get(), 0);

	const auto isCyan = [](const auto& pixel) {
		return pixel[0] < 8 && pixel[1] > 190 && pixel[2] > 240;
	};
	const auto isBlack = [](const auto& pixel) {
		return pixel[0] < 8 && pixel[1] < 8 && pixel[2] < 8;
	};
	if (!isCyan(center) || !isCyan(annulus) || !isBlack(outside)) {
		std::cerr << std::format(
			"ScopeFade fill failed: center=({}, {}, {}), "
			"annulus=({}, {}, {}), outside=({}, {}, {})\n",
			center[0],
			center[1],
			center[2],
			annulus[0],
			annulus[1],
			annulus[2],
			outside[0],
			outside[1],
			outside[2]);
		return 1;
	}

	// Exercise the Stage 4e.2 shader through the same ScopeFade geometry. The
	// source uses a two-axis gradient so a 2x sample can be distinguished from
	// both an identity copy and a flat diagnostic color. The value comes from
	// b4, which proves the profile/editor scalar reaches the shader contract.
	std::vector<std::uint8_t> sourcePixels(
		static_cast<std::size_t>(kWidth) * kHeight * 4);
	for (std::uint32_t y = 0; y < kHeight; ++y) {
		for (std::uint32_t x = 0; x < kWidth; ++x) {
			const auto pixelOffset =
				(static_cast<std::size_t>(y) * kWidth + x) * 4;
			sourcePixels[pixelOffset] =
				static_cast<std::uint8_t>(x * 2);
			sourcePixels[pixelOffset + 1] =
				static_cast<std::uint8_t>(y * 2);
			sourcePixels[pixelOffset + 2] = 32;
			sourcePixels[pixelOffset + 3] = 255;
		}
	}

	const D3D11_SUBRESOURCE_DATA sourceData{
		sourcePixels.data(),
		kWidth * 4,
		kWidth * kHeight * 4
	};
	context->UpdateSubresource(
		renderTarget.Get(),
		0,
		nullptr,
		sourceData.pSysMem,
		sourceData.SysMemPitch,
		sourceData.SysMemSlicePitch);

	// Reproduce the live hook contract: the scene texture is still the active
	// RTV source when it is copied to a distinct shader-resource texture.
	D3D11_TEXTURE2D_DESC sceneCopyDescription{};
	sceneCopyDescription.Width = kWidth;
	sceneCopyDescription.Height = kHeight;
	sceneCopyDescription.MipLevels = 1;
	sceneCopyDescription.ArraySize = 1;
	sceneCopyDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sceneCopyDescription.SampleDesc.Count = 1;
	sceneCopyDescription.Usage = D3D11_USAGE_DEFAULT;
	sceneCopyDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	ComPtr<ID3D11Texture2D> sceneCopy;
	ComPtr<ID3D11ShaderResourceView> sourceView;
	Check(
		device->CreateTexture2D(
			&sceneCopyDescription,
			nullptr,
			sceneCopy.GetAddressOf()),
		"CreateTexture2D(scene copy)");
	context->CopyResource(sceneCopy.Get(), renderTarget.Get());
	Check(
		device->CreateShaderResourceView(
			sceneCopy.Get(),
			nullptr,
			sourceView.GetAddressOf()),
		"CreateShaderResourceView(scene copy)");

	D3D11_SAMPLER_DESC samplerDescription{};
	samplerDescription.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
	samplerDescription.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDescription.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDescription.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDescription.MaxLOD = D3D11_FLOAT32_MAX;
	ComPtr<ID3D11SamplerState> sampler;
	Check(
		device->CreateSamplerState(
			&samplerDescription,
			sampler.GetAddressOf()),
		"CreateSamplerState");

	ResolutionConstants resolution{};
	resolution.width = static_cast<float>(kWidth);
	resolution.height = static_cast<float>(kHeight);
	resolution.magnification = 2.0F;
	resolution.aimCenterX =
		(kCenterNdcX * 0.5F + 0.5F) * static_cast<float>(kWidth);
	resolution.aimCenterY =
		(0.5F - kCenterNdcY * 0.5F) * static_cast<float>(kHeight);
	resolution.lensBasisXX = 0.80F * 0.5F * static_cast<float>(kWidth);
	resolution.lensBasisXY = 0.0F;
	resolution.lensBasisZX = 0.0F;
	resolution.lensBasisZY = -0.80F * 0.5F * static_cast<float>(kHeight);
	D3D11_BUFFER_DESC resolutionDescription{};
	resolutionDescription.ByteWidth = sizeof(resolution);
	resolutionDescription.Usage = D3D11_USAGE_DYNAMIC;
	resolutionDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	resolutionDescription.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	ComPtr<ID3D11Buffer> resolutionBuffer;
	Check(
		device->CreateBuffer(
			&resolutionDescription,
			nullptr,
			resolutionBuffer.GetAddressOf()),
		"CreateBuffer(resolution)");

	const auto updateResolution = [&](const ResolutionConstants& constants) {
		D3D11_MAPPED_SUBRESOURCE constantMapping{};
		Check(
			context->Map(
				resolutionBuffer.Get(),
				0,
				D3D11_MAP_WRITE_DISCARD,
				0,
				&constantMapping),
			"Map(resolution)");
		std::memcpy(
			constantMapping.pData,
			&constants,
			sizeof(constants));
		context->Unmap(resolutionBuffer.Get(), 0);
	};
	updateResolution(resolution);

	context->ClearRenderTargetView(renderTargetView.Get(), clear);
	context->PSSetShader(magnifyShader.Get(), nullptr, 0);
	ID3D11ShaderResourceView* boundSource = sourceView.Get();
	ID3D11SamplerState* boundSampler = sampler.Get();
	ID3D11Buffer* boundResolution = resolutionBuffer.Get();
	context->PSSetShaderResources(4, 1, &boundSource);
	context->PSSetSamplers(0, 1, &boundSampler);
	context->PSSetConstantBuffers(4, 1, &boundResolution);
	context->DrawIndexed(static_cast<UINT>(indices.size()), 0, 0);

	context->CopyResource(staging.Get(), renderTarget.Get());
	Check(
		context->Map(
			staging.Get(),
			0,
			D3D11_MAP_READ,
			0,
			&mapped),
		"Map(staging magnification)");
	const auto magnifiedCenter = Pixel(mapped, 77, 70);
	const auto magnifiedRight = Pixel(mapped, 100, 70);
	// Keep the vertical magnification sample inside the optical rim. The
	// shipping shader now has a deliberate edge vignette, so sampling near
	// the aperture boundary would test darkening rather than UV scale.
	const auto magnifiedTop = Pixel(mapped, 77, 48);
	const auto magnifiedOutside = Pixel(mapped, 4, 4);
	context->Unmap(staging.Get(), 0);

	const bool centerSamplesCenter =
		magnifiedCenter[0] >= 150 && magnifiedCenter[0] <= 158 &&
		magnifiedCenter[1] >= 136 && magnifiedCenter[1] <= 144;
	const bool rightIsTwoTimesMagnified =
		magnifiedRight[0] >= 172 && magnifiedRight[0] <= 180 &&
		magnifiedRight[1] >= 136 && magnifiedRight[1] <= 144;
	const bool topIsTwoTimesMagnified =
		magnifiedTop[0] >= 150 && magnifiedTop[0] <= 158 &&
		magnifiedTop[1] >= 114 && magnifiedTop[1] <= 122;
	if (!centerSamplesCenter ||
		!rightIsTwoTimesMagnified ||
		!topIsTwoTimesMagnified ||
		!isBlack(magnifiedOutside)) {
		std::cerr << std::format(
			"ScopeFade magnification failed: center=({}, {}, {}), "
			"right=({}, {}, {}), top=({}, {}, {}), "
			"outside=({}, {}, {})\n",
			magnifiedCenter[0],
			magnifiedCenter[1],
			magnifiedCenter[2],
			magnifiedRight[0],
			magnifiedRight[1],
			magnifiedRight[2],
			magnifiedTop[0],
			magnifiedTop[1],
			magnifiedTop[2],
			magnifiedOutside[0],
			magnifiedOutside[1],
			magnifiedOutside[2]);
		return 1;
	}

	// Real STS meshes store ScopeFade positions in packed formats. Their inner
	// ring is only approximately half the outer ring, which made the retired
	// per-primitive center/frame contract expose every triangle as a radial
	// wedge. Compare an intentionally packed-error fixture against the ideal
	// fixture at high magnification. A draw-wide pivot plus perspective-correct
	// lens coordinates must produce the same scene sample on both sides of all
	// 24 radial seams and throughout the generated center fill.
	resolution.magnification = 8.0F;
	resolution.aimOffsetValid = 0.0F;
	updateResolution(resolution);
	const auto renderFacetSamples = [&]() {
		context->ClearRenderTargetView(renderTargetView.Get(), clear);
		context->DrawIndexed(static_cast<UINT>(indices.size()), 0, 0);
		context->CopyResource(staging.Get(), renderTarget.Get());
		Check(
			context->Map(
				staging.Get(),
				0,
				D3D11_MAP_READ,
				0,
				&mapped),
			"Map(staging packed ScopeFade continuity)");

		std::vector<std::array<std::uint8_t, 4>> samples;
		for (std::uint32_t segment = 0; segment < kSegments; ++segment) {
			const float angle =
				static_cast<float>(segment) *
				6.28318530717958647692F /
				static_cast<float>(kSegments);
			const float radialX = std::sin(angle);
			const float radialY = -std::cos(angle);
			const float tangentX = std::cos(angle);
			const float tangentY = std::sin(angle);
			for (const float tangentOffset : { -1.0F, 0.0F, 1.0F }) {
				const auto x = static_cast<std::uint32_t>(std::lround(
					resolution.aimCenterX + radialX * 35.0F +
					tangentX * tangentOffset));
				const auto y = static_cast<std::uint32_t>(std::lround(
					resolution.aimCenterY + radialY * 35.0F +
					tangentY * tangentOffset));
				samples.push_back(Pixel(mapped, x, y));
			}
		}
		for (int y = -2; y <= 2; ++y) {
			for (int x = -2; x <= 2; ++x) {
				samples.push_back(Pixel(
					mapped,
					static_cast<std::uint32_t>(
						std::lround(resolution.aimCenterX) + x),
					static_cast<std::uint32_t>(
						std::lround(resolution.aimCenterY) + y)));
			}
		}
		context->Unmap(staging.Get(), 0);
		return samples;
	};

	const auto idealFacetSamples = renderFacetSamples();
	std::array<Vertex, kSegments * 2> packedErrorVertices{};
	for (std::uint32_t segment = 0; segment < kSegments; ++segment) {
		const float angle =
			static_cast<float>(segment) *
			6.28318530717958647692F /
			static_cast<float>(kSegments);
		const float sine = std::sin(angle);
		const float cosine = std::cos(angle);
		// The radial variation is deliberately small (below one percent), but
		// larger than one 128-pixel fixture subpixel so the old flat-frame seam
		// becomes deterministic rather than depending on raster rounding.
		const float outerRadius =
			0.80F + 0.0008F * std::sin(angle * 3.0F + 0.2F);
		const float innerRadius =
			0.40F + 0.0030F * std::cos(angle * 5.0F - 0.1F);
		packedErrorVertices[segment * 2] = {
			kCenterNdcX + sine * outerRadius,
			kCenterNdcY + cosine * outerRadius,
			0.5F
		};
		packedErrorVertices[segment * 2 + 1] = {
			kCenterNdcX + sine * innerRadius,
			kCenterNdcY + cosine * innerRadius,
			0.5F
		};
	}
	D3D11_MAPPED_SUBRESOURCE packedMapping{};
	Check(
		context->Map(
			vertexBuffer.Get(),
			0,
			D3D11_MAP_WRITE_DISCARD,
			0,
			&packedMapping),
		"Map(packed-error ScopeFade fixture)");
	std::memcpy(
		packedMapping.pData,
		packedErrorVertices.data(),
		sizeof(packedErrorVertices));
	context->Unmap(vertexBuffer.Get(), 0);
	const auto packedFacetSamples = renderFacetSamples();
	if (idealFacetSamples.size() != packedFacetSamples.size()) {
		throw std::runtime_error("packed ScopeFade sample count changed");
	}
	int maximumFacetDelta = 0;
	for (std::size_t sample = 0; sample < idealFacetSamples.size(); ++sample) {
		for (std::size_t channel = 0; channel < 3; ++channel) {
			maximumFacetDelta = std::max(
				maximumFacetDelta,
				std::abs(
					static_cast<int>(idealFacetSamples[sample][channel]) -
					static_cast<int>(packedFacetSamples[sample][channel])));
		}
	}
	if (maximumFacetDelta > 2) {
		std::cerr << std::format(
			"Packed ScopeFade continuity failed: maximum channel delta={}\n",
			maximumFacetDelta);
		return 1;
	}

	D3D11_MAPPED_SUBRESOURCE restoreMapping{};
	Check(
		context->Map(
			vertexBuffer.Get(),
			0,
			D3D11_MAP_WRITE_DISCARD,
			0,
			&restoreMapping),
		"Map(restore ideal ScopeFade fixture)");
	std::memcpy(restoreMapping.pData, vertices.data(), sizeof(vertices));
	context->Unmap(vertexBuffer.Get(), 0);
	resolution.magnification = 2.0F;

	// Reticle-aligned optics keep the GPU ScopeFade center for geometry, but
	// offset the scene-sampling pivot by the authored reticle-minus-lens
	// relationship. The constant buffer stores local X/Z radii so this offset
	// rotates with the exact draw-time lens basis. Segment zero reconstructs a
	// 51.2-pixel X basis and a 51.2-pixel upward Z basis in this fixture.
	resolution.aimOffsetValid = 1.0F;
	resolution.aimOffsetX = 8.0F / 51.2F;
	resolution.aimOffsetY = 4.0F / 51.2F;
	resolution.aimCenterX = 85.0F;
	resolution.aimCenterY = 66.0F;
	updateResolution(resolution);
	context->ClearRenderTargetView(renderTargetView.Get(), clear);
	context->DrawIndexed(static_cast<UINT>(indices.size()), 0, 0);
	context->CopyResource(staging.Get(), renderTarget.Get());
	Check(
		context->Map(
			staging.Get(),
			0,
			D3D11_MAP_READ,
			0,
			&mapped),
		"Map(staging aim offset)");
	const auto alignedPivot = Pixel(mapped, 85, 66);
	const auto alignedLensCenter = Pixel(mapped, 77, 70);
	context->Unmap(staging.Get(), 0);
	const bool pivotRemainsInvariant =
		alignedPivot[0] >= 166 && alignedPivot[0] <= 174 &&
		alignedPivot[1] >= 128 && alignedPivot[1] <= 136;
	const bool lensCenterFollowsOffsetPivot =
		alignedLensCenter[0] >= 158 && alignedLensCenter[0] <= 166 &&
		alignedLensCenter[1] >= 132 && alignedLensCenter[1] <= 140;
	if (!pivotRemainsInvariant || !lensCenterFollowsOffsetPivot) {
		std::cerr << std::format(
			"Reticle-aligned sampling failed: pivot=({}, {}, {}), "
			"lensCenter=({}, {}, {})\n",
			alignedPivot[0],
			alignedPivot[1],
			alignedPivot[2],
			alignedLensCenter[0],
			alignedLensCenter[1],
			alignedLensCenter[2]);
		return 1;
	}

	// At 1x, the scope image remains an identity copy regardless of the
	// authored reticle offset.
	resolution.magnification = 1.0F;
	updateResolution(resolution);
	context->ClearRenderTargetView(renderTargetView.Get(), clear);
	// The production reticle substitution supports both hooked entry points.
	// Earlier checks exercise DrawIndexed; the identity check uses the
	// one-instance DrawIndexedInstanced equivalent on the WARP device.
	context->DrawIndexedInstanced(
		static_cast<UINT>(indices.size()),
		1U,
		0U,
		0,
		0U);
	context->CopyResource(staging.Get(), renderTarget.Get());
	Check(
		context->Map(
			staging.Get(),
			0,
			D3D11_MAP_READ,
			0,
			&mapped),
		"Map(staging identity)");
	const auto identitySample = Pixel(mapped, 100, 70);
	context->Unmap(staging.Get(), 0);
	if (identitySample[0] < 196 || identitySample[0] > 204 ||
		identitySample[1] < 136 || identitySample[1] > 144) {
		std::cerr << std::format(
			"1x identity sampling failed: sample=({}, {}, {})\n",
			identitySample[0],
			identitySample[1],
			identitySample[2]);
		return 1;
	}

	// A single radial edge cannot recover a perspective-complete projected
	// lens plane. Submit a rolled/sheared ScopeFade whose X/Z columns have
	// unequal scale and independent homogeneous W gradients. The authored
	// local reticle pivot must remain invariant under magnification. The old
	// forced-perpendicular basis places this pivot more than ten pixels away
	// and fails this fixture.
	const Vertex projectiveCenter{
		0.0F,
		0.0F,
		0.60F,
		1.20F
	};
	const Vertex projectiveColumnX{
		1.125F,
		-0.5625F,
		0.02F,
		0.18F
	};
	const Vertex projectiveColumnZ{
		0.1875F,
		0.5625F,
		-0.03F,
		-0.12F
	};
	submitProjectiveLensPose(
		projectiveCenter,
		projectiveColumnX,
		projectiveColumnZ);
	const float projectiveAimX = 0.60F;
	const float projectiveAimZ = 0.20F;
	const Vertex projectiveAimClip{
		projectiveCenter.x +
			projectiveColumnX.x * projectiveAimX +
			projectiveColumnZ.x * projectiveAimZ,
		projectiveCenter.y +
			projectiveColumnX.y * projectiveAimX +
			projectiveColumnZ.y * projectiveAimZ,
		projectiveCenter.z +
			projectiveColumnX.z * projectiveAimX +
			projectiveColumnZ.z * projectiveAimZ,
		projectiveCenter.w +
			projectiveColumnX.w * projectiveAimX +
			projectiveColumnZ.w * projectiveAimZ
	};
	const float projectivePivotX =
		(projectiveAimClip.x / projectiveAimClip.w * 0.5F + 0.5F) *
		static_cast<float>(kWidth);
	const float projectivePivotY =
		(0.5F - projectiveAimClip.y / projectiveAimClip.w * 0.5F) *
		static_cast<float>(kHeight);
	const auto projectiveSampleX = static_cast<std::uint32_t>(
		std::lround(projectivePivotX));
	const auto projectiveSampleY = static_cast<std::uint32_t>(
		std::lround(projectivePivotY));

	resolution.magnification = 2.0F;
	resolution.aimOffsetValid = 1.0F;
	resolution.aimOffsetX = projectiveAimX;
	resolution.aimOffsetY = projectiveAimZ;
	resolution.aimCenterX = projectivePivotX;
	resolution.aimCenterY = projectivePivotY;
	resolution.physicalEyeBoxValid = 0.0F;
	// This fixture isolates projective pivot recovery. Keep the fallback rim
	// outside the synthetic aperture so optical darkening cannot change the
	// expected gradient sample.
	resolution.lensRadiusX = static_cast<float>(kWidth);
	resolution.lensRadiusY = static_cast<float>(kHeight);
	resolution.vignetteReach = 20.0F;
	updateResolution(resolution);
	context->ClearRenderTargetView(renderTargetView.Get(), clear);
	context->DrawIndexed(static_cast<UINT>(indices.size()), 0, 0);
	context->CopyResource(staging.Get(), renderTarget.Get());
	Check(
		context->Map(
			staging.Get(),
			0,
			D3D11_MAP_READ,
			0,
			&mapped),
		"Map(staging projective frame)");
	const auto projectivePivot =
		Pixel(mapped, projectiveSampleX, projectiveSampleY);
	context->Unmap(staging.Get(), 0);
	const int expectedProjectiveRed =
		static_cast<int>(std::lround(projectivePivotX * 2.0F));
	const int expectedProjectiveGreen =
		static_cast<int>(std::lround(projectivePivotY * 2.0F));
	if (std::abs(
			static_cast<int>(projectivePivot[0]) -
			expectedProjectiveRed) > 5 ||
		std::abs(
			static_cast<int>(projectivePivot[1]) -
			expectedProjectiveGreen) > 5) {
		std::cerr << std::format(
			"Projective ScopeFade pivot failed: pixel=({}, {}, {}), "
			"expected~=({}, {}), pivot=({:.2f}, {:.2f})\n",
			projectivePivot[0],
			projectivePivot[1],
			projectivePivot[2],
			expectedProjectiveRed,
			expectedProjectiveGreen,
			projectivePivotX,
			projectivePivotY);
		return 1;
	}

	// Prove the optical-lag sign with a directional source rather than a
	// uniform lens. Positive ScopeFade-local eye travel must counter-shift the
	// sampled scene toward lower source X, while negative travel must shift it
	// toward higher source X. Setting OpticalLagStrength to zero must restore
	// the neutral sample even when eye-motion telemetry is non-zero.
	submitLensPose(-kCenterNdcX, -kCenterNdcY);
	resolution.magnification = 1.0F;
	resolution.aimOffsetValid = 0.0F;
	resolution.sceneParallaxStrength = 1.0F;
	resolution.physicalEyeBoxValid = 1.0F;
	resolution.lensRadiusX = 40.0F;
	resolution.lensRadiusY = 40.0F;
	resolution.lensBasisXX = 40.0F;
	resolution.lensBasisXY = 0.0F;
	resolution.lensBasisZX = 0.0F;
	resolution.lensBasisZY = 40.0F;
	resolution.eyeBoxRadius = 2.0F;
	resolution.vignetteReach = 9.0F;
	resolution.vignetteSharpness = 3.0F;
	resolution.eyeBoxMaxTravel = 4.0F;

	const auto renderOpticalLagSample = [&]() {
		updateResolution(resolution);
		context->ClearRenderTargetView(renderTargetView.Get(), clear);
		context->DrawIndexed(static_cast<UINT>(indices.size()), 0, 0);
		context->CopyResource(staging.Get(), renderTarget.Get());
		Check(
			context->Map(
				staging.Get(),
				0,
				D3D11_MAP_READ,
				0,
				&mapped),
			"Map(staging optical lag)");
		const auto sample = Pixel(mapped, kWidth / 2, kHeight / 2);
		context->Unmap(staging.Get(), 0);
		return sample;
	};

	resolution.eyeOffsetX = 0.0F;
	resolution.eyeOffsetY = 0.0F;
	resolution.opticalLagStrength = 1.0F;
	const auto neutralOpticalSample = renderOpticalLagSample();

	resolution.eyeOffsetX = 0.30F;
	resolution.opticalLagStrength = 0.0F;
	const auto disabledOpticalSample = renderOpticalLagSample();

	resolution.opticalLagStrength = 1.0F;
	const auto positiveOpticalSample = renderOpticalLagSample();

	resolution.eyeOffsetX = -0.30F;
	const auto negativeOpticalSample = renderOpticalLagSample();

	const auto channelDifference = [](std::uint8_t left, std::uint8_t right) {
		return std::abs(static_cast<int>(left) - static_cast<int>(right));
	};
	const bool zeroStrengthIsNeutral =
		channelDifference(
			disabledOpticalSample[0], neutralOpticalSample[0]) <= 3 &&
		channelDifference(
			disabledOpticalSample[1], neutralOpticalSample[1]) <= 3;
	const bool positiveTravelLagsLeft =
		static_cast<int>(positiveOpticalSample[0]) + 10 <
		static_cast<int>(neutralOpticalSample[0]);
	const bool negativeTravelLagsRight =
		static_cast<int>(negativeOpticalSample[0]) >
		static_cast<int>(neutralOpticalSample[0]) + 10;
	const bool verticalChannelRemainsStable =
		channelDifference(
			positiveOpticalSample[1], neutralOpticalSample[1]) <= 3 &&
		channelDifference(
			negativeOpticalSample[1], neutralOpticalSample[1]) <= 3;
	if (!zeroStrengthIsNeutral ||
		!positiveTravelLagsLeft ||
		!negativeTravelLagsRight ||
		!verticalChannelRemainsStable) {
		std::cerr << std::format(
			"Optical lag direction failed: neutral=({}, {}, {}), "
			"disabled=({}, {}, {}), positive=({}, {}, {}), "
			"negative=({}, {}, {})\n",
			neutralOpticalSample[0],
			neutralOpticalSample[1],
			neutralOpticalSample[2],
			disabledOpticalSample[0],
			disabledOpticalSample[1],
			disabledOpticalSample[2],
			positiveOpticalSample[0],
			positiveOpticalSample[1],
			positiveOpticalSample[2],
			negativeOpticalSample[0],
			negativeOpticalSample[1],
			negativeOpticalSample[2]);
		return 1;
	}

	// ScopeFade vertices define the current physical lens frame, but absolute
	// screen position must not create eye travel. Moving the entire optic while
	// leaving the local-motion constants at zero therefore remains centered.
	// A separate +0.30-radius local eye displacement must create the one-sided
	// shadow. This is the production contract that lets pitch/yaw recenter
	// while recoil and first-person inertia remain visible.
	//
	// Replace the earlier two-axis magnification fixture with a uniform source
	// for this check. Otherwise translating the aperture also changes the
	// gradient value beneath each sample, and that source-color change can
	// overwhelm (or falsely imitate) the directional pupil shadow.
	std::vector<std::uint8_t> uniformSourcePixels(
		static_cast<std::size_t>(kWidth) * kHeight * 4);
	for (std::size_t pixel = 0;
		 pixel < static_cast<std::size_t>(kWidth) * kHeight;
		 ++pixel) {
		uniformSourcePixels[pixel * 4] = 192U;
		uniformSourcePixels[pixel * 4 + 1] = 192U;
		uniformSourcePixels[pixel * 4 + 2] = 192U;
		uniformSourcePixels[pixel * 4 + 3] = 255U;
	}
	context->UpdateSubresource(
		sceneCopy.Get(),
		0,
		nullptr,
		uniformSourcePixels.data(),
		kWidth * 4,
		kWidth * kHeight * 4);
	resolution.aimOffsetValid = 0.0F;
	resolution.eyeOffsetX = 0.0F;
	resolution.eyeOffsetY = 0.0F;
	resolution.opticalLagStrength = 1.0F;
	resolution.sceneParallaxStrength = 0.0F;
	resolution.physicalEyeBoxValid = 1.0F;
	// Restore the actual 40-pixel fixture radius after the preceding
	// projective-pivot check deliberately moved the fallback rim outside the
	// render target.
	resolution.lensRadiusX = 40.0F;
	resolution.lensRadiusY = 40.0F;
	resolution.lensBasisXX = 40.0F;
	resolution.lensBasisXY = 0.0F;
	resolution.lensBasisZX = 0.0F;
	resolution.lensBasisZY = 40.0F;
	// Use a deliberately constrained exit pupil so a 0.30-radius synthetic
	// lens displacement produces a measurable one-sided shadow. Production's
	// wider default is tested for stability rather than maximum contrast.
	resolution.eyeBoxRadius = 1.5F;
	resolution.vignetteReach = 10.0F;
	resolution.vignetteSharpness = 3.0F;
	resolution.eyeBoxMaxTravel = 1.0F;
	updateResolution(resolution);
	submitLensPose(-kCenterNdcX, -kCenterNdcY);
	context->ClearRenderTargetView(renderTargetView.Get(), clear);
	context->DrawIndexed(static_cast<UINT>(indices.size()), 0, 0);
	context->CopyResource(staging.Get(), renderTarget.Get());
	Check(
		context->Map(
			staging.Get(),
			0,
			D3D11_MAP_READ,
			0,
			&mapped),
		"Map(staging neutral physical eye-box)");
	// Keep the neutral reference comfortably inside the exit pupil. Sampling
	// exactly on the feather made this control sensitive to the 24-sided
	// ScopeFade raster boundary and could report a false directional bias.
	// The displaced samples below remain on the feather and prove direction.
	const auto neutralPupilLeft = Pixel(mapped, 44, 64);
	const auto neutralPupilRight = Pixel(mapped, 84, 64);
	context->Unmap(staging.Get(), 0);

	// The outer radius is 0.80 NDC, so 0.24 NDC translates the whole lens by
	// 0.30 local radii. Zero local eye motion must remain centered.
	submitLensPose(-kCenterNdcX + 0.24F, -kCenterNdcY);
	context->ClearRenderTargetView(renderTargetView.Get(), clear);
	context->DrawIndexed(static_cast<UINT>(indices.size()), 0, 0);
	context->CopyResource(staging.Get(), renderTarget.Get());
	Check(
		context->Map(
			staging.Get(),
			0,
			D3D11_MAP_READ,
			0,
			&mapped),
		"Map(staging translated neutral eye-box)");
	const auto translatedNeutralLeft = Pixel(mapped, 59, 64);
	const auto translatedNeutralRight = Pixel(mapped, 99, 64);
	context->Unmap(staging.Get(), 0);

	// Now publish actual local eye/optic motion without moving the authored
	// geometry again. The exit pupil and scene parallax must respond to this
	// transient signal rather than to the lens's monitor-space position.
	resolution.eyeOffsetX = 0.30F;
	updateResolution(resolution);
	context->ClearRenderTargetView(renderTargetView.Get(), clear);
	context->DrawIndexed(static_cast<UINT>(indices.size()), 0, 0);
	context->CopyResource(staging.Get(), renderTarget.Get());
	Check(
		context->Map(
			staging.Get(),
			0,
			D3D11_MAP_READ,
			0,
			&mapped),
		"Map(staging displaced physical eye-box)");
	const auto pupilLeft = Pixel(mapped, 49, 64);
	const auto pupilRight = Pixel(mapped, 109, 64);
	const auto physicalOutside = Pixel(mapped, 4, 4);
	context->Unmap(staging.Get(), 0);
	const auto brightness = [](const std::array<std::uint8_t, 4>& pixel) {
		return static_cast<unsigned>(pixel[0]) +
		       static_cast<unsigned>(pixel[1]) +
		       static_cast<unsigned>(pixel[2]);
	};
	const auto neutralBrightnessDifference = std::abs(
		static_cast<int>(brightness(neutralPupilLeft)) -
		static_cast<int>(brightness(neutralPupilRight)));
	const auto translatedNeutralBrightnessDifference = std::abs(
		static_cast<int>(brightness(translatedNeutralLeft)) -
		static_cast<int>(brightness(translatedNeutralRight)));
	if (brightness(pupilRight) <= brightness(pupilLeft) + 100U ||
		neutralBrightnessDifference > 8 ||
		translatedNeutralBrightnessDifference > 8 ||
		!isBlack(physicalOutside)) {
		std::cerr << std::format(
			"Local-motion physical eye-box direction failed: left=({}, {}, {}), "
			"right=({}, {}, {}), neutralLeft=({}, {}, {}), "
			"neutralRight=({}, {}, {}), translatedLeft=({}, {}, {}), "
			"translatedRight=({}, {}, {}), outside=({}, {}, {})\n",
			pupilLeft[0],
			pupilLeft[1],
			pupilLeft[2],
			pupilRight[0],
			pupilRight[1],
			pupilRight[2],
			neutralPupilLeft[0],
			neutralPupilLeft[1],
			neutralPupilLeft[2],
			neutralPupilRight[0],
			neutralPupilRight[1],
			neutralPupilRight[2],
			translatedNeutralLeft[0],
			translatedNeutralLeft[1],
			translatedNeutralLeft[2],
			translatedNeutralRight[0],
			translatedNeutralRight[1],
			translatedNeutralRight[2],
			physicalOutside[0],
			physicalOutside[1],
			physicalOutside[2]);
		return 1;
	}

	std::cout << std::format(
		"ScopeFade geometry fill PASSED: center=({}, {}, {}), "
		"annulus=({}, {}, {}), outside=({}, {}, {}); "
		"2x magnification PASSED: right=({}, {}, {}), "
		"top=({}, {}, {}); reticle pivot, 1x identity, projective "
		"lens frame, optical lag, stationary recentering, "
		"local-motion eye-box direction, and packed ScopeFade "
		"continuity (maximum channel delta={}) PASSED\n",
		center[0],
		center[1],
		center[2],
		annulus[0],
		annulus[1],
		annulus[2],
		outside[0],
		outside[1],
		outside[2],
		magnifiedRight[0],
		magnifiedRight[1],
		magnifiedRight[2],
		magnifiedTop[0],
		magnifiedTop[1],
		magnifiedTop[2],
		maximumFacetDelta);
	return 0;
} catch (const std::exception& exception) {
	std::cerr << exception.what() << '\n';
	return 2;
}
