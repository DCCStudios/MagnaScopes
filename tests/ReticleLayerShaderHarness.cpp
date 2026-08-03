#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace
{
	constexpr std::uint32_t kWidth = 128U;
	constexpr std::uint32_t kHeight = 128U;

	// Byte-for-byte mirror of ResolutionConstantData in Triangle.hlsli. The
	// post-composite reticle shader intentionally reads only the reticle pivot,
	// reticle scale, activation, lens basis, lens radii, and authored aim offset.
	struct alignas(16) ResolutionConstants
	{
		float width = static_cast<float>(kWidth);
		float height = static_cast<float>(kHeight);
		float sceneMagnification = 1.0F;
		float aimOffsetValid = 1.0F;

		float aimOffsetX = 0.0F;
		float aimOffsetY = 0.0F;
		float imageDenoise = 0.0F;
		float imageSharpen = 0.0F;

		float fishEyeStrength = 0.0F;
		float fishEyePower = 2.0F;
		float lensRadiusX = 56.0F;
		float lensRadiusY = 56.0F;

		float aimCenterX = 64.5F;
		float aimCenterY = 64.5F;
		float reticleMagnification = 1.0F;
		float activationProgress = 1.0F;

		float edgeRefractionStrength = 0.0F;
		float edgeRefractionWidth = 0.15F;
		float edgeChromaticAberration = 0.0F;
		float sceneParallaxStrength = 0.0F;

		float eyeOffsetX = 0.0F;
		float eyeOffsetY = 0.0F;
		float opticalLagStrength = 1.0F;
		float physicalEyeBoxValid = 1.0F;

		float lensBasisXX = 56.0F;
		float lensBasisXY = 0.0F;
		float lensBasisZX = 0.0F;
		float lensBasisZY = 56.0F;

		float eyeBoxRadius = 2.0F;
		float vignetteReach = 9.0F;
		float vignetteSharpness = 3.0F;
		float eyeBoxMaxTravel = 4.0F;

		float reticleSize = 4.0F;
		float reticleOffsetX = 0.0F;
		float reticleOffsetY = 0.0F;
		float eyeReliefDelta = 0.0F;
		float reticleShadowStrength = 0.0F;
		float reticleParallaxStrength = 1.0F;
		float lensCenterX = 64.5F;
		float lensCenterY = 64.5F;
	};
	static_assert(sizeof(ResolutionConstants) == 160U);

	struct PixelMetrics
	{
		std::uint32_t count = 0U;
		std::uint32_t minX = std::numeric_limits<std::uint32_t>::max();
		std::uint32_t minY = std::numeric_limits<std::uint32_t>::max();
		std::uint32_t maxX = 0U;
		std::uint32_t maxY = 0U;
		double centerX = 0.0;
		double centerY = 0.0;

		[[nodiscard]] std::uint32_t Width() const
		{
			return count > 0U ? maxX - minX + 1U : 0U;
		}

		[[nodiscard]] std::uint32_t Height() const
		{
			return count > 0U ? maxY - minY + 1U : 0U;
		}
	};

	struct ReticleCapturePair
	{
		std::vector<std::uint8_t> authored;
		std::vector<std::uint8_t> black;
		std::vector<std::uint8_t> white;
	};

	constexpr std::array<std::uint8_t, 4> kDestinationColor{
		47U,
		101U,
		163U,
		211U
	};

	void Check(HRESULT result, std::string_view operation)
	{
		if (FAILED(result)) {
			throw std::runtime_error(std::format(
				"{} failed: 0x{:08X}",
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
		const auto length = _ftelli64(file);
		_fseeki64(file, 0, SEEK_SET);
		std::vector<std::byte> bytes(static_cast<std::size_t>(length));
		if (fread(bytes.data(), 1, bytes.size(), file) != bytes.size()) {
			fclose(file);
			throw std::runtime_error(
				std::format("cannot read shader: {}", path.string()));
		}
		fclose(file);
		return bytes;
	}

	ReticleCapturePair MakeReticleCapturePair(
		std::uint32_t centerX,
		std::uint32_t centerY)
	{
		ReticleCapturePair captures{
			std::vector<std::uint8_t>(kWidth * kHeight * 4U, 0U),
			std::vector<std::uint8_t>(kWidth * kHeight * 4U, 0U),
			std::vector<std::uint8_t>(kWidth * kHeight * 4U, 255U)
		};
		auto write = [&captures](
						 std::uint32_t x,
						 std::uint32_t y,
						 std::array<std::uint8_t, 3> color,
						 std::uint8_t alpha) {
			if (x >= kWidth || y >= kHeight) {
				return;
			}
			const auto offset = (y * kWidth + x) * 4U;
			for (std::uint32_t channel = 0U; channel < 3U; ++channel) {
				captures.authored[offset + channel] = color[channel];
				const auto sourceContribution = static_cast<std::uint8_t>(
					std::lround(
						static_cast<double>(color[channel]) *
						static_cast<double>(alpha) / 255.0));
				captures.black[offset + channel] = sourceContribution;
				captures.white[offset + channel] = static_cast<std::uint8_t>(
					std::min(
						255U,
						static_cast<std::uint32_t>(sourceContribution) +
							255U - alpha));
			}
			captures.authored[offset + 3U] = alpha;
			// The production capture preserves the destination alpha channel.
			// Black and white backgrounds therefore remain zero and one in
			// alpha while RGB carries the source contribution/transmittance.
			captures.black[offset + 3U] = 0U;
			captures.white[offset + 3U] = 255U;
		};

		// Opaque black lines are the case that the old straight-RGBA capture
		// could not distinguish from its transparent black backing.
		for (int x = -5; x <= 6; ++x) {
			write(
				static_cast<std::uint32_t>(static_cast<int>(centerX) + x),
				centerY,
				{ 0U, 0U, 0U },
				255U);
			write(
				static_cast<std::uint32_t>(static_cast<int>(centerX) + x),
				centerY - 1U,
				{ 0U, 0U, 0U },
				96U);
		}
		for (int y = -4; y <= 5; ++y) {
			write(
				centerX,
				static_cast<std::uint32_t>(static_cast<int>(centerY) + y),
				{ 0U, 0U, 0U },
				255U);
			write(
				centerX + 1U,
				static_cast<std::uint32_t>(static_cast<int>(centerY) + y),
				{ 0U, 0U, 0U },
				96U);
		}

		// A bright, partially transparent red dot exercises emissive-looking
		// colored pixels without allowing it to hide the black cross center.
		for (std::uint32_t y = centerY - 4U; y <= centerY - 3U; ++y) {
			for (std::uint32_t x = centerX + 4U; x <= centerX + 5U; ++x) {
				write(x, y, { 255U, 24U, 12U }, 224U);
			}
		}
		return captures;
	}

	class ShaderFixture
	{
	public:
		explicit ShaderFixture(const std::filesystem::path& shaderPath)
		{
			D3D_FEATURE_LEVEL featureLevel{};
			Check(
				D3D11CreateDevice(
					nullptr,
					D3D_DRIVER_TYPE_WARP,
					nullptr,
					0U,
					nullptr,
					0U,
					D3D11_SDK_VERSION,
					device.GetAddressOf(),
					&featureLevel,
					context.GetAddressOf()),
				"D3D11CreateDevice(WARP)");

			static constexpr char vertexShaderSource[] = R"(
struct Output
{
    float4 position : SV_Position;
    float2 tex : TEXCOORD;
};
Output main(uint vertexId : SV_VertexID)
{
    const float2 positions[3] = {
        float2(-1.0f, 1.0f),
        float2(3.0f, 1.0f),
        float2(-1.0f, -3.0f)
    };
    Output output;
    output.position = float4(positions[vertexId], 0.0f, 1.0f);
    output.tex = positions[vertexId] * float2(0.5f, -0.5f) + 0.5f;
    return output;
}
)";
			ComPtr<ID3DBlob> vertexBlob;
			ComPtr<ID3DBlob> errors;
			Check(
				D3DCompile(
					vertexShaderSource,
					sizeof(vertexShaderSource),
					"ReticleLayerShaderHarness",
					nullptr,
					nullptr,
					"main",
					"vs_5_0",
					D3DCOMPILE_OPTIMIZATION_LEVEL3,
					0U,
					vertexBlob.GetAddressOf(),
					errors.GetAddressOf()),
				"D3DCompile(test VS)");
			Check(
				device->CreateVertexShader(
					vertexBlob->GetBufferPointer(),
					vertexBlob->GetBufferSize(),
					nullptr,
					vertexShader.GetAddressOf()),
				"CreateVertexShader");

			const auto shaderBytes = ReadBinary(shaderPath);
			Check(
				device->CreatePixelShader(
					shaderBytes.data(),
					shaderBytes.size(),
					nullptr,
					pixelShader.GetAddressOf()),
				"CreatePixelShader");

			D3D11_TEXTURE2D_DESC renderDescription{};
			renderDescription.Width = kWidth;
			renderDescription.Height = kHeight;
			renderDescription.MipLevels = 1U;
			renderDescription.ArraySize = 1U;
			renderDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			renderDescription.SampleDesc.Count = 1U;
			renderDescription.Usage = D3D11_USAGE_DEFAULT;
			renderDescription.BindFlags = D3D11_BIND_RENDER_TARGET;
			Check(
				device->CreateTexture2D(
					&renderDescription,
					nullptr,
					renderTarget.GetAddressOf()),
				"CreateTexture2D(output)");
			Check(
				device->CreateRenderTargetView(
					renderTarget.Get(),
					nullptr,
					renderTargetView.GetAddressOf()),
				"CreateRenderTargetView");

			D3D11_TEXTURE2D_DESC stagingDescription = renderDescription;
			stagingDescription.Usage = D3D11_USAGE_STAGING;
			stagingDescription.BindFlags = 0U;
			stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			Check(
				device->CreateTexture2D(
					&stagingDescription,
					nullptr,
					staging.GetAddressOf()),
				"CreateTexture2D(staging)");

			D3D11_BUFFER_DESC constantDescription{};
			constantDescription.ByteWidth = sizeof(ResolutionConstants);
			constantDescription.Usage = D3D11_USAGE_DEFAULT;
			constantDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			Check(
				device->CreateBuffer(
					&constantDescription,
					nullptr,
					constantBuffer.GetAddressOf()),
				"CreateBuffer(constants)");

			// The production reticle layer reads the lens and shadow depth
			// separation from ScopeEffectData at b5. Bind a valid fixture buffer
			// rather than relying on undefined state from an unbound slot.
			std::array<float, 92> scopeEffectConstants{};
			scopeEffectConstants[86] = 1.0F;
			scopeEffectConstants[87] = 1.0F;
			D3D11_BUFFER_DESC scopeEffectDescription{};
			scopeEffectDescription.ByteWidth =
				static_cast<UINT>(scopeEffectConstants.size() * sizeof(float));
			scopeEffectDescription.Usage = D3D11_USAGE_IMMUTABLE;
			scopeEffectDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			const D3D11_SUBRESOURCE_DATA scopeEffectData{
				scopeEffectConstants.data(),
				0U,
				0U
			};
			Check(
				device->CreateBuffer(
					&scopeEffectDescription,
					&scopeEffectData,
					scopeEffectBuffer.GetAddressOf()),
				"CreateBuffer(scope effect)");

			D3D11_SAMPLER_DESC samplerDescription{};
			samplerDescription.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
			samplerDescription.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
			samplerDescription.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
			samplerDescription.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			samplerDescription.MaxLOD = D3D11_FLOAT32_MAX;
			Check(
				device->CreateSamplerState(
					&samplerDescription,
					sampler.GetAddressOf()),
				"CreateSamplerState");

			D3D11_BLEND_DESC blendDescription{};
			auto& targetBlend = blendDescription.RenderTarget[0];
			targetBlend.BlendEnable = true;
			// The reticle pixel shader publishes the black-background source
			// contribution at SV_Target0 and the white-minus-black destination
			// transmittance at SV_Target1. Dual-source blending reconstructs the
			// authored draw exactly as B + destination * T without inventing a
			// straight alpha value for opaque black reticle lines.
			targetBlend.SrcBlend = D3D11_BLEND_ONE;
			targetBlend.DestBlend = D3D11_BLEND_SRC1_COLOR;
			targetBlend.BlendOp = D3D11_BLEND_OP_ADD;
			targetBlend.SrcBlendAlpha = D3D11_BLEND_ZERO;
			targetBlend.DestBlendAlpha = D3D11_BLEND_ONE;
			targetBlend.BlendOpAlpha = D3D11_BLEND_OP_ADD;
			targetBlend.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
			Check(
				device->CreateBlendState(
					&blendDescription,
					dualSourceBlend.GetAddressOf()),
				"CreateBlendState(dual-source reticle)");
		}

		std::vector<std::uint8_t> Render(
			const ResolutionConstants& constants,
			const ReticleCapturePair& captures)
		{
			D3D11_TEXTURE2D_DESC sourceDescription{};
			sourceDescription.Width = kWidth;
			sourceDescription.Height = kHeight;
			sourceDescription.MipLevels = 1U;
			sourceDescription.ArraySize = 1U;
			sourceDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			sourceDescription.SampleDesc.Count = 1U;
			sourceDescription.Usage = D3D11_USAGE_IMMUTABLE;
			sourceDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			const auto makeSourceView = [&](const std::vector<std::uint8_t>& pixels,
											std::string_view label) {
				D3D11_SUBRESOURCE_DATA sourceData{};
				sourceData.pSysMem = pixels.data();
				sourceData.SysMemPitch = kWidth * 4U;
				ComPtr<ID3D11Texture2D> sourceTexture;
				ComPtr<ID3D11ShaderResourceView> sourceView;
				Check(
					device->CreateTexture2D(
						&sourceDescription,
						&sourceData,
						sourceTexture.GetAddressOf()),
					std::format("CreateTexture2D({})", label));
				Check(
					device->CreateShaderResourceView(
						sourceTexture.Get(),
						nullptr,
						sourceView.GetAddressOf()),
					std::format("CreateShaderResourceView({})", label));
				return sourceView;
			};
			const auto blackSource = makeSourceView(captures.black, "black source");
			const auto whiteSource = makeSourceView(captures.white, "white source");

			context->UpdateSubresource(
				constantBuffer.Get(), 0U, nullptr, &constants, 0U, 0U);
			constexpr float clear[4]{
				static_cast<float>(kDestinationColor[0]) / 255.0F,
				static_cast<float>(kDestinationColor[1]) / 255.0F,
				static_cast<float>(kDestinationColor[2]) / 255.0F,
				static_cast<float>(kDestinationColor[3]) / 255.0F
			};
			context->ClearRenderTargetView(renderTargetView.Get(), clear);
			ID3D11RenderTargetView* target = renderTargetView.Get();
			context->OMSetRenderTargets(1U, &target, nullptr);
			context->OMSetBlendState(
				dualSourceBlend.Get(),
				nullptr,
				0xFFFFFFFFU);
			D3D11_VIEWPORT viewport{
				0.0F,
				0.0F,
				static_cast<float>(kWidth),
				static_cast<float>(kHeight),
				0.0F,
				1.0F
			};
			context->RSSetViewports(1U, &viewport);
			context->IASetInputLayout(nullptr);
			context->IASetPrimitiveTopology(
				D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			context->VSSetShader(vertexShader.Get(), nullptr, 0U);
			context->GSSetShader(nullptr, nullptr, 0U);
			context->PSSetShader(pixelShader.Get(), nullptr, 0U);
			ID3D11Buffer* constantsBuffer = constantBuffer.Get();
			ID3D11Buffer* scopeEffectConstantsBuffer = scopeEffectBuffer.Get();
			context->PSSetConstantBuffers(4U, 1U, &constantsBuffer);
			context->PSSetConstantBuffers(5U, 1U, &scopeEffectConstantsBuffer);
			ID3D11ShaderResourceView* sources[2]{
				blackSource.Get(),
				whiteSource.Get()
			};
			context->PSSetShaderResources(4U, 2U, sources);
			ID3D11SamplerState* activeSampler = sampler.Get();
			context->PSSetSamplers(0U, 1U, &activeSampler);
			context->Draw(3U, 0U);

			ID3D11ShaderResourceView* nullSources[2]{ nullptr, nullptr };
			context->PSSetShaderResources(4U, 2U, nullSources);
			context->CopyResource(staging.Get(), renderTarget.Get());
			D3D11_MAPPED_SUBRESOURCE mapped{};
			Check(
				context->Map(
					staging.Get(),
					0U,
					D3D11_MAP_READ,
					0U,
					&mapped),
				"Map(staging)");
			std::vector<std::uint8_t> output(kWidth * kHeight * 4U);
			for (std::uint32_t y = 0U; y < kHeight; ++y) {
				const auto* row =
					static_cast<const std::uint8_t*>(mapped.pData) +
					y * mapped.RowPitch;
				std::copy_n(
					row,
					kWidth * 4U,
					output.data() + y * kWidth * 4U);
			}
			context->Unmap(staging.Get(), 0U);
			return output;
		}

	private:
		ComPtr<ID3D11Device> device;
		ComPtr<ID3D11DeviceContext> context;
		ComPtr<ID3D11VertexShader> vertexShader;
		ComPtr<ID3D11PixelShader> pixelShader;
		ComPtr<ID3D11Texture2D> renderTarget;
		ComPtr<ID3D11RenderTargetView> renderTargetView;
		ComPtr<ID3D11Texture2D> staging;
		ComPtr<ID3D11Buffer> constantBuffer;
		ComPtr<ID3D11Buffer> scopeEffectBuffer;
		ComPtr<ID3D11SamplerState> sampler;
		ComPtr<ID3D11BlendState> dualSourceBlend;
	};

	PixelMetrics AnalyzeDifference(const std::vector<std::uint8_t>& pixels)
	{
		PixelMetrics metrics{};
		double totalWeight = 0.0;
		for (std::uint32_t y = 0U; y < kHeight; ++y) {
			for (std::uint32_t x = 0U; x < kWidth; ++x) {
				const auto offset = (y * kWidth + x) * 4U;
				const auto redDelta = std::abs(
					static_cast<int>(pixels[offset]) -
					static_cast<int>(kDestinationColor[0]));
				const auto greenDelta = std::abs(
					static_cast<int>(pixels[offset + 1U]) -
					static_cast<int>(kDestinationColor[1]));
				const auto blueDelta = std::abs(
					static_cast<int>(pixels[offset + 2U]) -
					static_cast<int>(kDestinationColor[2]));
				const auto weight = static_cast<double>(std::max({ redDelta,
					greenDelta,
					blueDelta }));
				if (weight <= 2.0) {
					continue;
				}
				++metrics.count;
				metrics.minX = std::min(metrics.minX, x);
				metrics.minY = std::min(metrics.minY, y);
				metrics.maxX = std::max(metrics.maxX, x);
				metrics.maxY = std::max(metrics.maxY, y);
				totalWeight += weight;
				metrics.centerX += (static_cast<double>(x) + 0.5) * weight;
				metrics.centerY += (static_cast<double>(y) + 0.5) * weight;
			}
		}
		if (totalWeight > 0.0) {
			metrics.centerX /= totalWeight;
			metrics.centerY /= totalWeight;
		}
		return metrics;
	}

	std::vector<std::uint8_t> ExpectedOneXComposite(
		const ReticleCapturePair& captures)
	{
		std::vector<std::uint8_t> expected(kWidth * kHeight * 4U, 0U);
		for (std::uint32_t pixel = 0U; pixel < kWidth * kHeight; ++pixel) {
			const auto offset = pixel * 4U;
			for (std::uint32_t channel = 0U; channel < 3U; ++channel) {
				const auto black = static_cast<double>(captures.black[offset + channel]);
				const auto white = static_cast<double>(captures.white[offset + channel]);
				const auto transmittance = std::clamp(white - black, 0.0, 255.0);
				expected[offset + channel] = static_cast<std::uint8_t>(
					std::clamp(
						std::lround(
							black +
							static_cast<double>(kDestinationColor[channel]) *
								transmittance / 255.0),
						0L,
						255L));
			}
			expected[offset + 3U] = kDestinationColor[3];
		}
		return expected;
	}

	void RequirePixelsNear(
		const std::vector<std::uint8_t>& actual,
		const std::vector<std::uint8_t>& expected,
		int tolerance,
		std::string_view failure)
	{
		if (actual.size() != expected.size()) {
			throw std::runtime_error(std::string(failure));
		}
		for (std::size_t index = 0U; index < actual.size(); ++index) {
			if (std::abs(
					static_cast<int>(actual[index]) -
					static_cast<int>(expected[index])) > tolerance) {
				throw std::runtime_error(std::format(
					"{} at byte {}: actual={}, expected={}",
					failure,
					index,
					actual[index],
					expected[index]));
			}
		}
	}

}

int wmain(int argc, wchar_t** argv)
try {
	const auto project = argc > 1 ?
	                         std::filesystem::path(argv[1]) :
	                         std::filesystem::current_path();
	ShaderFixture fixture(
		project / "Compile" / "Shaders" / "MagnaScope" /
		"ReticleLayer_PS.cso");

	const auto centeredLayer = MakeReticleCapturePair(64U, 64U);
	ResolutionConstants constants{};
	const auto oneX = fixture.Render(constants, centeredLayer);
	const auto expectedOneX = ExpectedOneXComposite(centeredLayer);
	RequirePixelsNear(
		oneX,
		expectedOneX,
		1,
		"dual-source reconstruction differs from the authored 1x blend");
	const auto pixel = [&oneX](std::uint32_t x, std::uint32_t y) {
		const auto offset = (y * kWidth + x) * 4U;
		return std::array<std::uint8_t, 4>{
			oneX[offset],
			oneX[offset + 1U],
			oneX[offset + 2U],
			oneX[offset + 3U]
		};
	};
	const auto untouched = pixel(4U, 4U);
	if (untouched != kDestinationColor) {
		throw std::runtime_error(
			"transparent reticle backing changed the non-black destination");
	}
	const auto opaqueBlack = pixel(60U, 64U);
	if (opaqueBlack[0] > 1U || opaqueBlack[1] > 1U ||
		opaqueBlack[2] > 1U) {
		throw std::runtime_error(
			"opaque black reticle line was not reconstructed");
	}
	const auto antialiasedBlack = pixel(60U, 63U);
	if (antialiasedBlack[0] >= kDestinationColor[0] ||
		antialiasedBlack[1] >= kDestinationColor[1] ||
		antialiasedBlack[2] >= kDestinationColor[2] ||
		antialiasedBlack[0] == 0U || antialiasedBlack[1] == 0U ||
		antialiasedBlack[2] == 0U) {
		throw std::runtime_error(
			"antialiased black reticle coverage was not preserved");
	}
	const auto redDot = pixel(68U, 60U);
	if (redDot[0] < 220U || redDot[1] >= redDot[0] ||
		redDot[2] >= redDot[0]) {
		throw std::runtime_error(
			"bright red reticle dot was not reconstructed");
	}
	// Scene magnification and reticle magnification are independent controls.
	// A 1x reticle must be byte-identical while the scene moves through the
	// user-facing 1x, 2x, and 4x optical settings. This is stronger than merely
	// comparing bounds and catches any scene warp, filter, or sample offset that
	// leaks into the late authored-reticle composite.
	ResolutionConstants sceneTwoXConstants = constants;
	sceneTwoXConstants.sceneMagnification = 2.0F;
	const auto sceneTwoX = fixture.Render(sceneTwoXConstants, centeredLayer);
	ResolutionConstants sceneFourXConstants = constants;
	sceneFourXConstants.sceneMagnification = 4.0F;
	const auto sceneFourX = fixture.Render(sceneFourXConstants, centeredLayer);
	if (oneX != sceneTwoX || oneX != sceneFourX) {
		throw std::runtime_error(
			"scene magnification changed the 1x reticle layer");
	}

	// Use a deliberately off-center authored reticle. Scaling around screen
	// center or the lens center can accidentally pass a centered fixture even
	// though it visibly walks a manually aligned STS reticle across the glass.
	constexpr float reticlePivotX = 78.5F;
	constexpr float reticlePivotY = 52.5F;
	const auto offsetLayer = MakeReticleCapturePair(78U, 52U);
	ResolutionConstants pivotConstants{};
	pivotConstants.aimCenterX = reticlePivotX;
	pivotConstants.aimCenterY = reticlePivotY;
	// Keep the physical lens centered at (64.5, 64.5) while publishing the
	// authored reticle-minus-lens offset through the projected local basis.
	pivotConstants.aimOffsetX =
		(reticlePivotX - 64.5F) / pivotConstants.lensBasisXX;
	pivotConstants.aimOffsetY =
		(reticlePivotY - 64.5F) / pivotConstants.lensBasisZY;
	pivotConstants.reticleMagnification = 0.5F;
	const auto halfX = fixture.Render(pivotConstants, offsetLayer);
	const auto halfMetrics = AnalyzeDifference(halfX);
	pivotConstants.reticleMagnification = 1.0F;
	const auto pivotOneX = fixture.Render(pivotConstants, offsetLayer);
	const auto pivotOneMetrics = AnalyzeDifference(pivotOneX);
	pivotConstants.reticleMagnification = 2.0F;
	const auto twoX = fixture.Render(pivotConstants, offsetLayer);
	const auto twoMetrics = AnalyzeDifference(twoX);
	ResolutionConstants sizeEquivalent = pivotConstants;
	sizeEquivalent.reticleMagnification = 1.0F;
	sizeEquivalent.reticleSize = 8.0F;
	const auto sizeEquivalentOutput =
		fixture.Render(sizeEquivalent, offsetLayer);
	if (twoX != sizeEquivalentOutput) {
		throw std::runtime_error(
			"reticle size and reticle magnification did not compose identically");
	}
	if (halfMetrics.count == 0U || pivotOneMetrics.count == 0U ||
		twoMetrics.count == 0U) {
		throw std::runtime_error("reticle layer produced no visible pixels");
	}
	const auto extentNear = [](double actual, double expected) {
		// Linear filtering contributes at most roughly one partially covered
		// output pixel at each edge of this deliberately tiny fixture.
		return std::abs(actual - expected) <= 1.5;
	};
	if (!extentNear(
			static_cast<double>(halfMetrics.Width()),
			static_cast<double>(pivotOneMetrics.Width()) * 0.5) ||
		!extentNear(
			static_cast<double>(halfMetrics.Height()),
			static_cast<double>(pivotOneMetrics.Height()) * 0.5) ||
		!extentNear(
			static_cast<double>(twoMetrics.Width()),
			static_cast<double>(pivotOneMetrics.Width()) * 2.0) ||
		!extentNear(
			static_cast<double>(twoMetrics.Height()),
			static_cast<double>(pivotOneMetrics.Height()) * 2.0)) {
		throw std::runtime_error(std::format(
			"reticle post-scale mismatch: 0.5x={}x{}, 1x={}x{}, 2x={}x{}",
			halfMetrics.Width(),
			halfMetrics.Height(),
			pivotOneMetrics.Width(),
			pivotOneMetrics.Height(),
			twoMetrics.Width(),
			twoMetrics.Height()));
	}
	// The fixture is asymmetric, so its alpha centroid legitimately moves away
	// from the pivot as it grows. Verify that this movement is exactly the
	// requested local scale about the authored reticle center.
	const auto expectedCenter = [](
							double oneXCenter,
							double pivot,
							double scale) {
		return pivot + (oneXCenter - pivot) * scale;
	};
	if (std::abs(
			halfMetrics.centerX -
				expectedCenter(pivotOneMetrics.centerX, reticlePivotX, 0.5)) >
			0.6 ||
		std::abs(
			halfMetrics.centerY -
				expectedCenter(pivotOneMetrics.centerY, reticlePivotY, 0.5)) >
			0.6 ||
		std::abs(
			twoMetrics.centerX -
				expectedCenter(pivotOneMetrics.centerX, reticlePivotX, 2.0)) >
			0.6 ||
		std::abs(
			twoMetrics.centerY -
				expectedCenter(pivotOneMetrics.centerY, reticlePivotY, 2.0)) >
			0.6) {
		throw std::runtime_error(
			"reticle scaling moved its authored screen-space pivot");
	}

	// Scene sampling effects must be irrelevant to the late reticle layer.
	// Physical eye motion and exit-pupil visibility are tested separately.
	ResolutionConstants cleanOptics{};
	cleanOptics.reticleMagnification = 2.0F;
	const auto cleanOutput = fixture.Render(cleanOptics, centeredLayer);
	ResolutionConstants hostileOptics = cleanOptics;
	hostileOptics.sceneMagnification = 15.0F;
	hostileOptics.imageDenoise = 1.0F;
	hostileOptics.imageSharpen = 1.0F;
	hostileOptics.fishEyeStrength = 2.0F;
	hostileOptics.fishEyePower = 6.0F;
	hostileOptics.edgeRefractionStrength = 0.25F;
	hostileOptics.edgeRefractionWidth = 0.5F;
	hostileOptics.edgeChromaticAberration = 2.0F;
	const auto hostileOutput = fixture.Render(hostileOptics, centeredLayer);
	if (cleanOutput != hostileOutput) {
		throw std::runtime_error(
			"scene optics changed the independent reticle layer");
	}

	// Reticle offset is optic-local and is applied after scaling. With a
	// 56-pixel X basis, 178.5714 thousandths is exactly +10 display pixels.
	ResolutionConstants offsetConstants = cleanOptics;
	offsetConstants.reticleOffsetX = 178.5714286F;
	const auto offsetOutput = fixture.Render(offsetConstants, centeredLayer);
	const auto cleanMetrics = AnalyzeDifference(cleanOutput);
	const auto offsetMetrics = AnalyzeDifference(offsetOutput);
	if (std::abs(offsetMetrics.centerX - cleanMetrics.centerX - 10.0) > 0.15 ||
		std::abs(offsetMetrics.centerY - cleanMetrics.centerY) > 0.15 ||
		offsetMetrics.count != cleanMetrics.count) {
		throw std::runtime_error(
			"reticle local offset was scaled or changed reticle coverage");
	}

	// The isolated reticle follows the same bounded scene-parallax display
	// translation while remaining independent of scene magnification.
	ResolutionConstants movingConstants{};
	movingConstants.sceneParallaxStrength = 1.0F;
	movingConstants.eyeOffsetX = 0.5F;
	movingConstants.opticalLagStrength = 1.0F;
	movingConstants.eyeBoxRadius = 4.0F;
	const auto movingOneX = fixture.Render(movingConstants, centeredLayer);
	const auto movingOneMetrics = AnalyzeDifference(movingOneX);
	movingConstants.sceneMagnification = 4.0F;
	const auto movingFourX = fixture.Render(movingConstants, centeredLayer);
	// Scene parallax saturates through ScopeShadowSoftLimitVector at one
	// aperture radius, the same limiter the magnified replay applies, so the
	// two layers stay locked together. Deriving the expectation from that
	// formula keeps this test honest if the limit is ever retuned; the
	// retired 1/(1 + 2m) form produced 14.0 pixels here, discarding nearly
	// half of an ordinary half-radius shift.
	constexpr double kTravel = 0.5;
	const double expectedTravelPixels =
		kTravel / std::sqrt(1.0 + kTravel * kTravel) * 56.0;
	if (movingOneX != movingFourX ||
		std::abs(
			movingOneMetrics.centerX -
			AnalyzeDifference(oneX).centerX -
			expectedTravelPixels) > 0.15) {
		throw std::runtime_error(
			"reticle optical translation depended on scene zoom or used wrong sign");
	}

	// Eye motion is already expressed in display X/Y. Rotating the published
	// CPU basis, or making its determinant nearly zero while retaining finite
	// column lengths, must not rotate or suppress the reticle follower. The
	// determinant remains relevant only when inverting lens coordinates.
	ResolutionConstants rotatedMotion = movingConstants;
	rotatedMotion.sceneMagnification = 1.0F;
	rotatedMotion.lensBasisXX = 0.0F;
	rotatedMotion.lensBasisXY = 56.0F;
	rotatedMotion.lensBasisZX = -56.0F;
	rotatedMotion.lensBasisZY = 0.0F;
	const auto rotatedMotionOutput = fixture.Render(rotatedMotion, centeredLayer);
	RequirePixelsNear(
		rotatedMotionOutput,
		movingOneX,
		1,
		"reticle motion rotated with the stale CPU basis");

	ResolutionConstants nearCollinearMotion = movingConstants;
	nearCollinearMotion.sceneMagnification = 1.0F;
	nearCollinearMotion.lensBasisXX = 56.0F;
	nearCollinearMotion.lensBasisXY = 0.0F;
	nearCollinearMotion.lensBasisZX = 56.0F;
	nearCollinearMotion.lensBasisZY = 0.0000005F;
	const auto nearCollinearMotionOutput =
		fixture.Render(nearCollinearMotion, centeredLayer);
	RequirePixelsNear(
		nearCollinearMotionOutput,
		movingOneX,
		1,
		"near-collinear CPU basis disabled the reticle follower");

	// The exit pupil is displaced by a bounded amount that can never carry the
	// aligned lens centre outside the lit disc, so an aligned optic keeps a
	// bright centre no matter how much travel the game thread publishes. The
	// reticle must follow that contract exactly: at maximum travel through a
	// deliberately narrow exit pupil, an aligned reticle stays fully composited
	// because the magnified scene beneath it is also still lit. The retired
	// unbounded formulation blacked out the whole lens here, which is the
	// defect in-game testers reported.
	ResolutionConstants alignedMaximumTravel{};
	alignedMaximumTravel.sceneParallaxStrength = 0.0F;
	alignedMaximumTravel.eyeOffsetX = 4.0F;
	alignedMaximumTravel.eyeBoxMaxTravel = 4.0F;
	alignedMaximumTravel.eyeBoxRadius = 0.1F;
	alignedMaximumTravel.vignetteReach = 10.0F;
	alignedMaximumTravel.vignetteSharpness = 3.0F;
	alignedMaximumTravel.reticleShadowStrength = 1.0F;
	const auto alignedMaximumTravelOutput =
		fixture.Render(alignedMaximumTravel, centeredLayer);
	{
		// The authored red dot sits at (68, 60), well inside the lit disc.
		constexpr std::size_t redDotOffset = (60U * kWidth + 68U) * 4U;
		for (std::size_t channel = 0U; channel < 4U; ++channel) {
			if (std::abs(
					static_cast<int>(
						alignedMaximumTravelOutput[redDotOffset + channel]) -
					static_cast<int>(oneX[redDotOffset + channel])) > 1) {
				throw std::runtime_error(
					"bounded exit pupil still darkened an aligned reticle at "
					"maximum published eye travel");
			}
		}
	}

	// Where the crescent has genuinely darkened the scene the reticle must
	// still vanish, so it can never paint over scope shadow. Placing the
	// physical lens 0.9 radii away puts the authored reticle on the far side
	// of the same displaced pupil.
	ResolutionConstants shadowedConstants = alignedMaximumTravel;
	shadowedConstants.lensCenterX = 64.5F + 0.9F * 56.0F;
	const auto shadowedOutput = fixture.Render(shadowedConstants, centeredLayer);
	if (AnalyzeDifference(shadowedOutput).count != 0U) {
		throw std::runtime_error(
			"reticle remained visible inside the displaced pupil crescent");
	}
	ResolutionConstants nearCollinearShadow = shadowedConstants;
	nearCollinearShadow.lensBasisXX = 56.0F;
	nearCollinearShadow.lensBasisXY = 0.0F;
	nearCollinearShadow.lensBasisZX = 56.0F;
	nearCollinearShadow.lensBasisZY = 0.0000005F;
	const auto nearCollinearShadowOutput =
		fixture.Render(nearCollinearShadow, centeredLayer);
	if (AnalyzeDifference(nearCollinearShadowOutput).count != 0U) {
		throw std::runtime_error(
			"near-collinear CPU basis disabled reticle scope shadow");
	}

	// A projection publication gap must preserve the authored layer instead
	// of clipping it against zero-filled constants after load or device reset.
	ResolutionConstants invalidProjection{};
	invalidProjection.aimCenterX = 0.0F;
	invalidProjection.aimCenterY = 0.0F;
	invalidProjection.lensRadiusX = 0.0F;
	invalidProjection.lensRadiusY = 0.0F;
	invalidProjection.reticleMagnification = 8.0F;
	const auto invalidProjectionOutput =
		fixture.Render(invalidProjection, centeredLayer);
	RequirePixelsNear(
		invalidProjectionOutput,
		expectedOneX,
		1,
		"invalid projection did not fail open to the authored blend");

	// ScopeFade geometry is not an ownership mask for the authored reticle.
	// STS authors can align the reticle independently from the lens, and the
	// projected lens center can move far outside the render target during a
	// transition or a sharp inertia impulse. With no physical eye translation
	// and no requested reticle shadow, neither lens transform may crop, move,
	// or otherwise alter the complete late reticle layer.
	struct LensTransform
	{
		float basisXX;
		float basisXY;
		float basisZX;
		float basisZY;
		float centerX;
		float centerY;
	};
	constexpr float diagonal = 39.5979805F;
	constexpr std::array hostileLensTransforms{
		LensTransform{ 56.0F, 0.0F, 0.0F, 56.0F, -2048.0F, 4096.0F },
		LensTransform{
			diagonal,
			diagonal,
			-diagonal,
			diagonal,
			8192.0F,
			-4096.0F },
		LensTransform{ 0.0F, 56.0F, -56.0F, 0.0F, -8192.0F, -8192.0F },
		LensTransform{ 48.4974213F, -28.0F, 28.0F, 48.4974213F, 16384.0F, 64.5F }
	};
	for (const auto& transform : hostileLensTransforms) {
		ResolutionConstants transformedConstants{};
		transformedConstants.sceneParallaxStrength = 2.0F;
		transformedConstants.opticalLagStrength = 4.0F;
		transformedConstants.eyeOffsetX = 0.0F;
		transformedConstants.eyeOffsetY = 0.0F;
		transformedConstants.reticleShadowStrength = 0.0F;
		transformedConstants.lensBasisXX = transform.basisXX;
		transformedConstants.lensBasisXY = transform.basisXY;
		transformedConstants.lensBasisZX = transform.basisZX;
		transformedConstants.lensBasisZY = transform.basisZY;
		transformedConstants.lensCenterX = transform.centerX;
		transformedConstants.lensCenterY = transform.centerY;
		const auto transformedOutput =
			fixture.Render(transformedConstants, centeredLayer);
		if (AnalyzeDifference(transformedOutput).count == 0U) {
			throw std::runtime_error(
				"lens transform removed the complete reticle layer");
		}
		if (transformedOutput != oneX) {
			throw std::runtime_error(std::format(
				"lens transform changed the independently authored reticle: "
				"basis=({}, {})/({}, {}), center=({}, {})",
				transform.basisXX,
				transform.basisXY,
				transform.basisZX,
				transform.basisZY,
				transform.centerX,
				transform.centerY));
		}
	}

	std::cout << std::format(
		"Reticle layer shader PASSED: scene 1x/2x/4x invariant; "
		"reticle 0.5x={}x{}, 1x={}x{}, 2x={}x{}; "
		"size/offset/motion/shadow verified; "
		"non-destructive lens transforms={}\n",
		halfMetrics.Width(),
		halfMetrics.Height(),
		pivotOneMetrics.Width(),
		pivotOneMetrics.Height(),
		twoMetrics.Width(),
		twoMetrics.Height(),
		hostileLensTransforms.size());
	return 0;
} catch (const std::exception& error) {
	std::cerr << "Reticle layer shader FAILED: " << error.what() << '\n';
	return 1;
}
