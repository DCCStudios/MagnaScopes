#include "hooking.h"
#include "DDSTextureLoader11.h"
#include "WICTextureLoader11.h"
#include <vector>
#include <Shlwapi.h>
#include <d3dcompiler.h>
#include <d3d11.h>
#include <dxgi1_4.h>
#include <d3d11_4.h>
#include <d3dcommon.h>
#include <MinHook.h>
#include <REX/W32/COMPTR.h>

#include <MathUtils.h>
#include "Settings.h"
#include "hookingStruct.h"
#include <renderdoc_app.h>

#include <d3d9.h>


#pragma comment(lib, "D3DCompiler.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "shlwapi.lib")


bool EnableDebugPrivilege()
{
	HANDLE hToken;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
		return false;
	}

	LUID luid;
	if (!LookupPrivilegeValue(NULL, SE_DEBUG_NAME, &luid)) {
		CloseHandle(hToken);
		return false;
	}

	TOKEN_PRIVILEGES tp;
	tp.PrivilegeCount = 1;
	tp.Privileges[0].Luid = luid;
	tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

	if (!AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(TOKEN_PRIVILEGES), NULL, NULL)) {
		CloseHandle(hToken);
		return false;
	}

	CloseHandle(hToken);
	return GetLastError() == ERROR_SUCCESS;
}

bool IsExecutableAddress(const void* address)
{
	if (!address) {
		return false;
	}
	MEMORY_BASIC_INFORMATION memory{};
	if (VirtualQuery(address, &memory, sizeof(memory)) != sizeof(memory) ||
		memory.State != MEM_COMMIT ||
		(memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
		return false;
	}
	const DWORD executableProtection =
		PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
	return (memory.Protect & executableProtection) != 0;
}

#ifdef _DEBUG
RENDERDOC_API_1_6_0* rdoc_api = nullptr;
#endif
static constexpr UINT TARGET_STRIDE = 20;
static constexpr UINT TARGET_INDEX_COUNT = 24;
static constexpr UINT TARGET_BUFFER_SIZE = 0x0000000008000000;
static constexpr UINT TARGET_TEXTURE_WIDTH = 2048;
static constexpr UINT TARGET_TEXTURE_HEIGHT = 2048;
static constexpr DXGI_FORMAT TARGET_TEXTURE_FORMAT = DXGI_FORMAT_BC2_UNORM_SRGB;


ComPtr<IDXGISwapChain> g_Swapchain = nullptr;
ComPtr<ID3D11Device> g_Device = nullptr;
ComPtr<ID3D11DeviceContext> g_Context = nullptr;

bool bChangeAimTexture = true;
bool bResetZoomDelta = false;
bool bSelfDraw = false;
bool isActive_TAA = false;
bool isActive_DOF = false;
bool renderedAtTAAThisFrame = false;
bool compositedThisFrame = false;
bool lastRenderProducedComposite = false;
bool renderPassHandledThisFrame = false;
// Stage 3 callbacks can execute on different engine threads. This atomic is a
// narrow handoff only: it tells the Menu Framework frame boundary that the
// guarded TAA callback already captured a source. No game object or profile
// pointer crosses through it.
std::atomic_bool taaVerificationCapturedSinceFramework = false;

using namespace DirectX;


static HWND g_hWnd;
static HMODULE g_hModule;

DWORD_PTR* pSwapChainVTable = nullptr;
DWORD_PTR* pDeviceVTable = nullptr;
DWORD_PTR* pDeviceContextVTable = nullptr;

int windowWidth;
int windowHeight;

float gameZoomDelta = 1;
bool bIsFirst = true;


ID3D11Buffer* targetVertexConstBuffer;
ID3D11Buffer* targetVertexConstBuffer1p5;
ID3D11Buffer* targetVertexConstBuffer1;
			
ID3D11Buffer* targetVertexConstBufferOutPut;
ID3D11Buffer* targetVertexConstBufferOutPut1p5;
ID3D11Buffer* targetVertexConstBufferOutPut1;

Microsoft::WRL::ComPtr<ID3D11Buffer> targetIndexBuffer;
DXGI_FORMAT targetIndexBufferFormat;
UINT targetIndexBufferOffset;

bool bDrawIndexed = true;

std::atomic<IDXGISwapChain*> lastPresentedSwapChain{ nullptr };
IDXGISwapChain* backBufferOwnerSwapChain = nullptr;
UINT mBackBufferIndex = UINT_MAX;
bool getBufferReferenceContractProbed = false;
bool getBufferAddsReference = true;

ID3D11Texture2D* AcquireBackBuffer(IDXGISwapChain* swapChain, UINT index)
{
	if (!swapChain) {
		return nullptr;
	}

	ID3D11Texture2D* texture = nullptr;
	if (FAILED(swapChain->GetBuffer(index, IID_PPV_ARGS(&texture))) || !texture) {
		return nullptr;
	}

	if (!getBufferReferenceContractProbed) {
		getBufferReferenceContractProbed = true;

		// Standard DXGI increments the returned texture's reference count.
		// Some frame-generation/upscaler proxy chains return their internal
		// pointer without doing so. Probe the contract once, matching F4SE
		// Menu Framework's proven handling, so MagnaScope never releases a
		// reference that it did not acquire.
		texture->AddRef();
		const ULONG firstCount = texture->Release();

		ID3D11Texture2D* second = nullptr;
		if (SUCCEEDED(swapChain->GetBuffer(index, IID_PPV_ARGS(&second))) && second) {
			second->AddRef();
			const ULONG secondCount = second->Release();
			getBufferAddsReference = secondCount > firstCount;
			if (getBufferAddsReference) {
				second->Release();
			}
		}

		logger::info(
			"Swap-chain GetBuffer {} ({} release contract)",
			getBufferAddsReference ? "adds a COM reference" : "returns a proxy-owned pointer",
			getBufferAddsReference ? "balanced" : "no direct");
	}

	return texture;
}

void ReleaseAcquiredBackBuffer(ID3D11Texture2D* texture)
{
	if (texture && getBufferAddsReference) {
		texture->Release();
	}
}

ID3D11RenderTargetView* tempRt[2] = {};
ID3D11DepthStencilView* tempSV = nullptr;

struct ScopedRenderTargetReferences
{
	~ScopedRenderTargetReferences()
	{
		SAFE_RELEASE(tempRt[0]);
		SAFE_RELEASE(tempRt[1]);
		SAFE_RELEASE(tempSV);
	}
};

UINT targetIndexCount = 0;
UINT targetStartIndexLocation = 0;
UINT targetBaseVertexLocation = 0;
ComPtr<ID3D11VertexShader> targetVS;
ComPtr<ID3D11ClassInstance> targetVSClassInstance;
UINT targetVSNumClassesInstance;

ComPtr<ID3D11PixelShader> targetPS;
ComPtr<ID3D11DepthStencilState> targetDepthStencilState = nullptr;
ComPtr<ID3D11InputLayout> targetInputLayout;



ComPtr<ID3D11Buffer> targetVertexBuffer;
UINT targetVertexBufferStrides;
UINT targetVertexBufferOffsets;

ComPtr<ID3D11ShaderResourceView> DrawIndexedSRV;

bool bHasGetBackBuffer = false;
Hook::D3D* D3DInstance = Hook::D3D::GetSington();

ComPtr<ID3D11Buffer> plane_pIndexBuffer = nullptr;

ID3D11Buffer* gdc_pVertexBuffer = NULL;
ID3D11InputLayout* gdc_pVertexLayout = NULL;
ID3D11Buffer* gdc_pIndexBuffer = NULL;
HMODULE upscalerMod;

using namespace ScopeData;

namespace
{
	HMODULE DetectUpscalerOrFrameGeneration()
	{
		// The original FTS knew only the old Fallout4Upscaler DLL name. LoreOut
		// uses the newer Streamline "Upscaling.dll" plus AAA Frame Generation,
		// so treating this configuration as native TAA is incorrect.
		const char* modules[] = {
			"Fallout4Upscaler.dll",
			"Upscaling.dll",
			"AAAFrameGeneration.dll",
			"sl.interposer.dll"
		};
		for (const auto* module : modules) {
			if (auto handle = GetModuleHandleA(module)) {
				return handle;
			}
		}
		return nullptr;
	}
}

constexpr UINT MAX_SRV_SLOTS = 128;     // D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT
constexpr UINT MAX_SAMPLER_SLOTS = 16;  // D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT
constexpr UINT MAX_CB_SLOTS = 14;       // D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT
using namespace RE::BSGraphics;

struct SavedState
{
	// IA Stage
	ID3D11InputLayout* pInputLayout;
	ID3D11Buffer* pVertexBuffers[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
	UINT VertexStrides[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
	UINT VertexOffsets[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
	ID3D11Buffer* pIndexBuffer;
	DXGI_FORMAT IndexBufferFormat;
	UINT IndexBufferOffset;
	D3D11_PRIMITIVE_TOPOLOGY PrimitiveTopology;

	// VS Stage
	ID3D11VertexShader* pVS;
	ID3D11Buffer* pVSCBuffers[MAX_CB_SLOTS];
	ID3D11ShaderResourceView* pVSSRVs[MAX_SRV_SLOTS];
	ID3D11SamplerState* pVSSamplers[MAX_SAMPLER_SLOTS];

	// PS Stage
	ID3D11PixelShader* pPS;
	ID3D11Buffer* pPSCBuffers[MAX_CB_SLOTS];
	ID3D11ShaderResourceView* pPSSRVs[MAX_SRV_SLOTS];
	ID3D11SamplerState* pPSSamplers[MAX_SAMPLER_SLOTS];

	// RS Stage
	D3D11_VIEWPORT Viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
	UINT NumViewports;
	ID3D11RasterizerState* pRasterizerState;

	// OM Stage
	ID3D11RenderTargetView* pRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
	ID3D11DepthStencilView* pDSV;
	ID3D11BlendState* pBlendState;
	FLOAT BlendFactor[4];
	UINT SampleMask;
	ID3D11DepthStencilState* pDepthStencilState;
	UINT StencilRef;
};


namespace Hook
{

	RE::PlayerCharacter* player;
	RE::PlayerCamera* pcam;
	ScopeDataHandler* sdh;

	bool legacyFlag = true;

	#define LF(f) (legacyFlag ? (f) : (f) / 1000.0f)

	std::unique_ptr<float[]> LFA(const float arr[], size_t size)
	{
		std::unique_ptr<float[]> arrNew(new float[size]);

		for (size_t i = 0; i < size; i++) {
			arrNew[i] = LF(arr[i]);
		}

		return arrNew;
	}

	const wchar_t* GetWC(const char* c)
	{
		size_t len = strlen(c) + 1;
		size_t converted = 0;
		wchar_t* WStr;
		WStr = (wchar_t*)malloc(len * sizeof(wchar_t));
		mbstowcs_s(&converted, WStr, len, c, _TRUNCATE);
		return WStr;
	}


	HRESULT CreateShaderFromFile(const WCHAR* csoFileNameInOut,const WCHAR* hlslFileName,LPCSTR entryPoint,LPCSTR shaderModel,ID3DBlob** ppBlobOut)
	{
		if (!ppBlobOut) {
			return E_INVALIDARG;
		}
		*ppBlobOut = nullptr;
		HRESULT hr = S_OK;

		if (csoFileNameInOut && D3DReadFileToBlob(csoFileNameInOut, ppBlobOut) == S_OK) {
			return hr;
		}

		// Existing FTS installations place the same compiled shaders in the
		// XiFeiLi directory. Probe that location before compiling source.
		if (csoFileNameInOut) {
			std::wstring legacyPath(csoFileNameInOut);
			if (const auto position = legacyPath.find(L"MagnaScope"); position != std::wstring::npos) {
				legacyPath.replace(position, std::wstring_view(L"MagnaScope").size(), L"XiFeiLi");
				if (D3DReadFileToBlob(legacyPath.c_str(), ppBlobOut) == S_OK) {
					logger::info("Loaded legacy FTS shader {}", std::filesystem::path(legacyPath).string());
					return S_OK;
				}
			}
		}

		if (hlslFileName && std::filesystem::exists(hlslFileName)) {
			DWORD dwShaderFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG

			dwShaderFlags |= D3DCOMPILE_DEBUG;

			dwShaderFlags |= D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
			ID3DBlob* errorBlob = nullptr;
			hr = D3DCompileFromFile(hlslFileName, nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, entryPoint, shaderModel,
				dwShaderFlags, 0, ppBlobOut, &errorBlob);
			if (FAILED(hr)) {
				if (errorBlob != nullptr) {
					const auto* message = reinterpret_cast<const char*>(errorBlob->GetBufferPointer());
					OutputDebugStringA(message);
					logger::error("Shader compilation failed: {}", message);
				}
				SAFE_RELEASE(errorBlob);
				return hr;
			}
			SAFE_RELEASE(errorBlob);
			return S_OK;
		}

		logger::error("Compiled shader was not found: {}", std::filesystem::path(csoFileNameInOut).string());
		return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
	}

	static XMMATRIX GetProjectionMatrix(float fov)
	{
		if (windowHeight == 0 || windowWidth == 0) {
			const auto* renderWindow = RE::BSGraphics::GetCurrentRendererWindow();
			if (!renderWindow) {
				return XMMatrixIdentity();
			}
			windowWidth = renderWindow->windowWidth;
			windowHeight = renderWindow->windowHeight;
		}

		XMMATRIX projectionMatrix = XMMatrixPerspectiveFovLH(
			fov > XM_PI ? XMConvertToRadians(fov) : fov,
			static_cast<float>(windowWidth) / static_cast<float>(windowHeight),
			0.1f,                  // Near clipping plane distance
			1000.0f                    // Far clipping plane distance
		);

		return projectionMatrix;
	}

	void SaveState(ID3D11DeviceContext* pContext, SavedState& state)
	{
		// IA Stage
		pContext->IAGetInputLayout(&state.pInputLayout);
		pContext->IAGetVertexBuffers(0, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT,
			state.pVertexBuffers, state.VertexStrides, state.VertexOffsets);
		pContext->IAGetIndexBuffer(&state.pIndexBuffer, &state.IndexBufferFormat, &state.IndexBufferOffset);
		pContext->IAGetPrimitiveTopology(&state.PrimitiveTopology);
		// VS Stage
		pContext->VSGetShader(&state.pVS, nullptr, nullptr);
		pContext->VSGetConstantBuffers(0, MAX_CB_SLOTS, state.pVSCBuffers);
		pContext->VSGetShaderResources(0, MAX_SRV_SLOTS, state.pVSSRVs);
		pContext->VSGetSamplers(0, MAX_SAMPLER_SLOTS, state.pVSSamplers);
		// PS Stage
		pContext->PSGetShader(&state.pPS, nullptr, nullptr);
		pContext->PSGetConstantBuffers(0, MAX_CB_SLOTS, state.pPSCBuffers);
		pContext->PSGetShaderResources(0, MAX_SRV_SLOTS, state.pPSSRVs);
		pContext->PSGetSamplers(0, MAX_SAMPLER_SLOTS, state.pPSSamplers);
		// RS Stage
		state.NumViewports = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
		pContext->RSGetViewports(&state.NumViewports, state.Viewports);
		pContext->RSGetState(&state.pRasterizerState);
		// OM Stage
		pContext->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, state.pRTVs, &state.pDSV);
		pContext->OMGetBlendState(&state.pBlendState, state.BlendFactor, &state.SampleMask);
		pContext->OMGetDepthStencilState(&state.pDepthStencilState, &state.StencilRef);
	}

	void RestoreState(ID3D11DeviceContext* pContext, SavedState& state)
	{
		// IA Stage
		pContext->IASetInputLayout(state.pInputLayout);
		pContext->IASetVertexBuffers(0, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT,
			state.pVertexBuffers, state.VertexStrides, state.VertexOffsets);
		pContext->IASetIndexBuffer(state.pIndexBuffer, state.IndexBufferFormat, state.IndexBufferOffset);
		pContext->IASetPrimitiveTopology(state.PrimitiveTopology);
		// VS Stage
		pContext->VSSetShader(state.pVS, nullptr, 0);
		pContext->VSSetConstantBuffers(0, MAX_CB_SLOTS, state.pVSCBuffers);
		pContext->VSSetShaderResources(0, MAX_SRV_SLOTS, state.pVSSRVs);
		pContext->VSSetSamplers(0, MAX_SAMPLER_SLOTS, state.pVSSamplers);
		// PS Stage
		pContext->PSSetShader(state.pPS, nullptr, 0);
		pContext->PSSetConstantBuffers(0, MAX_CB_SLOTS, state.pPSCBuffers);
		pContext->PSSetShaderResources(0, MAX_SRV_SLOTS, state.pPSSRVs);
		pContext->PSSetSamplers(0, MAX_SAMPLER_SLOTS, state.pPSSamplers);
		// RS Stage
		pContext->RSSetViewports(state.NumViewports, state.Viewports);
		pContext->RSSetState(state.pRasterizerState);
		// OM Stage
		pContext->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, state.pRTVs, state.pDSV);
		pContext->OMSetBlendState(state.pBlendState, state.BlendFactor, state.SampleMask);
		pContext->OMSetDepthStencilState(state.pDepthStencilState, state.StencilRef);
		// 释放临时引用
#define SAFE_RELEASE_ARRAY(arr, count) \
	for (UINT i = 0; i < count; ++i) SAFE_RELEASE(arr[i])
		SAFE_RELEASE(state.pInputLayout);
		SAFE_RELEASE_ARRAY(state.pVertexBuffers, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT);
		SAFE_RELEASE(state.pIndexBuffer);
		SAFE_RELEASE(state.pVS);
		SAFE_RELEASE_ARRAY(state.pVSCBuffers, MAX_CB_SLOTS);
		SAFE_RELEASE_ARRAY(state.pVSSRVs, MAX_SRV_SLOTS);
		SAFE_RELEASE_ARRAY(state.pVSSamplers, MAX_SAMPLER_SLOTS);
		SAFE_RELEASE(state.pPS);
		SAFE_RELEASE_ARRAY(state.pPSCBuffers, MAX_CB_SLOTS);
		SAFE_RELEASE_ARRAY(state.pPSSRVs, MAX_SRV_SLOTS);
		SAFE_RELEASE_ARRAY(state.pPSSamplers, MAX_SAMPLER_SLOTS);
		SAFE_RELEASE(state.pRasterizerState);
		SAFE_RELEASE_ARRAY(state.pRTVs, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT);
		SAFE_RELEASE(state.pDSV);
		SAFE_RELEASE(state.pBlendState);
		SAFE_RELEASE(state.pDepthStencilState);
#undef SAFE_RELEASE_ARRAY
	}

	class ScopedContextState
	{
	public:
		explicit ScopedContextState(ID3D11DeviceContext* context) :
			context_(context)
		{
			if (context_) {
				SaveState(context_, state_);
			}
		}

		~ScopedContextState()
		{
			if (context_) {
				RestoreState(context_, state_);
			}
		}

		ScopedContextState(const ScopedContextState&) = delete;
		ScopedContextState& operator=(const ScopedContextState&) = delete;

	private:
		ID3D11DeviceContext* context_;
		SavedState state_{};
	};

	RE::NiPoint3 D3D::WorldToScreen(RE::NiAVObject* cam, RE::NiAVObject* obj,float fov)
	{
		if (!obj) {
			return {};
		}
		return WorldPointToScreen(cam, obj->world.translate, fov);
	}

	RE::NiPoint3 D3D::WorldPointToScreen(
		RE::NiAVObject* cam,
		const RE::NiPoint3& worldPoint,
		float fov)
	{
		// Stages 0 through 2 deliberately do not install MagnaScope's DX11
		// hooks, so OnResize() has not populated these dimensions yet. Resolve
		// them from the renderer window before testing the projection wrapper;
		// otherwise Stage 1 silently returns the origin fallback and never
		// exercises the crash-isolation boundary it is intended to verify.
		if (windowWidth <= 0 || windowHeight <= 0) {
			const auto* renderWindow = RE::BSGraphics::GetCurrentRendererWindow();
			if (renderWindow) {
				windowWidth = static_cast<int>(renderWindow->windowWidth);
				windowHeight = static_cast<int>(renderWindow->windowHeight);
			}
		}
		if (windowWidth <= 0 || windowHeight <= 0) {
			return {
				0.0F,
				0.0F,
				1.0F
			};
		}

		// STS aperture geometry is part of the first-person scene graph. Its
		// coordinates are camera-relative weapon geometry, not main-world
		// coordinates. HUDMenuUtils::WorldPtToScreenPt3 therefore returned
		// (0,0,0) even with the correct function signature.
		//
		// Preserve the original FTS camera contract exactly where it matters:
		// NiMatrix3 stores the first-person Camera rotation in the convention
		// consumed by `camera.world.rotate * (point - camera.translation)`.
		// In that resulting view space, negative Z is forward, X is horizontal,
		// and Y is vertical. The previous inverse-transform/Y-forward
		// replacement produced finite but demonstrably off-screen coordinates
		// for live ScopeFade geometry.
		if (!cam) {
			return {
				static_cast<float>(windowWidth) * 0.5F,
				static_cast<float>(windowHeight) * 0.5F,
				1.0F
			};
		}

		const auto projectOriginalFTS = [&](const RE::NiPoint3& cameraPoint) {
			const float aspect =
				static_cast<float>(windowWidth) / static_cast<float>(windowHeight);
			const float fovRadians = std::clamp(
				fov > XM_PI ? XMConvertToRadians(fov) : fov,
				XMConvertToRadians(1.0F),
				XMConvertToRadians(179.0F));
			const float halfHeight = std::tan(fovRadians * 0.5F);
			const float forward = -cameraPoint.z;
			if (!std::isfinite(forward) || forward <= 0.001F ||
				!std::isfinite(halfHeight) || halfHeight <= 0.0F) {
				return RE::NiPoint3{
					static_cast<float>(windowWidth) * 0.5F,
					static_cast<float>(windowHeight) * 0.5F,
					forward
				};
			}
			const float ndcX =
				-cameraPoint.x / (forward * halfHeight * aspect);
			const float ndcY =
				-cameraPoint.y / (forward * halfHeight);
			return RE::NiPoint3{
				(ndcX + 1.0F) * 0.5F * static_cast<float>(windowWidth),
				(1.0F - ndcY) * 0.5F * static_cast<float>(windowHeight),
				forward
			};
		};

		const RE::NiPoint3 objectWorld = worldPoint;
		const RE::NiPoint3 delta = objectWorld - cam->world.translate;
		const RE::NiPoint3 cameraView = cam->world.rotate * delta;
		const RE::NiPoint3 projected = projectOriginalFTS(cameraView);

		if (player &&
			(player->gunState == RE::GUN_STATE::kSighted ||
			 player->gunState == RE::GUN_STATE::kFireSighted)) {
			static std::once_flag loggedProjectionContract;
			std::call_once(loggedProjectionContract, [&] {
				logger::info(
					"First-person aperture projection (original FTS camera contract): cameraView=({:.4f}, {:.4f}, {:.4f}), pixel=({:.2f}, {:.2f}, depth={:.4f}), viewport={}x{}",
					cameraView.x,
					cameraView.y,
					cameraView.z,
					projected.x,
					projected.y,
					projected.z,
					windowWidth,
					windowHeight);
			});
		}

		return projected;
	}

	D3D::ScreenSphereProjection D3D::ProjectWorldSphereToScreen(
		RE::NiAVObject* cam,
		const RE::NiPoint3& worldCenter,
		float worldRadius,
		float fov)
	{
		ScreenSphereProjection result{};
		if (!cam ||
			!std::isfinite(worldCenter.x) ||
			!std::isfinite(worldCenter.y) ||
			!std::isfinite(worldCenter.z) ||
			!std::isfinite(worldRadius) ||
			worldRadius <= 0.001F) {
			return result;
		}

		// Reuse WorldPointToScreen first so the projection viewport is resolved
		// through the same renderer-window fallback used by the center-only
		// verification stages.
		result.center = WorldPointToScreen(cam, worldCenter, fov);
		if (windowWidth <= 0 || windowHeight <= 0 ||
			!std::isfinite(result.center.x) ||
			!std::isfinite(result.center.y) ||
			!std::isfinite(result.center.z) ||
			result.center.z <= 0.001F) {
			return result;
		}

		const float aspect =
			static_cast<float>(windowWidth) / static_cast<float>(windowHeight);
		const float fovRadians = std::clamp(
			fov > XM_PI ? XMConvertToRadians(fov) : fov,
			XMConvertToRadians(1.0F),
			XMConvertToRadians(179.0F));
		const float halfHeight = std::tan(fovRadians * 0.5F);
		if (!std::isfinite(aspect) || aspect <= 0.0F ||
			!std::isfinite(halfHeight) || halfHeight <= 0.0F) {
			return result;
		}

		// worldBound.fRadius is already scaled into world units. Transform the
		// bound center once through the verified original FTS camera contract,
		// then offset it along camera X and Y. A sphere has the same radius in
		// every orientation, so no scene-graph basis reconstruction is needed.
		const RE::NiPoint3 cameraCenter =
			cam->world.rotate * (worldCenter - cam->world.translate);
		const auto projectCameraPoint = [&](const RE::NiPoint3& cameraPoint) {
			const float forward = -cameraPoint.z;
			if (!std::isfinite(forward) || forward <= 0.001F) {
				return RE::NiPoint3{
					result.center.x,
					result.center.y,
					forward
				};
			}

			const float ndcX =
				-cameraPoint.x / (forward * halfHeight * aspect);
			const float ndcY =
				-cameraPoint.y / (forward * halfHeight);
			return RE::NiPoint3{
				(ndcX + 1.0F) * 0.5F * static_cast<float>(windowWidth),
				(1.0F - ndcY) * 0.5F * static_cast<float>(windowHeight),
				forward
			};
		};

		RE::NiPoint3 horizontalEdge = cameraCenter;
		horizontalEdge.x += worldRadius;
		RE::NiPoint3 verticalEdge = cameraCenter;
		verticalEdge.y += worldRadius;
		const RE::NiPoint3 projectedHorizontal =
			projectCameraPoint(horizontalEdge);
		const RE::NiPoint3 projectedVertical =
			projectCameraPoint(verticalEdge);

		result.radiusX = std::abs(projectedHorizontal.x - result.center.x);
		result.radiusY = std::abs(projectedVertical.y - result.center.y);
		result.valid =
			std::isfinite(result.radiusX) &&
			std::isfinite(result.radiusY) &&
			result.radiusX > 1.0F &&
			result.radiusY > 1.0F &&
			result.radiusX < static_cast<float>(windowWidth) &&
			result.radiusY < static_cast<float>(windowHeight);
		return result;
	}

	bool D3D::InitEffect()
	{
		ComPtr<ID3DBlob> blob;
		if (FAILED(CreateShaderFromFile(L"Data\\Shaders\\MagnaScope\\ScopeEffect_PS.cso", L"src\\HLSL\\ScopeEffect_PS.hlsl", "main", "ps_5_0", blob.ReleaseAndGetAddressOf())) || !blob.Get())
			return false;
		HR(g_Device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, m_pPixelShader.GetAddressOf()));

		if (FAILED(CreateShaderFromFile(L"Data\\Shaders\\MagnaScope\\ScopeEffect_PS_Output.cso", L"src\\HLSL\\ScopeEffect_PS_Output.hlsl", "main", "ps_5_0", blob.ReleaseAndGetAddressOf())) || !blob.Get())
			return false;
		HR(g_Device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, m_outPutPixelShader.GetAddressOf()));

		if (FAILED(CreateShaderFromFile(L"Data\\Shaders\\MagnaScope\\ScopeEffect_VS.cso", L"src\\HLSL\\ScopeEffect_VS.hlsl", "main", "vs_5_0", blob.ReleaseAndGetAddressOf())) || !blob.Get())
			return false;
		HR(g_Device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, m_pVertexShader.GetAddressOf()));

		HR(g_Device->CreateInputLayout(gdc_layout, ARRAYSIZE(gdc_layout), blob->GetBufferPointer(), blob->GetBufferSize(), &gdc_pVertexLayout));
		
		if (FAILED(CreateShaderFromFile(L"Data\\Shaders\\MagnaScope\\ScopeEffect_VS_Output.cso", L"src\\HLSL\\ScopeEffect_VS_Output.hlsl", "main", "vs_5_0", blob.ReleaseAndGetAddressOf())) || !blob.Get())
			return false;
		HR(g_Device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, m_outPutVertexShader.GetAddressOf()));

		return m_pPixelShader.Get() && m_outPutPixelShader.Get() && m_pVertexShader.Get() && m_outPutVertexShader.Get();
	}

	bool D3D::InitLegacyEffect()
	{
		ComPtr<ID3DBlob> blob;

		if (FAILED(CreateShaderFromFile(L"Data\\Shaders\\MagnaScope\\AutoSTS_PS.cso", L"src\\HLSL\\AutoSTS_PS.hlsl", "main", "ps_5_0", blob.ReleaseAndGetAddressOf())) || !blob.Get())
			return false;
		HR(g_Device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, m_pPixelShader_AutoSTS.GetAddressOf()));

		if (FAILED(CreateShaderFromFile(L"Data\\Shaders\\MagnaScope\\ScopeEffect_PS_Legacy.cso", L"src\\HLSL\\ScopeEffect_PS_Legacy.hlsl", "main", "ps_5_0", blob.ReleaseAndGetAddressOf())) || !blob.Get())
			return false;
		HR(g_Device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, m_pPixelShader_Legacy.GetAddressOf()));

		if (FAILED(CreateShaderFromFile(L"Data\\Shaders\\MagnaScope\\ScopeEffect_PS_Output_Legacy.cso", L"src\\HLSL\\ScopeEffect_PS_Output_Legacy.hlsl", "main", "ps_5_0", blob.ReleaseAndGetAddressOf())) || !blob.Get())
			return false;
		HR(g_Device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, m_outPutPixelShader_Legacy.GetAddressOf()));

		if (FAILED(CreateShaderFromFile(L"Data\\Shaders\\MagnaScope\\ScopeEffect_VS_Legacy.cso", L"src\\HLSL\\ScopeEffect_VS_Legacy.hlsl", "main", "vs_5_0", blob.ReleaseAndGetAddressOf())) || !blob.Get())
			return false;
		HR(g_Device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, m_pVertexShader_Legacy.GetAddressOf()));
		return m_pPixelShader_AutoSTS.Get() && m_pPixelShader_Legacy.Get() && m_outPutPixelShader_Legacy.Get() && m_pVertexShader_Legacy.Get();
	}

	bool D3D::InitGeometryProbeEffect()
	{
		ComPtr<ID3DBlob> pixelBlob;
		if (FAILED(CreateShaderFromFile(
				L"Data\\Shaders\\MagnaScope\\ScopeGeometryProbe_PS.cso",
				L"src\\HLSL\\ScopeGeometryProbe_PS.hlsl",
				"main",
				"ps_5_0",
				pixelBlob.ReleaseAndGetAddressOf())) ||
			!pixelBlob.Get()) {
			logger::error(
				"Stage 4d geometry probe shader could not be loaded; "
				"the ScopeFade draw will remain untouched");
			return false;
		}

		const HRESULT pixelResult = g_Device->CreatePixelShader(
			pixelBlob->GetBufferPointer(),
			pixelBlob->GetBufferSize(),
			nullptr,
			m_pPixelShader_STSGeometryProbe.ReleaseAndGetAddressOf());
		if (FAILED(pixelResult) || !m_pPixelShader_STSGeometryProbe.Get()) {
			logger::error(
				"Stage 4d geometry probe pixel shader creation failed: 0x{:08X}",
				static_cast<std::uint32_t>(pixelResult));
			return false;
		}

		ComPtr<ID3DBlob> geometryBlob;
		if (FAILED(CreateShaderFromFile(
				L"Data\\Shaders\\MagnaScope\\ScopeGeometryFill_GS.cso",
				L"src\\HLSL\\ScopeGeometryFill_GS.hlsl",
				"main",
				"gs_5_0",
				geometryBlob.ReleaseAndGetAddressOf())) ||
			!geometryBlob.Get()) {
			logger::error(
				"Stage 4d geometry fill shader could not be loaded; "
				"the ScopeFade draw will remain untouched");
			m_pPixelShader_STSGeometryProbe.Reset();
			return false;
		}

		const HRESULT geometryResult = g_Device->CreateGeometryShader(
			geometryBlob->GetBufferPointer(),
			geometryBlob->GetBufferSize(),
			nullptr,
			m_pGeometryShader_STSGeometryFill.ReleaseAndGetAddressOf());
		if (FAILED(geometryResult) ||
			!m_pGeometryShader_STSGeometryFill.Get()) {
			logger::error(
				"Stage 4d geometry fill shader creation failed: 0x{:08X}",
				static_cast<std::uint32_t>(geometryResult));
			m_pPixelShader_STSGeometryProbe.Reset();
			return false;
		}

		logger::info(
			"Stage 4d ScopeFade fill shaders initialized; only an exact "
			"published ScopeFade buffer match may use them");
		return true;
	}

	void D3D::CreateBlender()
	{
		// 1. Alpha-To-Coverage 混合状态
		D3D11_BLEND_DESC blendDesc = {};
		blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		HR(g_Device->CreateBlendState(&blendDesc, BSAlphaToCoverage.GetAddressOf()));

		// 2. 透明混合状态 (BSTransparent)
		blendDesc = {};
		blendDesc.AlphaToCoverageEnable = false;
		blendDesc.IndependentBlendEnable = false;
		auto& rtDesc = blendDesc.RenderTarget[0];
		rtDesc.BlendEnable = true;
		rtDesc.SrcBlend = D3D11_BLEND_SRC_ALPHA;
		rtDesc.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
		rtDesc.BlendOp = D3D11_BLEND_OP_ADD;
		// The scope pass changes RGB only. Replacing destination alpha with
		// shader alpha wrote zero across the entire frame outside the lens,
		// which can invalidate ENB/upscaler composition metadata even though
		// the visible RGB looked unchanged.
		rtDesc.SrcBlendAlpha = D3D11_BLEND_ZERO;
		rtDesc.DestBlendAlpha = D3D11_BLEND_ONE;
		rtDesc.BlendOpAlpha = D3D11_BLEND_OP_ADD;
		rtDesc.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		HR(g_Device->CreateBlendState(&blendDesc, BSTransparent.GetAddressOf()));
	}

	void CreateConstantBuffer(ID3D11Device* device, ID3D11Buffer** buffer, UINT byteWidth)
	{
		D3D11_BUFFER_DESC desc = {};
		desc.Usage = D3D11_USAGE_DYNAMIC;
		desc.ByteWidth = byteWidth;
		desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		HR(device->CreateBuffer(&desc, nullptr, buffer));
	}

	template <typename T>
	void CreateVertexBuffer(ID3D11Device* device, ID3D11Buffer** buffer, const T* data, UINT count)
	{
		D3D11_BUFFER_DESC desc = {};
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.ByteWidth = sizeof(T) * count;
		desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
		D3D11_SUBRESOURCE_DATA initData = { data };
		HR(device->CreateBuffer(&desc, &initData, buffer));
	}

	template <typename T>
	void CreateIndexBuffer(ID3D11Device* device, ID3D11Buffer** buffer, const T* data, UINT count)
	{
		D3D11_BUFFER_DESC desc = {};
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.ByteWidth = sizeof(T) * count;
		desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
		D3D11_SUBRESOURCE_DATA initData = { data };
		HR(device->CreateBuffer(&desc, &initData, buffer));
	}


	bool D3D::InitResource()
	{
		// 常量缓冲区
		CreateConstantBuffer(g_Device.Get(), m_pScopeEffectBuffer.GetAddressOf(), sizeof(ScopeEffectShaderData));
		CreateConstantBuffer(g_Device.Get(), m_pConstantBufferData.GetAddressOf(), sizeof(ConstBufferData));

		// 动态常量缓冲区（示例）
		D3D11_BUFFER_DESC outputDesc = {};
		outputDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		outputDesc.Usage = D3D11_USAGE_DYNAMIC;
		outputDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		outputDesc.ByteWidth = 48;
		HR(g_Device->CreateBuffer(&outputDesc, nullptr, &targetVertexConstBufferOutPut));
		outputDesc.ByteWidth = 384;
		HR(g_Device->CreateBuffer(&outputDesc, nullptr, &targetVertexConstBufferOutPut1p5));
		outputDesc.ByteWidth = 752;
		HR(g_Device->CreateBuffer(&outputDesc, nullptr, &targetVertexConstBufferOutPut1));

		// 顶点/索引缓冲区
		CreateVertexBuffer(g_Device.Get(), &gdc_pVertexBuffer, gdc_Vertices, ARRAYSIZE(gdc_Vertices));
		CreateIndexBuffer(g_Device.Get(), &gdc_pIndexBuffer, gdc_Indices, ARRAYSIZE(gdc_Indices));

		// 采样器状态
		D3D11_SAMPLER_DESC sampDesc = {};
		sampDesc.Filter = D3D11_FILTER_ANISOTROPIC;
		sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sampDesc.MaxAnisotropy = 16;
		sampDesc.MinLOD = 0;
		sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
		HR(g_Device->CreateSamplerState(&sampDesc, m_pSamplerState.GetAddressOf()));

		CreateBlender();
		return true;
	}

	void CreateTextureAndViews(
		ID3D11Device* device,UINT width, UINT height, DXGI_FORMAT format,
		ID3D11Texture2D** texture,ID3D11RenderTargetView** rtv = nullptr,ID3D11ShaderResourceView** srv = nullptr)
	{
		D3D11_TEXTURE2D_DESC texDesc = {};
		texDesc.Width = width;
		texDesc.Height = height;
		texDesc.MipLevels = 1;
		texDesc.ArraySize = 1;
		texDesc.Format = format;
		texDesc.SampleDesc.Count = 1;
		texDesc.Usage = D3D11_USAGE_DEFAULT;
		texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		HR(device->CreateTexture2D(&texDesc, nullptr, texture));

		if (rtv) {
			D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
			rtvDesc.Format = format;
			rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
			HR(device->CreateRenderTargetView(*texture, &rtvDesc, rtv));
		}

		if (srv) {
			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
			srvDesc.Format = format;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MipLevels = 1;
			HR(device->CreateShaderResourceView(*texture, &srvDesc, srv));
		}
	}

	void D3D::OnResize()
	{
		if (!g_Swapchain.Get() || !g_Device.Get()) {
			return;
		}

		DXGI_SWAP_CHAIN_DESC swapChainDesc{};
		if (SUCCEEDED(g_Swapchain->GetDesc(&swapChainDesc))) {
			windowWidth = static_cast<int>(swapChainDesc.BufferDesc.Width);
			windowHeight = static_cast<int>(swapChainDesc.BufferDesc.Height);
		}
		if (windowWidth <= 0 || windowHeight <= 0) {
			const auto* renderWindow = RE::BSGraphics::GetCurrentRendererWindow();
			if (!renderWindow) {
				logger::error("Unable to determine render dimensions");
				return;
			}
			windowWidth = renderWindow->windowWidth;
			windowHeight = renderWindow->windowHeight;
		}

		//------------------------------
		// 1. 创建平面索引缓冲区
		//------------------------------
		CreateIndexBuffer(g_Device.Get(), plane_pIndexBuffer.GetAddressOf(), plane_Indices, ARRAYSIZE(plane_Indices));

		//------------------------------
		// 2. 创建深度模板缓冲区和视图
		//------------------------------
		// 深度模板纹理描述
		D3D11_TEXTURE2D_DESC depthStencilDesc = {};
		depthStencilDesc.Width = windowWidth;
		depthStencilDesc.Height = windowHeight;
		depthStencilDesc.MipLevels = 1;
		depthStencilDesc.ArraySize = 1;
		depthStencilDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
		depthStencilDesc.SampleDesc.Count = 1;
		depthStencilDesc.Usage = D3D11_USAGE_DEFAULT;
		depthStencilDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

		// 创建深度模板纹理和视图
		HR(g_Device->CreateTexture2D(&depthStencilDesc, nullptr, m_pDepthStencilBuffer.GetAddressOf()));
		HR(g_Device->CreateDepthStencilView(m_pDepthStencilBuffer.Get(), nullptr, m_pDepthStencilView.GetAddressOf()));

		//------------------------------
		// 3. 创建主渲染目标纹理及视图
		//------------------------------
		CreateTextureAndViews(
			g_Device.Get(),
			windowHeight, windowHeight, DXGI_FORMAT_R8G8B8A8_UNORM,
			mRTRenderTargetTexture.GetAddressOf(),
			mRTRenderTargetView.GetAddressOf(),
			mRTShaderResourceView.GetAddressOf());

		//------------------------------
		// 4. 创建目标纹理及SRV（仅绑定到着色器资源）
		//------------------------------

		// 创建目标纹理

		// 创建关联的SRV
	}

	void D3D::ReleaseSizeDependentResources()
	{
		// ResizeBuffers requires every reference to the swap-chain back buffer
		// to be released before the original call executes.
		m_pDepthStencilBuffer.Reset();
		m_pDepthStencilView.Reset();
		m_pRenderTargetView.Reset();
		mRTRenderTargetTexture.Reset();
		mRTRenderTargetView.Reset();
		mRTShaderResourceView.Reset();
		mCurRTTexture.Reset();
		rtTexture2D.Reset();
		mShaderResourceView.Reset();
		mBackBuffer.Reset();

		SAFE_RELEASE(tempRt[0]);
		SAFE_RELEASE(tempRt[1]);
		SAFE_RELEASE(tempSV);
		SAFE_RELEASE(targetVertexConstBufferOutPut);
		SAFE_RELEASE(targetVertexConstBufferOutPut1p5);
		SAFE_RELEASE(targetVertexConstBufferOutPut1);
		SAFE_RELEASE(gdc_pVertexBuffer);
		SAFE_RELEASE(gdc_pVertexLayout);
		SAFE_RELEASE(gdc_pIndexBuffer);
		mBackBufferIndex = UINT_MAX;
		bHasGetBackBuffer = false;
		bIsFirst = true;
	}


	void D3D::LoadAimTexture(const std::string& path)
	{
		D3DInstance->mTextDDS_SRV.Reset();
		if (path.empty()) {
			return;
		}

		std::wstring defaultPath = L"Data/Textures/FTS/Empty.dds";
		const wchar_t* tempPath = GetWC(path.c_str());

		HRESULT result = CreateDDSTextureFromFile(
			g_Device.Get(),
			tempPath ? tempPath : defaultPath.c_str(),
			nullptr,
			D3DInstance->mTextDDS_SRV.ReleaseAndGetAddressOf());
		if (FAILED(result) && tempPath) {
			logger::warn("Reticle texture {} failed to load; using Empty.dds", path);
			result = CreateDDSTextureFromFile(
				g_Device.Get(),
				defaultPath.c_str(),
				nullptr,
				D3DInstance->mTextDDS_SRV.ReleaseAndGetAddressOf());
		}
		if (FAILED(result)) {
			logger::error("No reticle texture could be loaded (HRESULT 0x{:08X})", static_cast<std::uint32_t>(result));
		}

		if (tempPath) free((void*)tempPath);
	}

	// 通用缓冲区更新模板
	template <typename T>
	void D3D::UpdateConstantBuffer(const ComPtr<ID3D11Buffer>& buffer, const T& data)
	{
		D3D11_MAPPED_SUBRESOURCE mapped;
		HR(g_Context->Map(buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
		memcpy_s(mapped.pData, sizeof(T), &data, sizeof(T));
		g_Context->Unmap(buffer.Get(), 0);
	}

	void D3D::UpdateGameConstants(const GameConstBuffer& src, ScopeEffectShaderData& dst)
	{
		using namespace detail;
		// A later explicit assignment repairs the incomplete legacy matrix
		// copy below without changing the serialized constant-buffer layout.
		// 矩阵内存直接复制
		memcpy_s(&dst.CameraRotation, sizeof(float[16]), &src.camMat.entry[0], sizeof(float[9]));  // 只复制3x3部分，之前是float16的，注意看看有没有错误

		// Populate all matrix lanes explicitly. The preceding 36-byte legacy
		// copy crosses row boundaries and leaves the 4x4 tail undefined.
		dst.CameraRotation = {
			src.camMat.entry[0][0], src.camMat.entry[0][1], src.camMat.entry[0][2], 0.0F,
			src.camMat.entry[1][0], src.camMat.entry[1][1], src.camMat.entry[1][2], 0.0F,
			src.camMat.entry[2][0], src.camMat.entry[2][1], src.camMat.entry[2][2], 0.0F,
			0.0F, 0.0F, 0.0F, 1.0F
		};

		CopyVector3(dst.CurrRootPos, src.rootPos);
		CopyVector3(dst.CurrWeaponPos, src.weaponPos);
		CopyVector3(dst.eyeDirection, src.virDir);
		CopyVector3(dst.eyeDirectionLerp, src.VirDirLerp);
		CopyVector3(dst.eyeTranslationLerp, src.VirTransLerp);

		// 二维坐标
		dst.FTS_ScreenPos = { src.ftsScreenPos.x, src.ftsScreenPos.y };
	}


	void D3D::UpdateScene(FTSData* currData)
	{
		if (bChangeAimTexture) {
			LoadAimTexture(currData->ZoomNodePath);
			bChangeAimTexture = false;
		}

#pragma region FO4GameConstantBuffer

		scopeData.EnableMerge = isEnableScopeEffect ? 1 : 0;
		if (!isEnableScopeEffect) {
			scopeData.ScopeEffect_Zoom = currData->shaderData.minZoom;
		}

		UpdateGameConstants(gameConstBuffer, scopeData);
		scopeData.targetAdjustFov = pcam->fovAdjustCurrent;

		// In edit mode the menu's unsaved slider values bound the zoom, so the
		// magnification controls preview live instead of waiting for a save.
		const float zoomMin = bEnableEditMode ? editZoomMin : currData->shaderData.minZoom;
		const float zoomMax = std::max(
			zoomMin,
			bEnableEditMode ? editZoomMax : currData->shaderData.maxZoom);

		if (bResetZoomDelta) {
			gameZoomDelta = zoomMin;
			bResetZoomDelta = false;
		}

		gameZoomDelta = std::clamp(gameZoomDelta, zoomMin, zoomMax);

		scopeData.ScopeEffect_Zoom = gameZoomDelta;
		scopeData.GameFov = pcam->firstPersonFOV;

#pragma endregion

#pragma region MyScopeShaderData
		if (!bEnableEditMode) {
			const auto& shaderData = currData->shaderData;
			legacyFlag = currData->legacyMode;
			bLegacyMode = currData->legacyMode;

			// 二维参数处理
			const auto ToFloat2 = [](const auto& arr) { return XMFLOAT2(arr[0], arr[1]); };
			const auto ToFloat4 = [](const auto& arr) { return XMFLOAT4(arr[0], arr[1], arr[2], arr[3]); };

			// 基本参数映射
			scopeData.reticle_Offset = ToFloat2(shaderData.reticle_Offset);
			scopeData.BaseWeaponPos = shaderData.baseWeaponPos;
			scopeData.camDepth = shaderData.camDepth;
			scopeData.EnableNV = shaderData.bCanEnableNV ? bEnableNVG : 0;
			scopeData.EnableZMove = shaderData.bEnableZMove;
			scopeData.isCircle = shaderData.IsCircle;
			scopeData.MovePercentage = shaderData.movePercentage;
			scopeData.nvIntensity = shaderData.nvIntensity;
			scopeData.ReticleSize = shaderData.ReticleSize;
			scopeData.ScopeEffect_Offset = ToFloat2(LFA(shaderData.PositionOffset, 2));
			scopeData.ScopeEffect_OriPositionOffset = ToFloat2(LFA(shaderData.OriPositionOffset, 2));
			scopeData.ScopeEffect_OriSize = ToFloat2(shaderData.OriSize);
			scopeData.ScopeEffect_Size = ToFloat2(LFA(shaderData.Size, 2));
			scopeData.rect = ToFloat4(LFA(shaderData.rectSize, 4));

			// 视差参数
			const auto& parallax = shaderData.parallax;
			scopeData.parallax_maxTravel = parallax.maxTravel;
			scopeData.parallax_Radius = parallax.radius;
			scopeData.parallax_relativeFogRadius = parallax.relativeFogRadius;
			scopeData.parallax_scopeSwayAmount = parallax.scopeSwayAmount;
			scopeData.baseFovAdjustTarget = shaderData.fovAdjust;

			scopeData.FishEyeStrength = shaderData.fishEyeStrength;
			scopeData.FishEyePower = shaderData.fishEyePower;
		}
#pragma endregion

		// 区域5: 统一更新常量缓冲区
		UpdateConstantBuffer(m_pScopeEffectBuffer, scopeData);

		// 特殊常量缓冲区
		constBufferData.width = windowWidth;
		constBufferData.height = windowHeight;
		UpdateConstantBuffer(m_pConstantBufferData, constBufferData);
		
	}

	void D3D::ScreenTextureMod()
	{
		UINT stride = sizeof(::Vertex);
		UINT offset = 0;
		const std::vector<VSConstantBufferSlot> noVertexConstants;
		SetupCommonRenderState(
			m_pVertexShader.Get(),
			nullptr,
			0,
			m_pPixelShader.Get(),
			gdc_pVertexLayout,
			BSTransparent.Get(),
			noVertexConstants,
			gdc_pIndexBuffer,
			DXGI_FORMAT_R32_UINT,
			0,
			&gdc_pVertexBuffer,
			&stride,
			&offset,
			1,
			mRTRenderTargetView.Get());

		// 指定图元类型为三角形列表
		g_Context->PSSetShaderResources(4, 1, D3DInstance->mShaderResourceView.GetAddressOf());
		g_Context->PSSetShaderResources(5, 1, D3DInstance->mTextDDS_SRV.GetAddressOf());

		bSelfDraw = true;
		g_Context->DrawIndexed(3, 0, 0);
		bSelfDraw = false;

		g_Context->OMSetRenderTargets(2, tempRt, tempSV);
	}

	 // 完整的公共渲染状态设置
	void D3D::SetupCommonRenderState(
		ID3D11VertexShader* vs, ID3D11ClassInstance* const* vsClassInstances, UINT vsClassInstancesCount,
		ID3D11PixelShader* ps, ID3D11InputLayout* inputLayout, ID3D11BlendState* blendState, const std::vector<VSConstantBufferSlot>& vsCBSlots,
		ID3D11Buffer* indexBuffer,DXGI_FORMAT indexFormat,UINT indexOffset,
		ID3D11Buffer* const* vertexBuffers, const UINT* strides, const UINT* offsets, UINT numVertexBuffers, ID3D11RenderTargetView* backBufferRTV = nullptr)
	{
		// === 1. 深度模板状态配置 ===
		D3D11_DEPTH_STENCIL_DESC tempDSD{};
		D3D11_DEPTH_STENCILOP_DESC tempDSOPD{};
		tempDSOPD.StencilFailOp = D3D11_STENCIL_OP_KEEP;
		tempDSOPD.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
		tempDSOPD.StencilPassOp = D3D11_STENCIL_OP_KEEP;
		tempDSOPD.StencilFunc = D3D11_COMPARISON_ALWAYS;

		tempDSD.DepthEnable = false;
		tempDSD.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
		tempDSD.DepthFunc = D3D11_COMPARISON_LESS;
		tempDSD.StencilEnable = false;
		tempDSD.StencilReadMask = 255;
		tempDSD.StencilWriteMask = 255;
		tempDSD.FrontFace = tempDSOPD;
		tempDSD.BackFace = tempDSOPD;

		Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthStencilState;
		HR(g_Device->CreateDepthStencilState(&tempDSD, depthStencilState.GetAddressOf()));

		// === 2. 核心渲染状态设置 ===
		// An explicit target (the swap chain back buffer at the Present
		// anchor) wins; otherwise composite into the game's bound target,
		// which is only correct mid-pipeline (TAA anchor).
		ID3D11RenderTargetView* rtvs[1] = {};
		rtvs[0] = backBufferRTV ? backBufferRTV :
		          (upscalerMod ? tempRt[0] : (tempRt[1] ? tempRt[1] : tempRt[0]));
		g_Context->OMSetRenderTargets(1, rtvs, backBufferRTV ? nullptr : tempSV);

		// The fullscreen pass must cover the whole destination; the viewport
		// the game left bound (especially at Present time, after UI work) can
		// be a sub-rectangle. ScopedContextState restores the original.
		if (rtvs[0]) {
			Microsoft::WRL::ComPtr<ID3D11Resource> targetResource;
			rtvs[0]->GetResource(targetResource.GetAddressOf());
			Microsoft::WRL::ComPtr<ID3D11Texture2D> targetTexture;
			if (targetResource && SUCCEEDED(targetResource.As(&targetTexture))) {
				D3D11_TEXTURE2D_DESC targetDesc{};
				targetTexture->GetDesc(&targetDesc);
				D3D11_VIEWPORT viewport{};
				viewport.Width = static_cast<float>(targetDesc.Width);
				viewport.Height = static_cast<float>(targetDesc.Height);
				viewport.MaxDepth = 1.0F;
				g_Context->RSSetViewports(1, &viewport);
			}
		}

		// Never inherit the game's rasterizer state. At Present time the last
		// pass is Scaleform UI, which leaves scissor testing enabled with a
		// stale UI rectangle; that clips a fullscreen triangle down to
		// nothing even though the draw call itself succeeds. An explicit
		// state with scissor off makes the pass anchor-independent.
		// ScopedContextState restores the game's state afterwards.
		D3D11_RASTERIZER_DESC rasterDesc{};
		rasterDesc.FillMode = D3D11_FILL_SOLID;
		rasterDesc.CullMode = D3D11_CULL_NONE;
		rasterDesc.DepthClipEnable = TRUE;
		rasterDesc.ScissorEnable = FALSE;
		Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterState;
		if (SUCCEEDED(g_Device->CreateRasterizerState(&rasterDesc, rasterState.GetAddressOf()))) {
			g_Context->RSSetState(rasterState.Get());
		}
		// 复制资源（关键修复点）
		// 设置着色器
		g_Context->VSSetShader(vs, vsClassInstances, vsClassInstancesCount);  // 修复类实例传递
		g_Context->PSSetShader(ps, nullptr, 0);

		// 设置公共常量缓冲区
		g_Context->PSSetConstantBuffers(4, 1, m_pConstantBufferData.GetAddressOf());
		g_Context->PSSetConstantBuffers(5, 1, m_pScopeEffectBuffer.GetAddressOf());

		// === 3. 顶点相关设置 ===
		// 设置输入布局
		g_Context->IASetInputLayout(inputLayout);

		// 设置顶点缓冲区（修复指针传递）
		g_Context->IASetVertexBuffers(0, numVertexBuffers, vertexBuffers, strides, offsets);

		// 设置索引缓冲区
		g_Context->IASetIndexBuffer(indexBuffer, indexFormat, indexOffset);

		// === 4. 混合和深度状态 ===
		g_Context->OMSetBlendState(blendState, nullptr, 0xFFFFFFFF);
		g_Context->OMSetDepthStencilState(depthStencilState.Get(), 0);

		// === 5. 着色器资源 ===
		g_Context->PSSetSamplers(0, 1, m_pSamplerState.GetAddressOf());

		// === 6. 顶点着色器常量缓冲区 ===
		for (const auto& slot : vsCBSlots) {
			g_Context->VSSetConstantBuffers(slot.StartSlot, slot.NumBuffers, slot.ppBuffers);
		}

		// === 7. 图元拓扑 ===
		g_Context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	}

	void D3D::RenderToReticleTexture()
	{
		if (!bIsFirst && isEnableRender) {
			// 旧版特定参数

			std::vector<VSConstantBufferSlot> vsCBuffersSlots = {
				{ 1, 1, &targetVertexConstBufferOutPut },
				{ 2, 1, &targetVertexConstBufferOutPut1p5 },
			};

			UINT strides = sizeof(::Vertex);
			UINT offsets = 0;

			// No staging copy is needed here: Render() already duplicated the
			// live render target into mCurRTTexture with a matching format.
			// The old m_DstTexture path relied on CopyResource into a fixed
			// R8G8B8A8_UNORM texture, which silently no-ops when the bound
			// render target uses any other format and left the scope black.
			//
			// At the Present anchor the composite must land in the swap chain
			// back buffer to be visible; at the TAA anchor the game's bound
			// target is still mid-pipeline and correct.
			ID3D11RenderTargetView* const compositeTarget =
				renderedAtTAAThisFrame ? nullptr : m_pRenderTargetView.Get();
			const auto* currentProfile =
				ScopeData::ScopeDataHandler::GetSingleton()->GetCurrentFTSData();
			ID3D11PixelShader* const pixelShader =
				currentProfile && currentProfile->autoProfile ?
					m_pPixelShader_AutoSTS.Get() :
					m_pPixelShader_Legacy.Get();
			SetupCommonRenderState(m_pVertexShader_Legacy.Get(), nullptr, 0, pixelShader, gdc_pVertexLayout, BSTransparent.Get(),
				vsCBuffersSlots, gdc_pIndexBuffer, DXGI_FORMAT_R32_UINT, 0, &gdc_pVertexBuffer, &strides, &offsets, 1, compositeTarget);

			// 旧版特定资源
			g_Context->PSSetShaderResources(4, 1, D3DInstance->mShaderResourceView.GetAddressOf());
			g_Context->PSSetShaderResources(5, 1, D3DInstance->mTextDDS_SRV.GetAddressOf());

			bSelfDraw = true;
			g_Context->DrawIndexed(3, 0, 0);
			bSelfDraw = false;
		}
	}

	void D3D::RenderToReticleTextureNew(UINT IndexCount, UINT StartIndexLocation, INT BaseVertexLocation)
	{
		if (targetVS.Get() && targetVertexConstBufferOutPut) {

			std::vector<VSConstantBufferSlot> vsCBuffersSlots = {
				{ 1, 1, &targetVertexConstBufferOutPut },
				{ 2, 1, &targetVertexConstBufferOutPut1p5 },
				{ 12, 1, &targetVertexConstBufferOutPut1 }
			};

			// Same anchor rule as the legacy pass: Present-anchor composites
			// go to the swap chain back buffer.
			ID3D11RenderTargetView* const compositeTarget =
				renderedAtTAAThisFrame ? nullptr : m_pRenderTargetView.Get();
			SetupCommonRenderState(
				targetVS.Get(), targetVSClassInstance.GetAddressOf(), targetVSNumClassesInstance, m_outPutPixelShader.Get(), targetInputLayout.Get(),
				BSTransparent.Get(), vsCBuffersSlots, targetIndexBuffer.Get(), DXGI_FORMAT_R16_UINT, targetIndexBufferOffset, targetVertexBuffer.GetAddressOf(), &targetVertexBufferStrides,
				&targetVertexBufferOffsets, 1, compositeTarget);

			// 新版特有资源绑定
			//g_Context->PSSetShaderResources(1, 1, &nullSRV);  // 显式清空未使用的槽位
			// The intermediate is windowHeight square. Copying it into the
			// full-screen m_DstTexture is invalid when the aspect ratio is not
			// 1:1, so bind its matching SRV directly.
			g_Context->PSSetShaderResources(
				6,
				1,
				mRTShaderResourceView.GetAddressOf());

			bSelfDraw = true;
			g_Context->DrawIndexed(IndexCount, StartIndexLocation, BaseVertexLocation);
			bSelfDraw = false;
		}
	}

	void D3D::MapScopeEffectBuffer(ScopeEffectShaderData data)
	{
		scopeData = data;
	}

	bool IsTargetDrawCall(const BufferInfo& vertexInfo,const BufferInfo& indexInfo,UINT indexCount)
	{
		return vertexInfo.stride == TARGET_STRIDE && indexCount == TARGET_INDEX_COUNT && indexInfo.desc.ByteWidth == TARGET_BUFFER_SIZE && vertexInfo.desc.ByteWidth == TARGET_BUFFER_SIZE;
	}

	bool IsTargetTexture(ID3D11ShaderResourceView* srv)
	{
		if (!srv)
			return false;

		Microsoft::WRL::ComPtr<ID3D11Resource> pResource;
		srv->GetResource(&pResource);

		Microsoft::WRL::ComPtr<ID3D11Texture2D> pTexture2D;
		if (FAILED(pResource.As(&pTexture2D))) return false;

		D3D11_TEXTURE2D_DESC texDesc;
		pTexture2D->GetDesc(&texDesc);

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
		srv->GetDesc(&srvDesc);

		return texDesc.Width == TARGET_TEXTURE_WIDTH && texDesc.Height == TARGET_TEXTURE_WIDTH && texDesc.Format == TARGET_TEXTURE_FORMAT && srvDesc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D;
	}

	UINT GetVertexBuffersInfo(
		ID3D11DeviceContext* pContext,
		std::vector<BufferInfo>& outInfos,
		UINT maxSlotsToCheck = D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT)
	{
		outInfos.clear();

		// 1. 直接尝试获取所有可能的槽位
		std::vector<ID3D11Buffer*> buffers(maxSlotsToCheck);
		std::vector<UINT> strides(maxSlotsToCheck);
		std::vector<UINT> offsets(maxSlotsToCheck);

		pContext->IAGetVertexBuffers(0, maxSlotsToCheck, buffers.data(), strides.data(), offsets.data());

		// 2. 计算实际绑定的缓冲区数量
		UINT actualCount = 0;
		for (UINT i = 0; i < maxSlotsToCheck; ++i) {
			if (buffers[i] != nullptr) {
				actualCount++;
			}
		}

		if (actualCount == 0)
			return 0;

		// 3. 填充输出结构
		outInfos.resize(actualCount);
		UINT validIndex = 0;
		for (UINT i = 0; i < maxSlotsToCheck && validIndex < actualCount; ++i) {
			if (buffers[i] != nullptr) {
				outInfos[validIndex].stride = strides[i];
				outInfos[validIndex].offset = offsets[i];
				buffers[i]->GetDesc(&outInfos[validIndex].desc);
				buffers[i]->Release();  // 释放获取的引用
				validIndex++;
			}
		}

		return actualCount;
	}

	bool GetIndexBufferInfo(ID3D11DeviceContext* pContext, BufferInfo& outInfo)
	{
		Microsoft::WRL::ComPtr<ID3D11Buffer> indexBuffer;
		DXGI_FORMAT format;

		// 获取索引缓冲区
		pContext->IAGetIndexBuffer(&indexBuffer, &format, &outInfo.offset);

		if (!indexBuffer)
			return false;

		// 获取缓冲区描述
		indexBuffer->GetDesc(&outInfo.desc);

		// 将格式信息存入stride（因为索引缓冲区没有stride概念）
		outInfo.stride = (format == DXGI_FORMAT_R32_UINT) ? 4 : 2;

		return true;
	}

	void CopyConstantBuffers(ID3D11DeviceContext* pContext,ID3D11Buffer* srcBuffers[],ID3D11Buffer* dstBuffers[],size_t count)
	{
		for (size_t i = 0; i < count; ++i) {
			if (srcBuffers[i] && dstBuffers[i]) {
				pContext->CopyResource(dstBuffers[i], srcBuffers[i]);
			}
		}
	}

	void __stdcall D3D::DrawIndexedHook(ID3D11DeviceContext* pContext, UINT IndexCount, UINT StartIndexLocation, INT BaseVertexLocation)
	{
		if (bSelfDraw) {
			return oldFuncs.phookD3D11DrawIndexed(pContext, IndexCount, StartIndexLocation, BaseVertexLocation);
		}
		const auto* activeProfile = ScopeData::ScopeDataHandler::GetSingleton()->GetCurrentFTSData();
		if (activeProfile && activeProfile->autoProfile) {
			const auto& verification = MagnaScope::GetSettings();
			// ScopeFade is a child of STS's ScopeAiming branch. The actual draw is therefore the authoritative visibility signal.
			// Do not additionally gate on gunState: Fallout can briefly report
			// a non-sighted firing state while ScopeAiming remains visible,
			// which made Stage 4b and 4c disappear during recoil.
			if (verification.AllowsGeometryProbe() &&
				automaticSTSGeometryReady.load(std::memory_order_acquire) &&
				D3DInstance->m_pGeometryShader_STSGeometryFill.Get() &&
				D3DInstance->m_pPixelShader_STSGeometryProbe.Get()) {
				const auto drawOrdinal =
					automaticSTSDrawOrdinalThisFrame.fetch_add(
						1U,
						std::memory_order_relaxed) +
					1U;
				ComPtr<ID3D11Buffer> currentVertexBuffer;
				ComPtr<ID3D11Buffer> currentIndexBuffer;
				UINT currentStride = 0;
				UINT currentVertexOffset = 0;
				DXGI_FORMAT currentIndexFormat = DXGI_FORMAT_UNKNOWN;
				UINT currentIndexOffset = 0;
				pContext->IAGetVertexBuffers(
					0,
					1,
					currentVertexBuffer.GetAddressOf(),
					&currentStride,
					&currentVertexOffset);
				pContext->IAGetIndexBuffer(
					currentIndexBuffer.GetAddressOf(),
					&currentIndexFormat,
					&currentIndexOffset);

				const bool exactGeometryMatch =
					reinterpret_cast<std::uintptr_t>(
						currentVertexBuffer.Get()) ==
						automaticSTSVertexBuffer.load(
							std::memory_order_relaxed) &&
					reinterpret_cast<std::uintptr_t>(
						currentIndexBuffer.Get()) ==
						automaticSTSIndexBuffer.load(
							std::memory_order_relaxed) &&
					IndexCount ==
						automaticSTSIndexCount.load(
							std::memory_order_relaxed) &&
						currentStride ==
							automaticSTSVertexStride.load(
								std::memory_order_relaxed);
				const auto expectedVertexDataOffset =
					automaticSTSVertexDataOffset.load(
						std::memory_order_relaxed);
				const auto expectedIndexDataOffset =
					automaticSTSIndexDataOffset.load(
						std::memory_order_relaxed);
				const std::int64_t effectiveVertexOffset =
					static_cast<std::int64_t>(currentVertexOffset) +
					static_cast<std::int64_t>(BaseVertexLocation) *
						static_cast<std::int64_t>(currentStride);
				const bool knownIndexFormat =
					currentIndexFormat == DXGI_FORMAT_R16_UINT ||
					currentIndexFormat == DXGI_FORMAT_R32_UINT;
				const std::uint32_t indexElementSize =
					currentIndexFormat == DXGI_FORMAT_R32_UINT ? 4U : 2U;
				const std::uint64_t effectiveIndexOffset =
					static_cast<std::uint64_t>(currentIndexOffset) +
					static_cast<std::uint64_t>(StartIndexLocation) *
						indexElementSize;
				const bool exactSuballocationMatch =
					knownIndexFormat &&
					(currentVertexOffset == expectedVertexDataOffset ||
					 effectiveVertexOffset ==
						static_cast<std::int64_t>(
							expectedVertexDataOffset)) &&
					(currentIndexOffset == expectedIndexDataOffset ||
					 effectiveIndexOffset == expectedIndexDataOffset);

				bool exactReticleMatch = false;
				if (automaticSTSReticleGeometryReady.load(
						std::memory_order_acquire)) {
					const auto expectedReticleVertexDataOffset =
						automaticSTSReticleVertexDataOffset.load(
							std::memory_order_relaxed);
					const auto expectedReticleIndexDataOffset =
						automaticSTSReticleIndexDataOffset.load(
							std::memory_order_relaxed);
					exactReticleMatch =
						reinterpret_cast<std::uintptr_t>(
							currentVertexBuffer.Get()) ==
							automaticSTSReticleVertexBuffer.load(
								std::memory_order_relaxed) &&
						reinterpret_cast<std::uintptr_t>(
							currentIndexBuffer.Get()) ==
							automaticSTSReticleIndexBuffer.load(
								std::memory_order_relaxed) &&
						IndexCount ==
							automaticSTSReticleIndexCount.load(
								std::memory_order_relaxed) &&
						currentStride ==
							automaticSTSReticleVertexStride.load(
								std::memory_order_relaxed) &&
						knownIndexFormat &&
						(currentVertexOffset ==
								expectedReticleVertexDataOffset ||
						 effectiveVertexOffset ==
								static_cast<std::int64_t>(
									expectedReticleVertexDataOffset)) &&
						(currentIndexOffset ==
								expectedReticleIndexDataOffset ||
						 effectiveIndexOffset ==
								expectedReticleIndexDataOffset);
				}
				if (exactReticleMatch) {
					automaticSTSReticleDrawsThisFrame.fetch_add(
						1U,
						std::memory_order_relaxed);
					automaticSTSLastReticleOrdinal.store(
						drawOrdinal,
						std::memory_order_relaxed);
				}

				bool exactHousingMatch = false;
				if (automaticSTSHousingGeometryReady.load(
						std::memory_order_acquire)) {
					const auto expectedHousingVertexDataOffset =
						automaticSTSHousingVertexDataOffset.load(
							std::memory_order_relaxed);
					const auto expectedHousingIndexDataOffset =
						automaticSTSHousingIndexDataOffset.load(
							std::memory_order_relaxed);
					exactHousingMatch =
						reinterpret_cast<std::uintptr_t>(
							currentVertexBuffer.Get()) ==
							automaticSTSHousingVertexBuffer.load(
								std::memory_order_relaxed) &&
						reinterpret_cast<std::uintptr_t>(
							currentIndexBuffer.Get()) ==
							automaticSTSHousingIndexBuffer.load(
								std::memory_order_relaxed) &&
						IndexCount ==
							automaticSTSHousingIndexCount.load(
								std::memory_order_relaxed) &&
						currentStride ==
							automaticSTSHousingVertexStride.load(
								std::memory_order_relaxed) &&
						knownIndexFormat &&
						(currentVertexOffset ==
								expectedHousingVertexDataOffset ||
						 effectiveVertexOffset ==
								static_cast<std::int64_t>(
									expectedHousingVertexDataOffset)) &&
						(currentIndexOffset ==
								expectedHousingIndexDataOffset ||
						 effectiveIndexOffset ==
								expectedHousingIndexDataOffset);
				}
				if (exactHousingMatch) {
					automaticSTSHousingDrawsThisFrame.fetch_add(
						1U,
						std::memory_order_relaxed);
					automaticSTSLastHousingOrdinal.store(
						drawOrdinal,
						std::memory_order_relaxed);
				}

				if (exactGeometryMatch && !exactSuballocationMatch) {
					// FO4 pools many shapes into the same large D3D buffers.
					// A buffer-pointer and index-count match is therefore only
					// a candidate until its byte suballocation also matches.
					// Keep diagnostics bounded on the render thread. A no-match
					// is deliberately safer than recoloring unrelated geometry.
					static std::atomic_uint32_t loggedSuballocationMismatches{ 0 };
					const auto diagnosticIndex =
						loggedSuballocationMismatches.fetch_add(
							1,
							std::memory_order_relaxed);
					if (diagnosticIndex < 8U) {
						logger::info(
							"Stage 4d ScopeFade buffer candidate rejected: "
							"vertexOffset={} effective={} expected={}, "
							"indexOffset={} effective={} expected={}, "
							"indexFormat={}, startIndex={}, baseVertex={}",
							currentVertexOffset,
							effectiveVertexOffset,
							expectedVertexDataOffset,
							currentIndexOffset,
							effectiveIndexOffset,
							expectedIndexDataOffset,
							static_cast<int>(currentIndexFormat),
							StartIndexLocation,
							BaseVertexLocation);
					}
				}

				if (exactGeometryMatch && exactSuballocationMatch) {
					automaticSTSScopeFadeDrawsThisFrame.fetch_add(
						1U,
						std::memory_order_relaxed);
					automaticSTSLastScopeFadeOrdinal.store(
						drawOrdinal,
						std::memory_order_relaxed);
					// Change only the pixel shader. The existing vertex shader,
					// transform constants, input assembly, rasterizer, depth
					// state, blend state, render targets, and draw order are the
					// contract under test. Preserve dynamic-linkage instances
					// as well as the shader pointer before returning.
					constexpr UINT kMaxClassInstances = 256;
					std::array<
						ID3D11ClassInstance*,
						kMaxClassInstances> pixelClassInstances{};
					UINT pixelClassInstanceCount = kMaxClassInstances;
					ComPtr<ID3D11PixelShader> originalPixelShader;
					pContext->PSGetShader(
						originalPixelShader.GetAddressOf(),
						pixelClassInstances.data(),
						&pixelClassInstanceCount);

					std::array<
						ID3D11ClassInstance*,
						kMaxClassInstances> geometryClassInstances{};
					UINT geometryClassInstanceCount = kMaxClassInstances;
					ComPtr<ID3D11GeometryShader> originalGeometryShader;
					pContext->GSGetShader(
						originalGeometryShader.GetAddressOf(),
						geometryClassInstances.data(),
						&geometryClassInstanceCount);

					pContext->GSSetShader(
						D3DInstance->
							m_pGeometryShader_STSGeometryFill.Get(),
						nullptr,
						0);
					pContext->PSSetShader(
						D3DInstance->
							m_pPixelShader_STSGeometryProbe.Get(),
						nullptr,
						0);
					oldFuncs.phookD3D11DrawIndexed(
						pContext,
						IndexCount,
						StartIndexLocation,
						BaseVertexLocation);
					const UINT restorePixelClassCount = std::min(
						pixelClassInstanceCount,
						kMaxClassInstances);
					pContext->PSSetShader(
						originalPixelShader.Get(),
						pixelClassInstances.data(),
						restorePixelClassCount);
					for (UINT i = 0;
						i < restorePixelClassCount;
						++i) {
						SAFE_RELEASE(pixelClassInstances[i]);
					}
					const UINT restoreGeometryClassCount = std::min(
						geometryClassInstanceCount,
						kMaxClassInstances);
					pContext->GSSetShader(
						originalGeometryShader.Get(),
						geometryClassInstances.data(),
						restoreGeometryClassCount);
					for (UINT i = 0;
						i < restoreGeometryClassCount;
						++i) {
						SAFE_RELEASE(geometryClassInstances[i]);
					}

					static std::once_flag loggedGeometryProbeMatch;
					std::call_once(loggedGeometryProbeMatch, [&] {
						logger::info(
							"Stage 4d exact ScopeFade draw matched: "
							"VB={:p}, IB={:p}, indices={}, stride={}, "
							"vertexOffset={}/{}, indexOffset={}/{}, "
							"indexFormat={}, startIndex={}, baseVertex={}; "
							"the geometry shader filled the annulus and the "
							"pixel shader was replaced",
							static_cast<void*>(
								currentVertexBuffer.Get()),
							static_cast<void*>(
								currentIndexBuffer.Get()),
								IndexCount,
								currentStride,
								currentVertexOffset,
								expectedVertexDataOffset,
							currentIndexOffset,
							expectedIndexDataOffset,
							static_cast<int>(currentIndexFormat),
							StartIndexLocation,
							BaseVertexLocation);
					});
					return;
				}
			}

			// STS owns its 3D reticle. Suppressing the inherited FTS
			// fingerprinted draw here would make the reticle disappear in
			// automatic mode.
			return oldFuncs.phookD3D11DrawIndexed(pContext, IndexCount, StartIndexLocation, BaseVertexLocation);
		}

		std::vector<BufferInfo> vertexInfo;
		BufferInfo indexInfo;
		if (!GetVertexBuffersInfo(pContext, vertexInfo) || !GetIndexBufferInfo(pContext, indexInfo)) {
			return oldFuncs.phookD3D11DrawIndexed(pContext, IndexCount, StartIndexLocation, BaseVertexLocation);
		}

		if (IsTargetDrawCall(vertexInfo[0], indexInfo, IndexCount)) {
			targetVSNumClassesInstance = 0;
			pContext->VSGetShader(targetVS.ReleaseAndGetAddressOf(), nullptr, &targetVSNumClassesInstance);
			pContext->PSGetShader(targetPS.ReleaseAndGetAddressOf(), nullptr, nullptr);
			pContext->OMGetDepthStencilState(targetDepthStencilState.ReleaseAndGetAddressOf(), nullptr);
			pContext->IAGetInputLayout(targetInputLayout.ReleaseAndGetAddressOf());

			pContext->IAGetIndexBuffer(targetIndexBuffer.ReleaseAndGetAddressOf(), &targetIndexBufferFormat, &targetIndexBufferOffset);

			pContext->IAGetVertexBuffers(0, 1, targetVertexBuffer.ReleaseAndGetAddressOf(), &targetVertexBufferStrides, &targetVertexBufferOffsets);
			SAFE_RELEASE(targetVertexConstBuffer);
			SAFE_RELEASE(targetVertexConstBuffer1p5);
			SAFE_RELEASE(targetVertexConstBuffer1);
			pContext->VSGetConstantBuffers(1, 1, &targetVertexConstBuffer);
			pContext->VSGetConstantBuffers(2, 1, &targetVertexConstBuffer1p5);
			pContext->VSGetConstantBuffers(12, 1, &targetVertexConstBuffer1);

			pContext->PSGetShaderResources(0, 1, DrawIndexedSRV.ReleaseAndGetAddressOf());

			if (!DrawIndexedSRV.Get() || !oldFuncs.phookD3D11DrawIndexed) {
				oldFuncs.phookD3D11DrawIndexed(pContext, IndexCount, StartIndexLocation, BaseVertexLocation);
				return;
			}

			ComPtr<ID3D11Resource> pResource;
			DrawIndexedSRV->GetResource(pResource.GetAddressOf());
			if (!pResource.Get()) {
				return oldFuncs.phookD3D11DrawIndexed(pContext, IndexCount, StartIndexLocation, BaseVertexLocation);
			}
			D3D11_SHADER_RESOURCE_VIEW_DESC tempSRVDesc;
			DrawIndexedSRV->GetDesc(&tempSRVDesc);

			// 获取资源的类型
			D3D11_RESOURCE_DIMENSION dimension;
			pResource->GetType(&dimension);

			// 检查是否是 2D 纹理
			if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
				// 使用 QueryInterface 获取 ID3D11Texture2D 接口的指针
				ComPtr<ID3D11Texture2D> pTexture2D;
				HRESULT hr = pResource.As(&pTexture2D);
				if (SUCCEEDED(hr)) {
					// 获取纹理描述
					D3D11_TEXTURE2D_DESC desc;
					pTexture2D->GetDesc(&desc);

					if (desc.Width == TARGET_TEXTURE_WIDTH && desc.Height == TARGET_TEXTURE_WIDTH && desc.ArraySize == 1 && desc.Format == TARGET_TEXTURE_FORMAT && tempSRVDesc.Format == TARGET_TEXTURE_FORMAT && tempSRVDesc.Texture2D.MipLevels == 1 && tempSRVDesc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D) {
						if (targetVertexConstBuffer && targetVertexConstBuffer1p5 && targetVertexConstBuffer1) {
							ID3D11Buffer* srcbuffers[] = { targetVertexConstBuffer, targetVertexConstBuffer1p5, targetVertexConstBuffer1 };
							ID3D11Buffer* dstBuffers[] = { targetVertexConstBufferOutPut, targetVertexConstBufferOutPut1p5, targetVertexConstBufferOutPut1 };
							CopyConstantBuffers(pContext, srcbuffers, dstBuffers, 3);
						}

						targetIndexCount = IndexCount;
						targetStartIndexLocation = StartIndexLocation;
						targetBaseVertexLocation = BaseVertexLocation;

						return oldFuncs.phookD3D11DrawIndexed(pContext, 0, 0, 0);
					}
				}
			}
			
		}

		return oldFuncs.phookD3D11DrawIndexed(pContext, IndexCount, StartIndexLocation, BaseVertexLocation);
	}

	void __stdcall D3D::DrawIndexedInstancedHook(
		ID3D11DeviceContext* pContext,
		UINT IndexCountPerInstance,
		UINT InstanceCount,
		UINT StartIndexLocation,
		INT BaseVertexLocation,
		UINT StartInstanceLocation)
	{
		if (!oldFuncs.phookD3D11DrawIndexedInstanced) {
			return;
		}

		const auto callOriginal = [&] {
			oldFuncs.phookD3D11DrawIndexedInstanced(
				pContext,
				IndexCountPerInstance,
				InstanceCount,
				StartIndexLocation,
				BaseVertexLocation,
				StartInstanceLocation);
		};
		if (bSelfDraw) {
			callOriginal();
			return;
		}

		const auto* activeProfile =
			ScopeData::ScopeDataHandler::GetSingleton()->
				GetCurrentFTSData();
		const auto& verification = MagnaScope::GetSettings();
		if (!activeProfile || !activeProfile->autoProfile ||
			!verification.AllowsGeometryProbe() ||
			!automaticSTSGeometryReady.load(std::memory_order_acquire)) {
			callOriginal();
			return;
		}

		const auto drawOrdinal =
			automaticSTSDrawOrdinalThisFrame.fetch_add(
				1U,
				std::memory_order_relaxed) +
			1U;
		ComPtr<ID3D11Buffer> currentVertexBuffer;
		ComPtr<ID3D11Buffer> currentIndexBuffer;
		UINT currentStride = 0;
		UINT currentVertexOffset = 0;
		DXGI_FORMAT currentIndexFormat = DXGI_FORMAT_UNKNOWN;
		UINT currentIndexOffset = 0;
		pContext->IAGetVertexBuffers(
			0,
			1,
			currentVertexBuffer.GetAddressOf(),
			&currentStride,
			&currentVertexOffset);
		pContext->IAGetIndexBuffer(
			currentIndexBuffer.GetAddressOf(),
			&currentIndexFormat,
			&currentIndexOffset);

		const bool knownIndexFormat =
			currentIndexFormat == DXGI_FORMAT_R16_UINT ||
			currentIndexFormat == DXGI_FORMAT_R32_UINT;
		const std::uint32_t indexElementSize =
			currentIndexFormat == DXGI_FORMAT_R32_UINT ? 4U : 2U;
		const std::int64_t effectiveVertexOffset =
			static_cast<std::int64_t>(currentVertexOffset) +
			static_cast<std::int64_t>(BaseVertexLocation) *
				static_cast<std::int64_t>(currentStride);
		const std::uint64_t effectiveIndexOffset =
			static_cast<std::uint64_t>(currentIndexOffset) +
			static_cast<std::uint64_t>(StartIndexLocation) *
				indexElementSize;

		const auto matchesPublished = [&](
			const std::atomic<std::uintptr_t>& publishedVertexBuffer,
			const std::atomic<std::uintptr_t>& publishedIndexBuffer,
			const std::atomic_uint32_t& publishedIndexCount,
			const std::atomic_uint32_t& publishedVertexStride,
			const std::atomic_uint32_t& publishedVertexDataOffset,
			const std::atomic_uint32_t& publishedIndexDataOffset,
			const std::atomic_bool& publishedReady) {
			if (!publishedReady.load(std::memory_order_acquire) ||
				!knownIndexFormat) {
				return false;
			}
			const auto expectedVertexDataOffset =
				publishedVertexDataOffset.load(
					std::memory_order_relaxed);
			const auto expectedIndexDataOffset =
				publishedIndexDataOffset.load(
					std::memory_order_relaxed);
			return reinterpret_cast<std::uintptr_t>(
					   currentVertexBuffer.Get()) ==
					   publishedVertexBuffer.load(
						   std::memory_order_relaxed) &&
				reinterpret_cast<std::uintptr_t>(
					currentIndexBuffer.Get()) ==
					publishedIndexBuffer.load(
						std::memory_order_relaxed) &&
				IndexCountPerInstance ==
					publishedIndexCount.load(
						std::memory_order_relaxed) &&
				currentStride ==
					publishedVertexStride.load(
						std::memory_order_relaxed) &&
				(currentVertexOffset == expectedVertexDataOffset ||
				 effectiveVertexOffset ==
					static_cast<std::int64_t>(
						expectedVertexDataOffset)) &&
				(currentIndexOffset == expectedIndexDataOffset ||
				 effectiveIndexOffset == expectedIndexDataOffset);
		};

		if (matchesPublished(
				automaticSTSVertexBuffer,
				automaticSTSIndexBuffer,
				automaticSTSIndexCount,
				automaticSTSVertexStride,
				automaticSTSVertexDataOffset,
				automaticSTSIndexDataOffset,
				automaticSTSGeometryReady)) {
			automaticSTSScopeFadeInstancedDrawsThisFrame.fetch_add(
				1U,
				std::memory_order_relaxed);
			automaticSTSLastScopeFadeOrdinal.store(
				drawOrdinal,
				std::memory_order_relaxed);
		}
		if (matchesPublished(
				automaticSTSReticleVertexBuffer,
				automaticSTSReticleIndexBuffer,
				automaticSTSReticleIndexCount,
				automaticSTSReticleVertexStride,
				automaticSTSReticleVertexDataOffset,
				automaticSTSReticleIndexDataOffset,
				automaticSTSReticleGeometryReady)) {
			automaticSTSReticleInstancedDrawsThisFrame.fetch_add(
				1U,
				std::memory_order_relaxed);
			automaticSTSLastReticleOrdinal.store(
				drawOrdinal,
				std::memory_order_relaxed);
		}
		if (matchesPublished(
				automaticSTSHousingVertexBuffer,
				automaticSTSHousingIndexBuffer,
				automaticSTSHousingIndexCount,
				automaticSTSHousingVertexStride,
				automaticSTSHousingVertexDataOffset,
				automaticSTSHousingIndexDataOffset,
				automaticSTSHousingGeometryReady)) {
			automaticSTSHousingInstancedDrawsThisFrame.fetch_add(
				1U,
				std::memory_order_relaxed);
			automaticSTSLastHousingOrdinal.store(
				drawOrdinal,
				std::memory_order_relaxed);
		}

		callOriginal();
	}

	bool D3D::CaptureVerificationSource(
		ID3D11Texture2D* sourceTexture,
		const char* anchorName,
		ID3D11Device* captureDevice,
		ID3D11DeviceContext* captureContext)
	{
		auto* device = captureDevice ? captureDevice : g_Device.Get();
		auto* context = captureContext ? captureContext : g_Context.Get();
		if (!sourceTexture || !anchorName || !device || !context) {
			return false;
		}

		const bool isTaaAnchor = std::strcmp(anchorName, "TAA") == 0;
		static std::atomic_bool verifiedFrameworkCapture = false;
		static std::atomic_bool verifiedTaaCapture = false;
		auto& verifiedCapture =
			isTaaAnchor ? verifiedTaaCapture : verifiedFrameworkCapture;
		if (verifiedCapture.load(std::memory_order_acquire)) {
			return true;
		}

		D3D11_TEXTURE2D_DESC sourceDesc{};
		sourceTexture->GetDesc(&sourceDesc);

		// CopyResource requires the complete resource shape to match. Preserve
		// mip, array, format, and sample metadata from the source while
		// removing every bind and CPU-access flag from the private copy.
		D3D11_TEXTURE2D_DESC captureDesc = sourceDesc;
		captureDesc.Usage = D3D11_USAGE_DEFAULT;
		captureDesc.BindFlags = 0;
		captureDesc.CPUAccessFlags = 0;
		captureDesc.MiscFlags = 0;

		// Stage 3 uses a local resource rather than D3D::mCurRTTexture. The TAA
		// callback and Menu Framework callback have been observed on different
		// threads, so sharing the later Stage 4 render resource here would race.
		ComPtr<ID3D11Texture2D> captureTexture;
		const HRESULT createCaptureResult = device->CreateTexture2D(
			&captureDesc,
			nullptr,
			captureTexture.GetAddressOf());
		if (FAILED(createCaptureResult) || !captureTexture.Get()) {
			logger::error(
				"Stage 3 {} capture texture creation failed with HRESULT 0x{:08X}",
				anchorName,
				static_cast<std::uint32_t>(createCaptureResult));
			return false;
		}

		context->CopyResource(captureTexture.Get(), sourceTexture);

		// A D3D11 staging texture cannot be multisampled. The copy itself still
		// proves the source is compatible, but its bytes require a later resolve.
		if (sourceDesc.SampleDesc.Count != 1) {
			logger::warn(
				"Verification stage 3 {} source copy completed: "
				"{}x{}, format={}, samples={}; readback requires a resolve",
				anchorName,
				sourceDesc.Width,
				sourceDesc.Height,
				static_cast<int>(sourceDesc.Format),
				sourceDesc.SampleDesc.Count);
			verifiedCapture.store(true, std::memory_order_release);
			return true;
		}

		// One synchronous readback proves the GPU copy contains source bytes.
		// It runs once per anchor and never binds or draws to a game target.
		D3D11_TEXTURE2D_DESC stagingDesc = captureDesc;
		stagingDesc.Usage = D3D11_USAGE_STAGING;
		stagingDesc.BindFlags = 0;
		stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		ComPtr<ID3D11Texture2D> stagingTexture;
		const HRESULT createStagingResult = device->CreateTexture2D(
			&stagingDesc,
			nullptr,
			stagingTexture.GetAddressOf());
		if (FAILED(createStagingResult) || !stagingTexture.Get()) {
			logger::error(
				"Stage 3 {} staging texture creation failed with HRESULT 0x{:08X}",
				anchorName,
				static_cast<std::uint32_t>(createStagingResult));
			return false;
		}

		context->CopyResource(stagingTexture.Get(), captureTexture.Get());
		D3D11_MAPPED_SUBRESOURCE mapped{};
		const HRESULT mapResult = context->Map(
			stagingTexture.Get(),
			0,
			D3D11_MAP_READ,
			0,
			&mapped);
		if (FAILED(mapResult) || !mapped.pData) {
			logger::error(
				"Stage 3 {} source readback failed with HRESULT 0x{:08X}",
				anchorName,
				static_cast<std::uint32_t>(mapResult));
			return false;
		}

		const auto* centerRow =
			static_cast<const std::uint8_t*>(mapped.pData) +
			(sourceDesc.Height / 2) * mapped.RowPitch;
		const std::size_t bytesToHash =
			std::min<std::size_t>(mapped.RowPitch, 256);
		const auto* centerSample =
			centerRow + (mapped.RowPitch - bytesToHash) / 2;
		std::uint64_t centerHash = 1469598103934665603ULL;
		std::size_t nonZeroBytes = 0;
		for (std::size_t index = 0; index < bytesToHash; ++index) {
			const auto value = centerSample[index];
			centerHash ^= value;
			centerHash *= 1099511628211ULL;
			nonZeroBytes += value != 0;
		}
		context->Unmap(stagingTexture.Get(), 0);

		logger::info(
			"Verification stage 3 {} source capture completed: "
			"{}x{}, format={}, samples={}, centerHash={:016X}, nonzeroBytes={}/{}; "
			"no shaders, RTVs, or draws",
			anchorName,
			sourceDesc.Width,
			sourceDesc.Height,
			static_cast<int>(sourceDesc.Format),
			sourceDesc.SampleDesc.Count,
			centerHash,
			nonZeroBytes,
			bytesToHash);
		verifiedCapture.store(true, std::memory_order_release);
		return true;
	}

	bool D3D::CaptureVerificationTAASource()
	{
		const auto* rendererData = RE::BSGraphics::GetRendererData();
		if (!rendererData || !rendererData->context) {
			return false;
		}

		// This runs inside Fallout 4's TAA Render callback. Query only the
		// render-thread D3D state that the original FTS contract relies on.
		// No camera, weapon, scene graph, or profile object is touched here.
		auto* context =
			static_cast<ID3D11DeviceContext*>(static_cast<void*>(rendererData->context));
		ID3D11RenderTargetView* targets[2]{};
		ID3D11DepthStencilView* depth = nullptr;
		context->OMGetRenderTargets(2, targets, &depth);

		auto releaseTargets = [&] {
			SAFE_RELEASE(targets[0]);
			SAFE_RELEASE(targets[1]);
			SAFE_RELEASE(depth);
		};

		auto* sourceView = targets[1] ? targets[1] : targets[0];
		ID3D11Resource* sourceResource = nullptr;
		if (sourceView) {
			sourceView->GetResource(&sourceResource);
		}

		ComPtr<ID3D11Texture2D> sourceTexture;
		if (sourceResource) {
			sourceResource->QueryInterface(
				__uuidof(ID3D11Texture2D),
				reinterpret_cast<void**>(sourceTexture.GetAddressOf()));
			sourceResource->Release();
		}
		releaseTargets();
		if (!sourceTexture.Get()) {
			return false;
		}

		ComPtr<ID3D11Device> device;
		ComPtr<ID3D11DeviceContext> immediateContext;
		sourceTexture->GetDevice(device.GetAddressOf());
		if (device.Get()) {
			device->GetImmediateContext(immediateContext.GetAddressOf());
		}
		return CaptureVerificationSource(
			sourceTexture.Get(),
			"TAA",
			device.Get(),
			immediateContext.Get());
	}

	void D3D::Render()
	{
		lastRenderProducedComposite = false;
		const auto& verification = MagnaScope::GetSettings();
		if (!verification.AllowsRenderer()) {
			return;
		}

		if (!g_Swapchain.Get() || !g_Device.Get() || !g_Context.Get())
		{
			const auto* rendererData = RE::BSGraphics::GetRendererData();
			const auto* renderWindow = RE::BSGraphics::GetCurrentRendererWindow();
			if (!rendererData || !renderWindow || !renderWindow->swapChain ||
				!rendererData->device || !rendererData->context) {
				return;
			}
			g_Swapchain = static_cast<IDXGISwapChain*>(static_cast<void*>(renderWindow->swapChain));
			g_Device = static_cast<ID3D11Device*>(static_cast<void*>(rendererData->device));
			g_Context = static_cast<ID3D11DeviceContext*>(static_cast<void*>(rendererData->context));
		}

		if (!sdh) {
			sdh = ScopeData::ScopeDataHandler::GetSingleton();
		}

		if (bQueryRender && sdh) {
			if (!verification.AllowsComposite() && renderedAtTAAThisFrame) {
				if (!isEnableRender || !pcam || !player) {
					return;
				}
				const auto* currData = sdh->GetCurrentFTSData();
				if (!currData || !currData->containAlladditionalKeywords) {
					return;
				}

				// The original FTS callback runs after the game's TAA Render
				// method. At that point its output remains bound to the output
				// merger. Read those references without changing any state,
				// choose the same RT1-preferred source contract as upstream,
				// and release every reference before returning.
				ID3D11RenderTargetView* taaTargets[2]{};
				ID3D11DepthStencilView* taaDepth = nullptr;
				g_Context->OMGetRenderTargets(2, taaTargets, &taaDepth);
				auto releaseTaaTargets = [&] {
					SAFE_RELEASE(taaTargets[0]);
					SAFE_RELEASE(taaTargets[1]);
					SAFE_RELEASE(taaDepth);
				};

				auto* sourceView = taaTargets[1] ? taaTargets[1] : taaTargets[0];
				ID3D11Resource* sourceResource = nullptr;
				if (sourceView) {
					sourceView->GetResource(&sourceResource);
				}
				ComPtr<ID3D11Texture2D> taaSource;
				if (sourceResource) {
					sourceResource->QueryInterface(
						__uuidof(ID3D11Texture2D),
						reinterpret_cast<void**>(taaSource.GetAddressOf()));
					sourceResource->Release();
				}

				const bool captured =
					CaptureVerificationSource(taaSource.Get(), "TAA");
				releaseTaaTargets();
				if (!captured) {
					logger::warn(
						"Stage 3b TAA callback found no compatible bound color target; "
						"framework Present capture will be used");
					renderedAtTAAThisFrame = false;
				}
				return;
			}

			auto* activeSwapChain =
				lastPresentedSwapChain.load(std::memory_order_acquire);
			if (!activeSwapChain) {
				activeSwapChain = g_Swapchain.Get();
			}
			if (!activeSwapChain) {
				return;
			}

			if (activeSwapChain != backBufferOwnerSwapChain) {
				// ENB/upscaler proxies can present through a different chain
				// object than BSGraphics exposes. The Present argument is the
				// authoritative displayed chain; invalidate every object tied
				// to the previous chain when that identity changes.
				m_pRenderTargetView.Reset();
				mBackBuffer.Reset();
				mBackBufferIndex = UINT_MAX;
				bHasGetBackBuffer = false;
				backBufferOwnerSwapChain = activeSwapChain;
				getBufferReferenceContractProbed = false;
				getBufferAddsReference = true;
				logger::info(
					"Scope swap-chain owner: renderer={:p}, presented={:p}",
					static_cast<void*>(g_Swapchain.Get()),
					static_cast<void*>(activeSwapChain));
			}


			DXGI_SWAP_CHAIN_DESC swapChainDesc{};
			const bool hasSwapChainDesc =
				SUCCEEDED(activeSwapChain->GetDesc(&swapChainDesc));
			const bool isFlipModel =
				hasSwapChainDesc &&
				(swapChainDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL ||
				 swapChainDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD);
			constexpr UINT currentBackBufferIndex = 0;

			if (!verification.AllowsComposite()) {
				// Stage 3a has no ResizeBuffers hook. Do not retain a back
				// buffer reference between framework callbacks because doing
				// so can prevent a legitimate ResizeBuffers call from
				// succeeding during an alt-tab or resolution change.
				mBackBuffer.Reset();
				bHasGetBackBuffer = false;
			}

			if (!bHasGetBackBuffer || isFlipModel)
			{
				// Stay on the IDXGISwapChain contract. AAA Frame Generation's
				// DXGISwapChainProxy implements only that base interface: its
				// QueryInterface forwards success for IDXGISwapChain3 but then
				// substitutes the base-only proxy pointer. Calling
				// GetCurrentBackBufferIndex through that pointer jumps through
				// a nonexistent vtable slot and crashes. The proxy's verified
				// GetBuffer implementation ignores the index and returns its
				// actual D3D11 presentation texture, matching F4SE Menu
				// Framework's proven GetBuffer(0) handling.
				//
				// Real flip-model chains rotate which texture GetBuffer(0)
				// exposes, so refresh the texture and RTV each presented frame.
				// Fallout 4 OG's normal discard-model chain keeps the cached
				// fast path.
				m_pRenderTargetView.Reset();
				mBackBuffer.Reset();
				ID3D11Texture2D* acquiredBackBuffer =
					AcquireBackBuffer(activeSwapChain, currentBackBufferIndex);
				if (!acquiredBackBuffer) {
					logger::error(
						"Swap chain did not return back buffer {}",
						currentBackBufferIndex);
					return;
				}

				// Take our own explicit reference regardless of the proxy's
				// GetBuffer behavior, then balance only the reference actually
				// returned by GetBuffer.
				mBackBuffer = acquiredBackBuffer;
				ReleaseAcquiredBackBuffer(acquiredBackBuffer);
				if (verification.AllowsComposite()) {
					const HRESULT createRtvResult = g_Device->CreateRenderTargetView(
						mBackBuffer.Get(),
						nullptr,
						m_pRenderTargetView.ReleaseAndGetAddressOf());
					if (FAILED(createRtvResult)) {
						logger::error(
							"Scope back-buffer RTV creation failed with HRESULT 0x{:08X}",
							static_cast<std::uint32_t>(createRtvResult));
						return;
					}
				}

				static std::once_flag loggedSwapChainContract;
				std::call_once(loggedSwapChainContract, [&] {
					if (!hasSwapChainDesc) {
						swapChainDesc = {};
					}
					logger::info(
						"Scope final-frame target: swapEffect={}, buffers={}, GetBufferIndex={}{}",
						static_cast<int>(swapChainDesc.SwapEffect),
						swapChainDesc.BufferCount,
						currentBackBufferIndex,
						isFlipModel ? " (refreshed each frame)" : "");
				});

				mBackBufferIndex = currentBackBufferIndex;
				bHasGetBackBuffer = true;
			}

			if (isEnableRender && pcam && player)
			{
				auto currData = sdh->GetCurrentFTSData();

				if (!currData || !currData->containAlladditionalKeywords)
					return;

				if (!verification.AllowsComposite()) {
					// Stage 3a is intentionally smaller than the original FTS
					// render contract. It validates only the displayed
					// back-buffer identity and a GPU copy issued from F4SE Menu
					// Framework's before-render callback. It creates no RTV or
					// SRV, initializes no shaders or constant buffers, and
					// changes no pipeline state.
					CaptureVerificationSource(mBackBuffer.Get(), "FrameworkPresent");
					// Stage 3a deliberately owns no resize hook, so release the
					// displayed back buffer before returning to the framework.
					mBackBuffer.Reset();
					bHasGetBackBuffer = false;
					return;
				}

				if (bIsFirst)
				{
					OnResize();
					if (!InitEffect() || !InitLegacyEffect() || !InitResource()) {
						logger::error("Scope GPU resource initialization failed; rendering disabled");
						isEnableRender = false;
						return;
					}
				}

				ScopedContextState contextState(g_Context.Get());
				rtTexture2D.Reset();

				SAFE_RELEASE(tempRt[0]);
				SAFE_RELEASE(tempRt[1]);
				SAFE_RELEASE(tempSV);
				g_Context->OMGetRenderTargets(2, tempRt, &tempSV);
				ScopedRenderTargetReferences renderTargetReferences;
				if (!renderedAtTAAThisFrame) {
					// Present anchor: the game's mid-frame render targets have
					// already been consumed by the time Present runs, so
					// compositing into whatever is still bound never reaches
					// the screen. The swap chain back buffer is the only
					// surface that still gets displayed; use it as both the
					// source and (below, in the reticle passes) the target.
					rtTexture2D = mBackBuffer;
				} else {
					ID3D11Resource* rtResource = nullptr;
					if (upscalerMod) {
						if (tempRt[0] != nullptr) {
							tempRt[0]->GetResource(&rtResource);  // 获取纹理接口
						}
					} else {
						auto* sourceView = tempRt[1] ? tempRt[1] : tempRt[0];
						if (sourceView != nullptr) {
							sourceView->GetResource(&rtResource);  // 获取纹理接口
						}
					}
					if (rtResource != nullptr) {
						rtResource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)rtTexture2D.GetAddressOf());
						rtResource->Release();  // 释放原始资源引用
					}
				}
				if (!rtTexture2D.Get()) {
					logger::warn("No compatible render target was available for the scope pass");
					return;
				}
				D3D11_TEXTURE2D_DESC originalDesc;
				rtTexture2D->GetDesc(&originalDesc);  // 获取原纹理参数
				rtTextureDesc = originalDesc;
				rtTextureDesc.MipLevels = 1;
				rtTextureDesc.ArraySize = 1;
				rtTextureDesc.Usage = D3D11_USAGE_DEFAULT;
				rtTextureDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
				rtTextureDesc.CPUAccessFlags = 0;
				rtTextureDesc.MiscFlags = 0;

				bool recreateCapture =
					!mCurRTTexture.Get() || !mShaderResourceView.Get();
				if (!recreateCapture) {
					D3D11_TEXTURE2D_DESC captureDesc{};
					mCurRTTexture->GetDesc(&captureDesc);
					recreateCapture =
						captureDesc.Width != rtTextureDesc.Width ||
						captureDesc.Height != rtTextureDesc.Height ||
						captureDesc.Format != rtTextureDesc.Format ||
						captureDesc.SampleDesc.Count != rtTextureDesc.SampleDesc.Count ||
						captureDesc.SampleDesc.Quality != rtTextureDesc.SampleDesc.Quality;
				}
				if (recreateCapture) {
					mShaderResourceView.Reset();
					mCurRTTexture.Reset();
					HR(g_Device->CreateTexture2D(
						&rtTextureDesc,
						nullptr,
						mCurRTTexture.GetAddressOf()));
					HR(g_Device->CreateShaderResourceView(
						mCurRTTexture.Get(),
						nullptr,
						mShaderResourceView.GetAddressOf()));
				}
				

				bIsFirst = false;

				g_Context->CopyResource(mCurRTTexture.Get(), rtTexture2D.Get());
				UpdateScene(currData);

				// One-shot diagnostics on the 30th scoped frame (a steady-state
				// frame, after any aim transition). Reports the compositing
				// configuration, probes the captured source pixels before the
				// draw, and probes the composite target after the draw. If the
				// two probes match, the pass changed no pixels and the problem
				// is in the draw itself, not in what reaches the screen later.
				static const ScopeData::FTSData* diagnosedProfile = nullptr;
				static int diagCountdown = 30;
				if (diagnosedProfile != currData) {
					diagnosedProfile = currData;
					diagCountdown = 30;
				}
				const bool diagFrame = diagCountdown > 0 && --diagCountdown == 0;
				struct ProbeResult
				{
					bool valid = false;
					std::array<std::uint64_t, 3> hashes{};
				};
				const auto probeLens = [&](ID3D11Texture2D* texture, const char* label) {
					ProbeResult result{};
					D3D11_TEXTURE2D_DESC stagingDesc = rtTextureDesc;
					stagingDesc.Usage = D3D11_USAGE_STAGING;
					stagingDesc.BindFlags = 0;
					stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
					stagingDesc.MiscFlags = 0;
					ComPtr<ID3D11Texture2D> stagingTexture;
					if (SUCCEEDED(g_Device->CreateTexture2D(&stagingDesc, nullptr, stagingTexture.GetAddressOf()))) {
						g_Context->CopyResource(stagingTexture.Get(), texture);
						D3D11_MAPPED_SUBRESOURCE mapped{};
						if (SUCCEEDED(g_Context->Map(stagingTexture.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
							// Probe three blocks inside the actual lens. The
							// optical center maps to itself under magnification,
							// so a center-only probe cannot prove a zoom pass.
							// The two off-center blocks must change at zoom > 1.
							const auto* base = static_cast<const std::uint8_t*>(mapped.pData);
							const bool validX =
								scopeData.FTS_ScreenPos.x >= 0.0F &&
								scopeData.FTS_ScreenPos.x < stagingDesc.Width;
							const bool validY =
								scopeData.FTS_ScreenPos.y >= 0.0F &&
								scopeData.FTS_ScreenPos.y < stagingDesc.Height;
							const int lensX = validX ?
								static_cast<int>(scopeData.FTS_ScreenPos.x) :
								static_cast<int>(stagingDesc.Width / 2);
							const int lensY = validY ?
								static_cast<int>(scopeData.FTS_ScreenPos.y) :
								static_cast<int>(stagingDesc.Height / 2);
							const int radius = std::max(
								16,
								static_cast<int>(
									scopeData.ScopeEffect_Size.x *
									(stagingDesc.Height / 1080.0F) * 0.5F));
							const std::array<std::pair<int, int>, 3> points{ {
								{ lensX, lensY },
								{ lensX + radius / 2, lensY },
								{ lensX, lensY - radius / 2 }
							} };
							std::array<std::array<std::uint64_t, 3>, 3> sums{};
							result.hashes.fill(1469598103934665603ULL);
							for (std::size_t point = 0; point < points.size(); ++point) {
								const int startX = std::clamp(
									points[point].first - 8,
									0,
									static_cast<int>(stagingDesc.Width) - 16);
								const int startY = std::clamp(
									points[point].second - 8,
									0,
									static_cast<int>(stagingDesc.Height) - 16);
								for (int y = 0; y < 16; ++y) {
									const auto* row =
										base + (startY + y) * mapped.RowPitch + startX * 4;
									for (int x = 0; x < 16; ++x) {
										for (int channel = 0; channel < 4; ++channel) {
											const auto value = row[x * 4 + channel];
											result.hashes[point] ^= value;
											result.hashes[point] *= 1099511628211ULL;
											if (channel < 3) {
												sums[point][channel] += value;
											}
										}
									}
								}
							}
							g_Context->Unmap(stagingTexture.Get(), 0);
							result.valid = true;
							logger::info(
								"Scope {} probe at ({}, {}): center={} {} {}, right={} {} {}, upper={} {} {}, hashes={:016X}/{:016X}/{:016X}",
								label,
								lensX,
								lensY,
								sums[0][0] / 256,
								sums[0][1] / 256,
								sums[0][2] / 256,
								sums[1][0] / 256,
								sums[1][1] / 256,
								sums[1][2] / 256,
								sums[2][0] / 256,
								sums[2][1] / 256,
								sums[2][2] / 256,
								result.hashes[0],
								result.hashes[1],
								result.hashes[2]);
						} else {
							logger::warn("Scope {} probe could not map the staging texture", label);
						}
					}
					return result;
				};
				ProbeResult sourceProbe{};
				if (diagFrame) {
					logger::info(
						"Scope pass: {}x{} format {} ({} anchor, legacy={}, zoom={:.2f}, merge={}, circle {:.0f}x{:.0f} at ({:.3f}, {:.3f}))",
						originalDesc.Width,
						originalDesc.Height,
						static_cast<int>(originalDesc.Format),
						renderedAtTAAThisFrame ? "TAA" : "Present",
						bLegacyMode,
						scopeData.ScopeEffect_Zoom,
						scopeData.EnableMerge,
						scopeData.ScopeEffect_Size.x,
						scopeData.ScopeEffect_Size.y,
						scopeData.FTS_ScreenPos.x,
						scopeData.FTS_ScreenPos.y);
					sourceProbe = probeLens(mCurRTTexture.Get(), "source");
				}

				if (bLegacyMode) {
					D3DInstance->RenderToReticleTexture();
					lastRenderProducedComposite =
						!bIsFirst && isEnableRender;
				} else {
					D3DInstance->ScreenTextureMod();
					D3DInstance->RenderToReticleTextureNew(targetIndexCount, targetStartIndexLocation, targetBaseVertexLocation);
					lastRenderProducedComposite =
						targetVS.Get() &&
						targetVertexConstBufferOutPut &&
						targetIndexCount > 0;
				}
				renderPassHandledThisFrame = true;

				if (diagFrame) {
					// rtTexture2D is the composite destination at both anchors
					// (the back buffer at Present, the bound target at TAA).
					const auto targetProbe = probeLens(rtTexture2D.Get(), "target-after-draw");
					if (sourceProbe.valid && targetProbe.valid) {
						logger::info(
							"Scope draw changed sampled blocks: center={}, right={}, upper={}",
							sourceProbe.hashes[0] != targetProbe.hashes[0],
							sourceProbe.hashes[1] != targetProbe.hashes[1],
							sourceProbe.hashes[2] != targetProbe.hashes[2]);
					}
				}
				
				g_Context->OMSetRenderTargets(2, tempRt, tempSV);
			}
		}
	}

	void D3D::RenderFromFramework()
	{
		if (!frameworkRenderAnchor) {
			return;
		}

		static std::once_flag loggedFrameworkAnchor;
		std::call_once(loggedFrameworkAnchor, [] {
			logger::info("F4SE Menu Framework before-render anchor is dispatching");
		});

		const auto& verification = MagnaScope::GetSettings();
		if (verification.AllowsRenderer() && !verification.AllowsComposite()) {
			// Stage 3 owns no Present hook, so this verified framework event is
			// its frame boundary. A successful TAA capture arrives earlier in
			// the same frame and wins. Otherwise the already passed framework
			// Present source remains the fallback.
			lastRenderProducedComposite = false;
			const bool taaCaptured =
				taaVerificationCapturedSinceFramework.exchange(
					false,
					std::memory_order_acq_rel);
			if (isEnableRender.load(std::memory_order_acquire) && !taaCaptured) {
				Render();
			}
			renderedAtTAAThisFrame = false;
			compositedThisFrame = false;
			return;
		}

		// Stage 4 may prefer an earlier TAA result and keeps the shared guard
		// until MagnaScope's own Present callback ends the frame.
		if (isEnableRender && !renderPassHandledThisFrame) {
			Render();
			compositedThisFrame = lastRenderProducedComposite;
		}
	}

	void D3D::PublishLensProjection(
		float centerX,
		float centerY,
		float aimCenterX,
		float aimCenterY,
		float radiusX,
		float radiusY,
		bool automaticSTS,
		float activationProgress)
	{
		// Publish the readiness flag last. The render thread acquires it before
		// consuming the relaxed coordinate fields, which prevents it from
		// observing a new ready sample paired with an older center or radius.
		projectedTrackingReady.store(false, std::memory_order_release);
		projectedLensX.store(centerX, std::memory_order_relaxed);
		projectedLensY.store(centerY, std::memory_order_relaxed);
		projectedAimX.store(aimCenterX, std::memory_order_relaxed);
		projectedAimY.store(aimCenterY, std::memory_order_relaxed);
		projectedLensRadiusX.store(radiusX, std::memory_order_relaxed);
		projectedLensRadiusY.store(radiusY, std::memory_order_relaxed);
		projectedActivationProgress.store(
			std::clamp(activationProgress, 0.0F, 1.0F),
			std::memory_order_relaxed);
		projectedSourceWidth.store(
			static_cast<float>(windowWidth),
			std::memory_order_relaxed);
		projectedSourceHeight.store(
			static_cast<float>(windowHeight),
			std::memory_order_relaxed);
		projectedAutomaticSTS.store(automaticSTS, std::memory_order_relaxed);
		projectedTrackingReady.store(
			activationProgress > 0.0F,
			std::memory_order_release);
	}

	void D3D::InvalidateLensProjection()
	{
		// Invalidating readiness is sufficient to make every visual consumer
		// fail closed. Coordinates remain available for diagnostics but cannot
		// accidentally draw while the player is at the hip or changing optics.
		projectedTrackingReady.store(false, std::memory_order_release);
		projectedAutomaticSTS.store(false, std::memory_order_relaxed);
		projectedActivationProgress.store(0.0F, std::memory_order_relaxed);
	}

	D3D::LensProjectionSnapshot D3D::GetLensProjectionSnapshot() const
	{
		LensProjectionSnapshot result{};
		result.trackingReady =
			projectedTrackingReady.load(std::memory_order_acquire);
		result.centerX = projectedLensX.load(std::memory_order_relaxed);
		result.centerY = projectedLensY.load(std::memory_order_relaxed);
		result.aimCenterX = projectedAimX.load(std::memory_order_relaxed);
		result.aimCenterY = projectedAimY.load(std::memory_order_relaxed);
		result.radiusX =
			projectedLensRadiusX.load(std::memory_order_relaxed);
		result.radiusY =
			projectedLensRadiusY.load(std::memory_order_relaxed);
		result.activationProgress =
			projectedActivationProgress.load(std::memory_order_relaxed);
		result.sourceWidth =
			projectedSourceWidth.load(std::memory_order_relaxed);
		result.sourceHeight =
			projectedSourceHeight.load(std::memory_order_relaxed);
		result.renderEnabled = isEnableRender.load(std::memory_order_acquire);
		result.automaticSTS =
			projectedAutomaticSTS.load(std::memory_order_relaxed);
		return result;
	}

	void D3D::PublishAutomaticSTSGeometry(
		RE::NiAVObject* renderSurface,
		RE::NiAVObject* reticleSurface,
		RE::NiAVObject* aimingHousingSurface)
	{
		// BSGeometry::rendererData is documented by the vendored CommonLibF4
		// layout as a BSGraphics::TriShape allocation. Read it only on the game
		// thread while the selected scene objects are alive. The render thread
		// receives opaque D3D identities and never follows a scene pointer.
		const auto publishIdentity = [](
			RE::NiAVObject* object,
			std::atomic<std::uintptr_t>& publishedVertexBuffer,
			std::atomic<std::uintptr_t>& publishedIndexBuffer,
			std::atomic_uint32_t& publishedIndexCount,
			std::atomic_uint32_t& publishedVertexStride,
			std::atomic_uint32_t& publishedVertexDataOffset,
			std::atomic_uint32_t& publishedIndexDataOffset,
			std::atomic_bool& publishedReady,
			const char* label) {
			auto* triShape = object ? object->IsTriShape() : nullptr;
			auto* rendererShape =
				triShape && triShape->rendererData ?
					static_cast<RE::BSGraphics::TriShape*>(
						triShape->rendererData) :
					nullptr;
			auto* vertexBuffer =
				rendererShape && rendererShape->vertexBuffer ?
					reinterpret_cast<ID3D11Buffer*>(
						rendererShape->vertexBuffer->buffer) :
					nullptr;
			auto* indexBuffer =
				rendererShape && rendererShape->indexBuffer ?
					reinterpret_cast<ID3D11Buffer*>(
						rendererShape->indexBuffer->buffer) :
					nullptr;
			const std::uint32_t indexCount =
				triShape ? triShape->numTriangles * 3U : 0U;
			const std::uint32_t vertexStride =
				triShape ? triShape->vertexDesc.GetSize() : 0U;
			const std::uint32_t vertexDataOffset =
				rendererShape && rendererShape->vertexBuffer ?
					rendererShape->vertexBuffer->dataOffset :
					0U;
			const std::uint32_t indexDataOffset =
				rendererShape && rendererShape->indexBuffer ?
					rendererShape->indexBuffer->dataOffset :
					0U;

			if (!vertexBuffer || !indexBuffer || indexCount == 0U ||
				vertexStride == 0U) {
				publishedReady.store(false, std::memory_order_release);
				return false;
			}

			const auto vertexAddress =
				reinterpret_cast<std::uintptr_t>(vertexBuffer);
			const auto previousVertex =
				publishedVertexBuffer.load(std::memory_order_relaxed);

			// Publish readiness last. DrawIndexedHook acquires it before
			// comparing the relaxed fields, so a new object cannot be paired
			// with the previous object's count or suballocation.
			publishedReady.store(false, std::memory_order_release);
			publishedVertexBuffer.store(
				vertexAddress,
				std::memory_order_relaxed);
			publishedIndexBuffer.store(
				reinterpret_cast<std::uintptr_t>(indexBuffer),
				std::memory_order_relaxed);
			publishedIndexCount.store(indexCount, std::memory_order_relaxed);
			publishedVertexStride.store(
				vertexStride,
				std::memory_order_relaxed);
			publishedVertexDataOffset.store(
				vertexDataOffset,
				std::memory_order_relaxed);
			publishedIndexDataOffset.store(
				indexDataOffset,
				std::memory_order_relaxed);
			publishedReady.store(true, std::memory_order_release);

			if (previousVertex != vertexAddress) {
				logger::info(
					"Published automatic STS {} identity: surface={}, "
					"VB={:p}, IB={:p}, indices={}, stride={}, "
					"vertexDataOffset={}, indexDataOffset={}",
					label,
					object->name.c_str(),
					static_cast<void*>(vertexBuffer),
					static_cast<void*>(indexBuffer),
					indexCount,
					vertexStride,
					vertexDataOffset,
					indexDataOffset);
			}
			return true;
		};

		if (!publishIdentity(
				renderSurface,
				automaticSTSVertexBuffer,
				automaticSTSIndexBuffer,
				automaticSTSIndexCount,
				automaticSTSVertexStride,
				automaticSTSVertexDataOffset,
				automaticSTSIndexDataOffset,
				automaticSTSGeometryReady,
				"ScopeFade")) {
			InvalidateAutomaticSTSGeometry();
			return;
		}

		// Reticle:0 is not the aperture geometry and must never supply its
		// transform or depth. It is published only as a draw-order witness.
		// Stage 4d.2c proved that this exact identity also vanishes on a recoil
		// frame even though a reticle remains visible, so the visible reticle
		// must be a different geometry or render path.
		publishIdentity(
			reticleSurface,
			automaticSTSReticleVertexBuffer,
			automaticSTSReticleIndexBuffer,
			automaticSTSReticleIndexCount,
			automaticSTSReticleVertexStride,
			automaticSTSReticleVertexDataOffset,
			automaticSTSReticleIndexDataOffset,
			automaticSTSReticleGeometryReady,
			"Reticle");

		// The direct ScopeAiming child ending in _STS is the authored aiming
		// housing selected by FindSTSAperture. Unlike the front ScopeFade plane,
		// the housing remains visibly present throughout recoil. Stage 4d.2d
		// publishes it only as a candidate injection anchor; this diagnostic
		// build never changes its shaders or submits extra geometry.
		publishIdentity(
			aimingHousingSurface,
			automaticSTSHousingVertexBuffer,
			automaticSTSHousingIndexBuffer,
			automaticSTSHousingIndexCount,
			automaticSTSHousingVertexStride,
			automaticSTSHousingVertexDataOffset,
			automaticSTSHousingIndexDataOffset,
			automaticSTSHousingGeometryReady,
			"AimingHousing");
	}

	void D3D::InvalidateAutomaticSTSGeometry()
	{
		// The opaque addresses are left intact for post-mortem diagnostics.
		// Clearing readiness first prevents a render-thread draw from matching
		// an object that has just been unequipped.
		automaticSTSGeometryReady.store(false, std::memory_order_release);
		automaticSTSReticleGeometryReady.store(
			false,
			std::memory_order_release);
		automaticSTSHousingGeometryReady.store(
			false,
			std::memory_order_release);
		automaticSTSScopeFadeDrawsThisFrame.store(
			0U,
			std::memory_order_relaxed);
		automaticSTSReticleDrawsThisFrame.store(
			0U,
			std::memory_order_relaxed);
		automaticSTSHousingDrawsThisFrame.store(
			0U,
			std::memory_order_relaxed);
		automaticSTSScopeFadeInstancedDrawsThisFrame.store(
			0U,
			std::memory_order_relaxed);
		automaticSTSReticleInstancedDrawsThisFrame.store(
			0U,
			std::memory_order_relaxed);
		automaticSTSHousingInstancedDrawsThisFrame.store(
			0U,
			std::memory_order_relaxed);
		automaticSTSDrawOrdinalThisFrame.store(
			0U,
			std::memory_order_relaxed);
		automaticSTSLastScopeFadeOrdinal.store(
			0U,
			std::memory_order_relaxed);
		automaticSTSLastReticleOrdinal.store(
			0U,
			std::memory_order_relaxed);
		automaticSTSLastHousingOrdinal.store(
			0U,
			std::memory_order_relaxed);
	}

	void D3D::PublishAutomaticSTSGunState(
		std::uint32_t gunState) noexcept
	{
		automaticSTSGunState.store(gunState, std::memory_order_release);
	}

	void D3D::SetFrameworkRenderAnchor(bool enabled)
	{
		frameworkRenderAnchor = enabled;
	}

	HRESULT __fastcall D3D::PresentHook(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags)
	{

		if (!oldFuncs.phookD3D11Present) {
			return DXGI_ERROR_INVALID_CALL;
		}
		bSelfDraw = false;
		lastPresentedSwapChain.store(pSwapChain, std::memory_order_release);

		const auto& verification = MagnaScope::GetSettings();
		if (verification.AllowsGeometryProbe() &&
			automaticSTSGeometryReady.load(std::memory_order_acquire)) {
			const auto scopeFadeDraws =
				automaticSTSScopeFadeDrawsThisFrame.exchange(
					0U,
					std::memory_order_acq_rel);
			const auto reticleDraws =
				automaticSTSReticleDrawsThisFrame.exchange(
					0U,
					std::memory_order_acq_rel);
			const auto housingDraws =
				automaticSTSHousingDrawsThisFrame.exchange(
					0U,
					std::memory_order_acq_rel);
			const auto scopeFadeInstancedDraws =
				automaticSTSScopeFadeInstancedDrawsThisFrame.exchange(
					0U,
					std::memory_order_acq_rel);
			const auto reticleInstancedDraws =
				automaticSTSReticleInstancedDrawsThisFrame.exchange(
					0U,
					std::memory_order_acq_rel);
			const auto housingInstancedDraws =
				automaticSTSHousingInstancedDrawsThisFrame.exchange(
					0U,
					std::memory_order_acq_rel);
			const auto totalAutomaticDraws =
				automaticSTSDrawOrdinalThisFrame.exchange(
					0U,
					std::memory_order_acq_rel);
			const auto scopeFadeOrdinal =
				automaticSTSLastScopeFadeOrdinal.exchange(
					0U,
					std::memory_order_acq_rel);
			const auto reticleOrdinal =
				automaticSTSLastReticleOrdinal.exchange(
					0U,
					std::memory_order_acq_rel);
			const auto housingOrdinal =
				automaticSTSLastHousingOrdinal.exchange(
					0U,
					std::memory_order_acq_rel);
			const auto gunState =
				automaticSTSGunState.load(std::memory_order_acquire);

			static std::atomic_uint32_t previousScopeFadeDraws{
				std::numeric_limits<std::uint32_t>::max()
			};
			static std::atomic_uint32_t previousReticleDraws{
				std::numeric_limits<std::uint32_t>::max()
			};
			static std::atomic_uint32_t previousHousingDraws{
				std::numeric_limits<std::uint32_t>::max()
			};
			static std::atomic_uint32_t previousInstancedDraws{
				std::numeric_limits<std::uint32_t>::max()
			};
			static std::atomic_uint32_t previousGunState{
				std::numeric_limits<std::uint32_t>::max()
			};
			const bool firingSighted =
				gunState ==
				static_cast<std::uint32_t>(RE::GUN_STATE::kFireSighted);
			const bool changed =
				scopeFadeDraws != previousScopeFadeDraws.load(
					std::memory_order_relaxed) ||
				reticleDraws != previousReticleDraws.load(
					std::memory_order_relaxed) ||
				housingDraws != previousHousingDraws.load(
					std::memory_order_relaxed) ||
				scopeFadeInstancedDraws +
						reticleInstancedDraws +
						housingInstancedDraws !=
					previousInstancedDraws.load(
						std::memory_order_relaxed) ||
				gunState != previousGunState.load(
					std::memory_order_relaxed);
			if (firingSighted || changed) {
				logger::info(
					"Stage 4d.2d draw telemetry: gunState={} ({}), "
					"automaticDraws={}, "
					"ScopeFade=DI:{} DII:{} @{}, "
					"Reticle=DI:{} DII:{} @{}, "
					"Housing=DI:{} DII:{} @{}",
					gunState,
					firingSighted ? "FireSighted" :
						(gunState ==
						 static_cast<std::uint32_t>(
							 RE::GUN_STATE::kSighted) ?
							 "Sighted" :
							 "other"),
					totalAutomaticDraws,
					scopeFadeDraws,
					scopeFadeInstancedDraws,
					scopeFadeOrdinal,
					reticleDraws,
					reticleInstancedDraws,
					reticleOrdinal,
					housingDraws,
					housingInstancedDraws,
					housingOrdinal);
			}
			previousScopeFadeDraws.store(
				scopeFadeDraws,
				std::memory_order_relaxed);
			previousReticleDraws.store(
				reticleDraws,
				std::memory_order_relaxed);
			previousHousingDraws.store(
				housingDraws,
				std::memory_order_relaxed);
			previousInstancedDraws.store(
				scopeFadeInstancedDraws +
					reticleInstancedDraws +
					housingInstancedDraws,
				std::memory_order_relaxed);
			previousGunState.store(
				gunState,
				std::memory_order_relaxed);
		}

		#ifdef _DEBUG
		if (GetAsyncKeyState(VK_F4) & 1) {
			logger::info("Frame capture requested");
			rdoc_api->TriggerCapture();
		}
		#endif  // _DEBUG

		// The TAA vtable hook and framework before-render callback both run
		// before the real Present. If neither produced the scope this frame,
		// composite now. The shared guard also prevents a double draw no
		// matter which Present hook is outermost.
		if (D3D::isEnableRender && !renderPassHandledThisFrame) {
			D3DInstance->Render();
			compositedThisFrame = lastRenderProducedComposite;
		}

		const auto result =
			oldFuncs.phookD3D11Present(pSwapChain, SyncInterval, Flags);
		renderedAtTAAThisFrame = false;
		compositedThisFrame = false;
		renderPassHandledThisFrame = false;
		return result;
	}

	HRESULT __stdcall D3D::ResizeBuffersHook(
		IDXGISwapChain* swapChain,
		UINT bufferCount,
		UINT width,
		UINT height,
		DXGI_FORMAT newFormat,
		UINT flags)
	{
		if (!oldFuncs.resizeBuffers) {
			return DXGI_ERROR_INVALID_CALL;
		}

		D3DInstance->ReleaseSizeDependentResources();

		const HRESULT result = oldFuncs.resizeBuffers(
			swapChain,
			bufferCount,
			width,
			height,
			newFormat,
			flags);
		if (SUCCEEDED(result)) {
			windowWidth = static_cast<int>(width);
			windowHeight = static_cast<int>(height);
			logger::info("Swap chain resized to {}x{}; scope resources will be recreated", width, height);
		} else {
			logger::error("ResizeBuffers failed with HRESULT 0x{:08X}", static_cast<std::uint32_t>(result));
		}
		return result;
	}

	D3D* D3D::GetSington()
	{
		static D3D instance;	
		return &instance;
	}

	void D3D::InitRenderDoc()
	{
#ifdef _DEBUG
		HMODULE mod = LoadLibraryA("renderdoc.dll");
		if (!mod) {
			logger::error("Failed to load renderdoc.dll. Error code: {}", GetLastError());
			return;
		}

		pRENDERDOC_GetAPI RENDERDOC_GetAPI =
			(pRENDERDOC_GetAPI)GetProcAddress(mod, "RENDERDOC_GetAPI");
		if (!RENDERDOC_GetAPI) {
			logger::error("Failed to get RENDERDOC_GetAPI function. Error code: {}", GetLastError());
			return;
		}

		int ret = RENDERDOC_GetAPI(eRENDERDOC_API_Version_1_6_0, (void**)&rdoc_api);
		if (ret != 1) {
			logger::error("RENDERDOC_GetAPI failed with return code: {}", ret);
			return;
		}
		int major, minor, patch;
		rdoc_api->GetAPIVersion(&major, &minor, &patch);
		logger::info("RenderDoc API v{}.{}.{} loaded", major, minor, patch);
		// Set RenderDoc options
		rdoc_api->SetCaptureOptionU32(eRENDERDOC_Option_AllowFullscreen, 1);
		rdoc_api->SetCaptureOptionU32(eRENDERDOC_Option_AllowVSync, 1);
		rdoc_api->SetCaptureOptionU32(eRENDERDOC_Option_APIValidation, 1);
		rdoc_api->SetCaptureOptionU32(eRENDERDOC_Option_CaptureAllCmdLists, 1);
		rdoc_api->SetCaptureOptionU32(eRENDERDOC_Option_CaptureCallstacks, 1);
		rdoc_api->SetCaptureOptionU32(eRENDERDOC_Option_RefAllResources, 1);

		logger::info("RenderDoc initialized successfully");
#endif
	}

	bool D3D::CreateAndEnableHook(void* target, void* hook, void** original, const char* hookName)
	{
		if (!target || !hook || !original || !IsExecutableAddress(target)) {
			logger::error("{} received an invalid hook target", hookName);
			return false;
		}
		if (MH_CreateHook(target, hook, original) != MH_OK) {
			logger::error("Failed to create {} hook", hookName);
			return false;
		}
		if (MH_EnableHook(target) != MH_OK) {
			logger::error("Failed to enable {} hook", hookName);
			return false;
		}
		logger::info("Installed {} at {:p}", hookName, target);
		return true;
	}

	struct HookedRender_TAA
	{
		// thunk 函数
		static void thunk(
			RE::ImageSpaceEffectTemporalAA* This,
			RE::BSTriShape* a_geometry,
			RE::ImageSpaceEffectParam* a_param)
		{
			// 调用原函数
			const auto original = func.get();
			if (!original || !This) {
				logger::critical(
					"TAA callback received an invalid original function or instance");
				return;
			}

			D3DPERF_BeginEvent(0xffffffff, L"TAA");
			original(This, a_geometry, a_param);
			D3DPERF_EndEvent();
			isActive_TAA = This->IsActive();

			static std::once_flag loggedActiveTaa;
			static std::once_flag loggedInactiveTaa;
			auto& activityLog =
				isActive_TAA ? loggedActiveTaa : loggedInactiveTaa;
			std::call_once(activityLog, [&] {
				logger::info(
					"Stage 3b TAA callback observed: active={}, upscaler={}",
					isActive_TAA,
					upscalerMod != nullptr);
			});

			const auto& verification = MagnaScope::GetSettings();
			const bool renderEnabled =
				D3D::isEnableRender.load(std::memory_order_acquire);
			if (verification.AllowsComposite() && renderEnabled &&
				isActive_TAA && !renderPassHandledThisFrame) {
				// Preserve the original FTS Stage 4 behavior behind the
				// composite gate. It is not reachable during Stage 3b.
				if (!upscalerMod) {
					renderedAtTAAThisFrame = true;
					D3DInstance->Render();
					compositedThisFrame = lastRenderProducedComposite;
					if (!compositedThisFrame && !renderPassHandledThisFrame) {
						renderedAtTAAThisFrame = false;
					}
				}
			} else if (verification.AllowsTAACapture() && renderEnabled &&
				isActive_TAA) {
				if (!upscalerMod) {
					const bool captured =
						D3DInstance->CaptureVerificationTAASource();
					taaVerificationCapturedSinceFramework.store(
						captured,
						std::memory_order_release);
					if (!captured) {
						static std::once_flag loggedMissingTaaTarget;
						std::call_once(loggedMissingTaaTarget, [] {
							logger::warn(
								"Stage 3b TAA callback found no compatible bound "
								"color target; FrameworkPresent remains the fallback");
						});
					}
				}
			}
		}
		static inline REL::Relocation<decltype(thunk)*> func;
	};

	bool InstallGuardedTAAHook()
	{
		static std::mutex installLock;
		static bool attempted = false;
		static bool installed = false;
		std::scoped_lock lock{ installLock };
		if (attempted) {
			return installed;
		}
		attempted = true;

		if (!REX::FModule::IsRuntimeOG()) {
			logger::warn("TAA callback is not installed outside Fallout 4 OG");
			return false;
		}

		upscalerMod = DetectUpscalerOrFrameGeneration();
		REL::Relocation<std::uintptr_t> taaVtable{
			RE::ImageSpaceEffectTemporalAA::VTABLE[0]
		};
		const auto renderSlotAddress =
			taaVtable.address() + sizeof(std::uintptr_t);
		const auto originalRender =
			*reinterpret_cast<const std::uintptr_t*>(renderSlotAddress);
		if (!IsExecutableAddress(
				reinterpret_cast<const void*>(originalRender))) {
			logger::error(
				"TAA vtable slot 1 does not contain an executable target");
			return false;
		}

		// A vtable slot redirected into another DLL belongs to that plugin. Do
		// not overwrite it because that would silently break the other render
		// integration and could invalidate its call chain.
		HMODULE targetOwner = nullptr;
		const bool foundOwner = GetModuleHandleExW(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
				GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(originalRender),
			&targetOwner) != FALSE;
		const HMODULE falloutModule = GetModuleHandleW(nullptr);
		if (!foundOwner || targetOwner != falloutModule) {
			logger::warn(
				"TAA vtable slot 1 is already owned by another module; "
				"the callback was not replaced");
			return false;
		}

		// Publish the call-through before exposing the thunk in the vtable.
		// The render thread can execute this slot concurrently with
		// kGameDataReady, so the reverse order creates a short null-original
		// race even though installation is otherwise single-threaded.
		HookedRender_TAA::func =
			REL::Relocation<decltype(HookedRender_TAA::thunk)*>(
				originalRender);
		const auto replacedRender = taaVtable.write_vfunc(
			1,
			reinterpret_cast<std::uintptr_t>(&HookedRender_TAA::thunk));
		if (replacedRender != originalRender) {
			taaVtable.write_vfunc(1, replacedRender);
			logger::critical(
				"TAA vtable changed during installation; original slot restored");
			return false;
		}
		const auto installedRender =
			*reinterpret_cast<const std::uintptr_t*>(renderSlotAddress);
		if (installedRender !=
			reinterpret_cast<std::uintptr_t>(&HookedRender_TAA::thunk)) {
			taaVtable.write_vfunc(1, originalRender);
			logger::critical(
				"TAA vtable did not retain the callback; original slot restored");
			return false;
		}
		installed = true;
		logger::info(
			"Installed guarded OG TAA source-capture callback at {:p}; "
			"original={:p}",
			reinterpret_cast<void*>(renderSlotAddress),
			reinterpret_cast<void*>(originalRender));
		return true;
	}

	bool D3D::InstallVerificationTAAHook()
	{
		return InstallGuardedTAAHook();
	}


	DWORD __stdcall D3D::HookDX11_Init()
	{
		// Detect an already loaded upscaler without changing its module
		// reference count or forcing it to initialize out of order.
		upscalerMod = DetectUpscalerOrFrameGeneration();

		logger::info("HookDX11_Init");

		const auto* rendererData = RE::BSGraphics::GetRendererData();
		const auto* renderWindow = RE::BSGraphics::GetCurrentRendererWindow();
		if (!rendererData || !renderWindow || !renderWindow->swapChain ||
			!rendererData->device || !rendererData->context) {
			logger::error("Renderer objects are unavailable; DX11 hooks were not installed");
			return 1;
		}
		g_Swapchain = (IDXGISwapChain*)static_cast<void*>(renderWindow->swapChain);
		g_Device = (ID3D11Device*)static_cast<void*>(rendererData->device);
		g_Context = (ID3D11DeviceContext*)static_cast<void*>(rendererData->context);

		EnableDebugPrivilege();

		logger::info("Post D3D11CreateDeviceAndSwapChain");

		// 获取虚函数表
		pSwapChainVTable = *reinterpret_cast<DWORD_PTR**>(g_Swapchain.Get());
		pDeviceVTable = *reinterpret_cast<DWORD_PTR**>(g_Device.Get());
		pDeviceContextVTable = *reinterpret_cast<DWORD_PTR**>(g_Context.Get());

		sdh = ScopeData::ScopeDataHandler::GetSingleton();

		logger::info("Get VTable");

		if (MagnaScope::GetSettings().AllowsGeometryProbe() &&
			!InitGeometryProbeEffect()) {
			InvalidateAutomaticSTSGeometry();
			logger::critical(
				"Stage 4d geometry probe initialization failed; "
				"all STS draws will pass through unchanged");
		}

		// MinHook may already be initialized by another DX11 integration.
		const auto minHookResult = MH_Initialize();
		if (minHookResult != MH_OK && minHookResult != MH_ERROR_ALREADY_INITIALIZED) {
			logger::error("Failed to initialize MinHook");
			return 1;
		}

		// 批量创建钩子
		const std::pair<DWORD_PTR*, HookInfo> hooks[] = {
			{ pSwapChainVTable, HookInfo{ 8, reinterpret_cast<void*>(PresentHook), reinterpret_cast<void**>(&oldFuncs.phookD3D11Present), "PresentHook" } },
			{ pSwapChainVTable, HookInfo{ 13, reinterpret_cast<void*>(ResizeBuffersHook), reinterpret_cast<void**>(&oldFuncs.resizeBuffers), "ResizeBuffersHook" } },
			{ pDeviceContextVTable, HookInfo{ 12, reinterpret_cast<void*>(DrawIndexedHook), reinterpret_cast<void**>(&oldFuncs.phookD3D11DrawIndexed), "DrawIndexedHook" } },
			// Windows SDK d3d11.h declares DrawIndexedInstanced at
			// ID3D11DeviceContext vtable slot 20. This diagnostic hook only
			// counts exact published STS identities and always forwards the
			// original draw unchanged.
			{ pDeviceContextVTable, HookInfo{ 20, reinterpret_cast<void*>(DrawIndexedInstancedHook), reinterpret_cast<void**>(&oldFuncs.phookD3D11DrawIndexedInstanced), "DrawIndexedInstancedHook" } },
		};

		for (const auto& [vtable, info] : hooks) {
			CreateAndEnableHook(reinterpret_cast<void*>(vtable[info.index]), info.hook, info.original, info.name);
		}

		if (!InstallGuardedTAAHook()) {
			logger::warn("TAA render hook guard failed; Present fallback remains active");
		}
		logger::info("Render anchors active: TAA preferred, Present fallback");

		logger::info("Install Hook");

#ifdef _DEBUG
		if (rdoc_api && g_Device.Get()) {
			IDXGIDevice* pDXGIDevice = nullptr;
			if (SUCCEEDED(g_Device->QueryInterface(__uuidof(IDXGIDevice), (void**)&pDXGIDevice))) {
				rdoc_api->SetActiveWindow((void*)pDXGIDevice, g_hWnd);
				logger::info("Set RenderDoc active window to {:x}", (uintptr_t)g_hWnd);
				pDXGIDevice->Release();
			} else {
				logger::error("Failed to get DXGI device for RenderDoc");
			}
		}
#endif  // _DEBUG

		return S_OK;
	}

	D3D11_HOOK_API void D3D::ImplHookDX11_Init(HMODULE hModule, void* hwnd)
	{
		g_hWnd = (HWND)hwnd;
		g_hModule = hModule;
		HookDX11_Init();
	}

	void D3D::QueryChangeReticleTexture() { bChangeAimTexture = true; }
	void D3D::ResetZoomDelta() { bResetZoomDelta = true; }
	void D3D::AdjustZoomDelta(float delta) { gameZoomDelta += delta; }
	void D3D::SetZoom(float zoom) { gameZoomDelta = zoom; }
	void D3D::SetFinishAimAnim(bool flag) { bFinishAimAnim = flag; }
	void D3D::SetScopeEffect(bool flag) { isEnableScopeEffect = flag; }
	bool D3D::GetScopeEffect() { return isEnableScopeEffect; }
	void D3D::SetInterfaceTextRefresh(bool flag)
	{
		bRefreshChar = flag;
		bEnableEditMode = false;
	}

	void D3D::SetGameConstData(GameConstBuffer c){ gameConstBuffer = c;}
	void D3D::InitPlayerData(RE::PlayerCharacter* pl, RE::PlayerCamera* pc){ player = pl; pcam = pc;}
	void D3D::SetNVG(int flag) { bEnableNVG = flag; }
	void D3D::StartScope(bool flag) { bStartScope = flag; }

	D3D::OldFuncs D3D::oldFuncs;
	std::atomic_bool D3D::isEnableRender = false;
	bool D3D::frameworkRenderAnchor = false;
	std::atomic<float> D3D::projectedLensX = 0.0F;
	std::atomic<float> D3D::projectedLensY = 0.0F;
	std::atomic<float> D3D::projectedAimX = 0.0F;
	std::atomic<float> D3D::projectedAimY = 0.0F;
	std::atomic<float> D3D::projectedLensRadiusX = 0.0F;
	std::atomic<float> D3D::projectedLensRadiusY = 0.0F;
	std::atomic<float> D3D::projectedActivationProgress = 0.0F;
	std::atomic<float> D3D::projectedSourceWidth = 0.0F;
	std::atomic<float> D3D::projectedSourceHeight = 0.0F;
	std::atomic_bool D3D::projectedAutomaticSTS = false;
	std::atomic_bool D3D::projectedTrackingReady = false;
	std::atomic<std::uintptr_t> D3D::automaticSTSVertexBuffer = 0;
	std::atomic<std::uintptr_t> D3D::automaticSTSIndexBuffer = 0;
	std::atomic_uint32_t D3D::automaticSTSIndexCount = 0;
	std::atomic_uint32_t D3D::automaticSTSVertexStride = 0;
	std::atomic_uint32_t D3D::automaticSTSVertexDataOffset = 0;
	std::atomic_uint32_t D3D::automaticSTSIndexDataOffset = 0;
	std::atomic_bool D3D::automaticSTSGeometryReady = false;
	std::atomic<std::uintptr_t> D3D::automaticSTSReticleVertexBuffer = 0;
	std::atomic<std::uintptr_t> D3D::automaticSTSReticleIndexBuffer = 0;
	std::atomic_uint32_t D3D::automaticSTSReticleIndexCount = 0;
	std::atomic_uint32_t D3D::automaticSTSReticleVertexStride = 0;
	std::atomic_uint32_t D3D::automaticSTSReticleVertexDataOffset = 0;
	std::atomic_uint32_t D3D::automaticSTSReticleIndexDataOffset = 0;
	std::atomic_bool D3D::automaticSTSReticleGeometryReady = false;
	std::atomic<std::uintptr_t> D3D::automaticSTSHousingVertexBuffer = 0;
	std::atomic<std::uintptr_t> D3D::automaticSTSHousingIndexBuffer = 0;
	std::atomic_uint32_t D3D::automaticSTSHousingIndexCount = 0;
	std::atomic_uint32_t D3D::automaticSTSHousingVertexStride = 0;
	std::atomic_uint32_t D3D::automaticSTSHousingVertexDataOffset = 0;
	std::atomic_uint32_t D3D::automaticSTSHousingIndexDataOffset = 0;
	std::atomic_bool D3D::automaticSTSHousingGeometryReady = false;
	std::atomic_uint32_t D3D::automaticSTSScopeFadeDrawsThisFrame = 0;
	std::atomic_uint32_t D3D::automaticSTSReticleDrawsThisFrame = 0;
	std::atomic_uint32_t D3D::automaticSTSHousingDrawsThisFrame = 0;
	std::atomic_uint32_t
		D3D::automaticSTSScopeFadeInstancedDrawsThisFrame = 0;
	std::atomic_uint32_t
		D3D::automaticSTSReticleInstancedDrawsThisFrame = 0;
	std::atomic_uint32_t
		D3D::automaticSTSHousingInstancedDrawsThisFrame = 0;
	std::atomic_uint32_t D3D::automaticSTSDrawOrdinalThisFrame = 0;
	std::atomic_uint32_t D3D::automaticSTSLastScopeFadeOrdinal = 0;
	std::atomic_uint32_t D3D::automaticSTSLastReticleOrdinal = 0;
	std::atomic_uint32_t D3D::automaticSTSLastHousingOrdinal = 0;
	std::atomic_uint32_t D3D::automaticSTSGunState = 0;
	bool D3D::bStartScope = false;
	bool D3D::bFinishAimAnim = false;
	bool D3D::bRefreshChar = true;
	int D3D::bEnableNVG = 0;
	bool D3D::bQueryRender = false;
	bool D3D::bIsInGame = false;
	bool D3D::isEnableScopeEffect = false;
	bool D3D::bEnableEditMode = false;
	float D3D::editZoomMin = 1.0F;
	float D3D::editZoomMax = 4.0F;
	bool D3D::bLegacyMode;

	std::once_flag D3D::flagOnce;
}
