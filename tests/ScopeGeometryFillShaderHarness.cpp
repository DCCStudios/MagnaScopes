#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <format>
#include <iostream>
#include <stdexcept>
#include <vector>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace
{
	constexpr std::uint32_t kWidth = 128;
	constexpr std::uint32_t kHeight = 128;
	constexpr std::uint32_t kSegments = 24;

	struct Vertex
	{
		float x;
		float y;
		float z;
	};

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
}

int wmain(int argc, wchar_t** argv)
try {
	const auto project = argc > 1 ?
		std::filesystem::path(argv[1]) :
		std::filesystem::current_path();
	const auto shaderDirectory =
		project / "Package" / "MagnaScope" / "Shaders" / "MagnaScope";
	const auto geometryShaderBytes =
		ReadBinary(shaderDirectory / "ScopeGeometryFill_GS.cso");
	const auto pixelShaderBytes =
		ReadBinary(shaderDirectory / "ScopeGeometryProbe_PS.cso");

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
    float3 position : POSITION;
};
struct Output
{
    float4 position : SV_Position;
};
Output main(Input input)
{
    Output output;
    output.position = float4(input.position, 1.0f);
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

	const D3D11_INPUT_ELEMENT_DESC inputElement{
		"POSITION",
		0,
		DXGI_FORMAT_R32G32B32_FLOAT,
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
		vertices[segment * 2] = { sine * 0.80F, cosine * 0.80F, 0.5F };
		vertices[segment * 2 + 1] = {
			sine * 0.40F,
			cosine * 0.40F,
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
	vertexDescription.Usage = D3D11_USAGE_IMMUTABLE;
	vertexDescription.BindFlags = D3D11_BIND_VERTEX_BUFFER;
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

	const auto center = Pixel(mapped, kWidth / 2, kHeight / 2);
	const auto annulus = Pixel(mapped, kWidth / 2, 20);
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

	std::cout << std::format(
		"ScopeFade geometry fill PASSED: center=({}, {}, {}), "
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
	return 0;
} catch (const std::exception& exception) {
	std::cerr << exception.what() << '\n';
	return 2;
}
