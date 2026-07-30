#include <Windows.h>
#include <d3d11.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{
	constexpr std::uint32_t kWidth = 256;
	constexpr std::uint32_t kHeight = 256;

	void Check(HRESULT result, std::string_view operation)
	{
		if (FAILED(result)) {
			throw std::runtime_error(
				std::format("{} failed with HRESULT 0x{:08X}", operation, static_cast<std::uint32_t>(result)));
		}
	}

	ComPtr<ID3DBlob> LoadPackagedShader(
		const std::filesystem::path& artifact)
	{
		if (!std::filesystem::is_regular_file(artifact)) {
			throw std::runtime_error(
				std::format("packaged shader is missing: {}", artifact.string()));
		}
		ComPtr<ID3DBlob> shader;
		Check(
			D3DReadFileToBlob(artifact.c_str(), shader.GetAddressOf()),
			std::format("D3DReadFileToBlob({})", artifact.filename().string()));
		return shader;
	}

	struct Vertex
	{
		float position[3];
		float uv[2];
	};

	struct alignas(16) ResolutionData
	{
		float width{ static_cast<float>(kWidth) };
		float height{ static_cast<float>(kHeight) };
		float padding[2]{};
	};

	struct Float2
	{
		float x{};
		float y{};
	};

	struct Float3
	{
		float x{};
		float y{};
		float z{};
	};

	struct Float4
	{
		float x{};
		float y{};
		float z{};
		float w{};
	};

	struct Matrix4
	{
		float value[4][4]{};
	};

	// Byte-for-byte mirror of ScopeEffectData in Triangle.hlsli. Keeping the
	// test independent of CommonLibF4 makes it a small renderer contract test.
	struct alignas(16) ScopeEffectData
	{
		float camDepth{ 1.0F };
		float gameFov{ 90.0F };
		float zoom{ 2.0F };
		float parallaxRadius{ 1000.0F };

		float relativeFogRadius{ 1000.0F };
		float scopeSwayAmount{ 2.0F };
		float maxTravel{ 0.0F };
		float reticleSize{ 8.0F };

		float nightVisionIntensity{ 3.0F };
		float baseWeaponPosition{};
		float movePercentage{};
		std::int32_t enableZMove{};

		std::int32_t isCircle{ 1 };
		std::int32_t enableNightVision{};
		std::int32_t enableMerge{ 1 };
		float baseAdjustFov{};

		Float2 size{ 540.0F, 540.0F };
		Float2 originalPositionOffset{};
		Float2 originalSize{};
		Float2 positionOffset{};

		Float3 eyeDirection{};
		float targetAdjustFov{};
		Float3 eyeDirectionLerp{};
		float padding2{};
		Float3 eyeTranslationLerp{};
		float padding3{};
		Float3 currentWeaponPosition{};
		float padding4{};
		Float3 currentRootPosition{};
		float padding5{};

		Matrix4 cameraRotation{};
		Float2 screenPosition{
			static_cast<float>(kWidth) * 0.5F,
			static_cast<float>(kHeight) * 0.5F
		};
		Float2 reticleOffset{};
		Matrix4 projection{};
		Float4 rectangle{};

		float fishEyeStrength{ 0.35F };
		float fishEyePower{ 2.0F };
		Float2 padding6{};
	};
	static_assert(sizeof(ScopeEffectData) % 16 == 0);

	template <class T>
	ComPtr<ID3D11Buffer> CreateConstantBuffer(ID3D11Device* device, const T& data)
	{
		D3D11_BUFFER_DESC description{};
		description.ByteWidth = static_cast<UINT>(sizeof(T));
		description.Usage = D3D11_USAGE_DEFAULT;
		description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		D3D11_SUBRESOURCE_DATA initial{};
		initial.pSysMem = &data;

		ComPtr<ID3D11Buffer> buffer;
		Check(
			device->CreateBuffer(&description, &initial, buffer.GetAddressOf()),
			"CreateBuffer(constant)");
		return buffer;
	}

	std::uint64_t HashRegion(
		const std::vector<std::uint32_t>& pixels,
		std::uint32_t left,
		std::uint32_t top,
		std::uint32_t right,
		std::uint32_t bottom)
	{
		std::uint64_t hash = 1469598103934665603ULL;
		for (auto y = top; y < bottom; ++y) {
			for (auto x = left; x < right; ++x) {
				auto value = pixels[y * kWidth + x];
				for (std::size_t byte = 0; byte < sizeof(value); ++byte) {
					hash ^= static_cast<std::uint8_t>(value >> (byte * 8));
					hash *= 1099511628211ULL;
				}
			}
		}
		return hash;
	}
}

int wmain()
{
	try {
		const auto project = std::filesystem::current_path();
		const auto shaderDirectory =
			project / "Package" / "MagnaScope" / "Shaders" / "MagnaScope";
		const auto vertexShaderBlob =
			LoadPackagedShader(shaderDirectory / "ScopeEffect_VS_Legacy.cso");
		const auto pixelShaderBlob =
			LoadPackagedShader(shaderDirectory / "AutoSTS_PS.cso");
		ComPtr<ID3D11ShaderReflection> reflection;
		Check(
			D3DReflect(
				pixelShaderBlob->GetBufferPointer(),
				pixelShaderBlob->GetBufferSize(),
				IID_PPV_ARGS(reflection.GetAddressOf())),
			"D3DReflect(AutoSTS)");
		auto* reflectedScopeBuffer =
			reflection->GetConstantBufferByName("ScopeEffectData");
		D3D11_SHADER_BUFFER_DESC reflectedScopeDescription{};
		Check(
			reflectedScopeBuffer->GetDesc(&reflectedScopeDescription),
			"GetDesc(ScopeEffectData)");
		if (reflectedScopeDescription.Size != sizeof(ScopeEffectData)) {
			throw std::runtime_error(
				std::format(
					"ScopeEffectData layout mismatch: HLSL={} C++={}",
					reflectedScopeDescription.Size,
					sizeof(ScopeEffectData)));
		}

		D3D_FEATURE_LEVEL featureLevel{};
		ComPtr<ID3D11Device> device;
		ComPtr<ID3D11DeviceContext> context;
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

		ComPtr<ID3D11VertexShader> vertexShader;
		Check(
			device->CreateVertexShader(
				vertexShaderBlob->GetBufferPointer(),
				vertexShaderBlob->GetBufferSize(),
				nullptr,
				vertexShader.GetAddressOf()),
			"CreateVertexShader");
		ComPtr<ID3D11PixelShader> pixelShader;
		Check(
			device->CreatePixelShader(
				pixelShaderBlob->GetBufferPointer(),
				pixelShaderBlob->GetBufferSize(),
				nullptr,
				pixelShader.GetAddressOf()),
			"CreatePixelShader");

		constexpr std::array<D3D11_INPUT_ELEMENT_DESC, 2> inputElements{ {
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
			  D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12,
			  D3D11_INPUT_PER_VERTEX_DATA, 0 }
		} };
		ComPtr<ID3D11InputLayout> inputLayout;
		Check(
			device->CreateInputLayout(
				inputElements.data(),
				static_cast<UINT>(inputElements.size()),
				vertexShaderBlob->GetBufferPointer(),
				vertexShaderBlob->GetBufferSize(),
				inputLayout.GetAddressOf()),
			"CreateInputLayout");

		constexpr std::array<Vertex, 3> vertices{ {
			{ { 0.0F, 0.0F, 0.0F }, { 0.0F, 0.0F } },
			{ { 0.0F, 0.0F, 0.0F }, { 0.0F, 2.0F } },
			{ { 0.0F, 0.0F, 0.0F }, { 2.0F, 0.0F } }
		} };
		constexpr std::array<std::uint32_t, 3> indices{ 0, 1, 2 };

		D3D11_BUFFER_DESC vertexDescription{};
		vertexDescription.ByteWidth = sizeof(vertices);
		vertexDescription.Usage = D3D11_USAGE_IMMUTABLE;
		vertexDescription.BindFlags = D3D11_BIND_VERTEX_BUFFER;
		D3D11_SUBRESOURCE_DATA vertexInitial{ vertices.data() };
		ComPtr<ID3D11Buffer> vertexBuffer;
		Check(
			device->CreateBuffer(
				&vertexDescription,
				&vertexInitial,
				vertexBuffer.GetAddressOf()),
			"CreateBuffer(vertex)");

		D3D11_BUFFER_DESC indexDescription{};
		indexDescription.ByteWidth = sizeof(indices);
		indexDescription.Usage = D3D11_USAGE_IMMUTABLE;
		indexDescription.BindFlags = D3D11_BIND_INDEX_BUFFER;
		D3D11_SUBRESOURCE_DATA indexInitial{ indices.data() };
		ComPtr<ID3D11Buffer> indexBuffer;
		Check(
			device->CreateBuffer(
				&indexDescription,
				&indexInitial,
				indexBuffer.GetAddressOf()),
			"CreateBuffer(index)");

		std::vector<std::uint32_t> sourcePixels(kWidth * kHeight);
		for (std::uint32_t y = 0; y < kHeight; ++y) {
			for (std::uint32_t x = 0; x < kWidth; ++x) {
				const auto checker = ((x / 8) ^ (y / 8)) & 1U;
				const auto red = static_cast<std::uint8_t>((x + checker * 73U) & 0xFFU);
				const auto green = static_cast<std::uint8_t>((y + checker * 41U) & 0xFFU);
				const auto blue = static_cast<std::uint8_t>(((x * 3U + y * 5U) + checker * 29U) & 0xFFU);
				sourcePixels[y * kWidth + x] =
					0xFF000000U |
					(static_cast<std::uint32_t>(blue) << 16) |
					(static_cast<std::uint32_t>(green) << 8) |
					red;
			}
		}

		D3D11_TEXTURE2D_DESC textureDescription{};
		textureDescription.Width = kWidth;
		textureDescription.Height = kHeight;
		textureDescription.MipLevels = 1;
		textureDescription.ArraySize = 1;
		textureDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		textureDescription.SampleDesc.Count = 1;
		textureDescription.Usage = D3D11_USAGE_DEFAULT;
		textureDescription.BindFlags =
			D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		D3D11_SUBRESOURCE_DATA textureInitial{
			sourcePixels.data(),
			kWidth * sizeof(std::uint32_t),
			0
		};

		ComPtr<ID3D11Texture2D> sourceTexture;
		Check(
			device->CreateTexture2D(
				&textureDescription,
				&textureInitial,
				sourceTexture.GetAddressOf()),
			"CreateTexture2D(source)");
		ComPtr<ID3D11ShaderResourceView> sourceView;
		Check(
			device->CreateShaderResourceView(
				sourceTexture.Get(),
				nullptr,
				sourceView.GetAddressOf()),
			"CreateShaderResourceView");

		ComPtr<ID3D11Texture2D> targetTexture;
		Check(
			device->CreateTexture2D(
				&textureDescription,
				&textureInitial,
				targetTexture.GetAddressOf()),
			"CreateTexture2D(target)");
		ComPtr<ID3D11RenderTargetView> targetView;
		Check(
			device->CreateRenderTargetView(
				targetTexture.Get(),
				nullptr,
				targetView.GetAddressOf()),
			"CreateRenderTargetView");

		D3D11_TEXTURE2D_DESC stagingDescription = textureDescription;
		stagingDescription.Usage = D3D11_USAGE_STAGING;
		stagingDescription.BindFlags = 0;
		stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		ComPtr<ID3D11Texture2D> stagingTexture;
		Check(
			device->CreateTexture2D(
				&stagingDescription,
				nullptr,
				stagingTexture.GetAddressOf()),
			"CreateTexture2D(staging)");

		const auto resolutionBuffer =
			CreateConstantBuffer(device.Get(), ResolutionData{});
		ScopeEffectData scopeData{};
		for (std::size_t index = 0; index < 4; ++index) {
			scopeData.cameraRotation.value[index][index] = 1.0F;
			scopeData.projection.value[index][index] = 1.0F;
		}
		const auto scopeBuffer = CreateConstantBuffer(device.Get(), scopeData);

		D3D11_SAMPLER_DESC samplerDescription{};
		samplerDescription.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
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

		D3D11_BLEND_DESC blendDescription{};
		blendDescription.RenderTarget[0].BlendEnable = TRUE;
		blendDescription.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
		blendDescription.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
		blendDescription.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
		blendDescription.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
		blendDescription.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
		blendDescription.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
		blendDescription.RenderTarget[0].RenderTargetWriteMask =
			D3D11_COLOR_WRITE_ENABLE_ALL;
		ComPtr<ID3D11BlendState> blendState;
		Check(
			device->CreateBlendState(
				&blendDescription,
				blendState.GetAddressOf()),
			"CreateBlendState");

		D3D11_RASTERIZER_DESC rasterizerDescription{};
		rasterizerDescription.FillMode = D3D11_FILL_SOLID;
		rasterizerDescription.CullMode = D3D11_CULL_NONE;
		rasterizerDescription.DepthClipEnable = TRUE;
		rasterizerDescription.ScissorEnable = FALSE;
		ComPtr<ID3D11RasterizerState> rasterizerState;
		Check(
			device->CreateRasterizerState(
				&rasterizerDescription,
				rasterizerState.GetAddressOf()),
			"CreateRasterizerState");

		D3D11_DEPTH_STENCIL_DESC depthDescription{};
		depthDescription.DepthEnable = FALSE;
		depthDescription.StencilEnable = FALSE;
		ComPtr<ID3D11DepthStencilState> depthState;
		Check(
			device->CreateDepthStencilState(
				&depthDescription,
				depthState.GetAddressOf()),
			"CreateDepthStencilState");

		const UINT stride = sizeof(Vertex);
		const UINT offset = 0;
		ID3D11Buffer* vertexBufferRaw = vertexBuffer.Get();
		ID3D11Buffer* pixelBuffers[]{ resolutionBuffer.Get(), scopeBuffer.Get() };
		ID3D11SamplerState* samplerRaw = sampler.Get();
		ID3D11ShaderResourceView* sourceViewRaw = sourceView.Get();
		ID3D11RenderTargetView* targetViewRaw = targetView.Get();
		constexpr D3D11_VIEWPORT viewport{
			0.0F,
			0.0F,
			static_cast<float>(kWidth),
			static_cast<float>(kHeight),
			0.0F,
			1.0F
		};

		context->IASetInputLayout(inputLayout.Get());
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context->IASetVertexBuffers(0, 1, &vertexBufferRaw, &stride, &offset);
		context->IASetIndexBuffer(indexBuffer.Get(), DXGI_FORMAT_R32_UINT, 0);
		context->VSSetShader(vertexShader.Get(), nullptr, 0);
		context->PSSetShader(pixelShader.Get(), nullptr, 0);
		context->PSSetConstantBuffers(4, 2, pixelBuffers);
		context->PSSetSamplers(0, 1, &samplerRaw);
		context->PSSetShaderResources(4, 1, &sourceViewRaw);
		context->OMSetRenderTargets(1, &targetViewRaw, nullptr);
		context->OMSetBlendState(blendState.Get(), nullptr, 0xFFFFFFFFU);
		context->OMSetDepthStencilState(depthState.Get(), 0);
		context->RSSetState(rasterizerState.Get());
		context->RSSetViewports(1, &viewport);
		context->DrawIndexed(3, 0, 0);

		context->CopyResource(stagingTexture.Get(), targetTexture.Get());
		D3D11_MAPPED_SUBRESOURCE mapped{};
		Check(
			context->Map(
				stagingTexture.Get(),
				0,
				D3D11_MAP_READ,
				0,
				&mapped),
			"Map(staging)");
		std::vector<std::uint32_t> resultPixels(kWidth * kHeight);
		for (std::uint32_t y = 0; y < kHeight; ++y) {
			std::memcpy(
				resultPixels.data() + y * kWidth,
				static_cast<const std::byte*>(mapped.pData) + y * mapped.RowPitch,
				kWidth * sizeof(std::uint32_t));
		}
		context->Unmap(stagingTexture.Get(), 0);

		const auto outerBefore =
			HashRegion(sourcePixels, 0, 0, 48, 48);
		const auto outerAfter =
			HashRegion(resultPixels, 0, 0, 48, 48);
		const auto lensBefore =
			HashRegion(sourcePixels, 80, 80, 176, 176);
		const auto lensAfter =
			HashRegion(resultPixels, 80, 80, 176, 176);

		std::size_t changedInside = 0;
		std::size_t changedOutside = 0;
		for (std::uint32_t y = 0; y < kHeight; ++y) {
			for (std::uint32_t x = 0; x < kWidth; ++x) {
				const auto changed =
					sourcePixels[y * kWidth + x] != resultPixels[y * kWidth + x];
				const auto dx = static_cast<float>(x) + 0.5F - kWidth * 0.5F;
				const auto dy = static_cast<float>(y) + 0.5F - kHeight * 0.5F;
				const auto inExpectedLens = std::sqrt(dx * dx + dy * dy) <= 64.0F;
				if (changed && inExpectedLens) {
					++changedInside;
				} else if (changed) {
					++changedOutside;
				}
			}
		}

		std::cout
			<< std::format(
				"AutoSTS WARP contract (cbuffer {} bytes): outer {:016X}->{:016X}, "
				"lens {:016X}->{:016X}, changed inside={}, outside={}\n",
				reflectedScopeDescription.Size,
				outerBefore,
				outerAfter,
				lensBefore,
				lensAfter,
				changedInside,
				changedOutside);

		const bool passed =
			outerBefore == outerAfter &&
			lensBefore != lensAfter &&
			changedInside > 1000 &&
			changedOutside == 0;
		if (!passed) {
			std::cerr << "AutoSTS shader contract FAILED\n";
			return 1;
		}

		std::cout << "AutoSTS shader contract PASSED\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << "AutoSTS shader test error: " << error.what() << '\n';
		return 2;
	}
}
