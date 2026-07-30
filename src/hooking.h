#pragma once

#include <d3d11.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include "FTSData.h"
#include <REX/W32/COMPTR.h>


#define D3D11_HOOK_API
#define SAFE_RELEASE(p) { if ((p)) { (p)->Release(); (p) = nullptr; } }
#ifndef HR
#	define HR(x)                                                 \
		{                                                         \
			HRESULT hr = (x);                                     \
			if (FAILED(hr)) {                                     \
				logger::error("[-] {}, {}, {}", __FILE__, __LINE__, hr); \
			}                                                     \
		}
#endif


template <class T>
using ComPtr = REX::W32::ComPtr<T>;

extern ComPtr<IDXGISwapChain> g_Swapchain;
extern ComPtr<ID3D11Device> g_Device;
extern ComPtr<ID3D11DeviceContext> g_Context;

namespace Hook
{
	
	HRESULT CreateShaderFromFile(
		const WCHAR* csoFileNameInOut,
		const WCHAR* hlslFileName,
		LPCSTR entryPoint,
		LPCSTR shaderModel,
		ID3DBlob** ppBlobOut);

	using namespace DirectX;

	class D3D
	{
		using Present = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT);
		using ResizeBuffers = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
		typedef HRESULT(__stdcall* D3D11PresentHook)(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);
		typedef void(__stdcall* D3D11DrawIndexedHook)(ID3D11DeviceContext* pContext, UINT IndexCount, UINT StartIndexLocation, INT BaseVertexLocation);
		typedef void(__stdcall* D3D11DrawIndexedInstancedHook)(
			ID3D11DeviceContext* pContext,
			UINT IndexCountPerInstance,
			UINT InstanceCount,
			UINT StartIndexLocation,
			INT BaseVertexLocation,
			UINT StartInstanceLocation);
		

	private:
		static std::once_flag flagOnce;

	public:
		
		static D3D* GetSington();
		void InitRenderDoc();
	public:

		__declspec(align(16)) struct ConstBufferData
		{
			float width;
			float height;
		};

	public:

		struct ScopeEffectShaderData
		{
			float camDepth = 1;
			float GameFov = 90;
			float ScopeEffect_Zoom = 1.5F;
			float parallax_Radius = 2.0F ;

			float parallax_relativeFogRadius = 8.0F;
			float parallax_scopeSwayAmount = 2.0F;
			float parallax_maxTravel = 16.0F;
			float ReticleSize = 8;

			float nvIntensity = 3;
			float BaseWeaponPos = 0;
			float MovePercentage = 0;
			int EnableZMove = 0;

			int isCircle = 1;
			int EnableNV = 0;
			int EnableMerge = 0;
			float baseFovAdjustTarget = 0;

			XMFLOAT2 ScopeEffect_Size = { 0, 0 };
			XMFLOAT2 ScopeEffect_OriPositionOffset = { 0, 0 };
			XMFLOAT2 ScopeEffect_OriSize = { 0, 0 };
			XMFLOAT2 ScopeEffect_Offset = { 0, 0 };

			XMFLOAT3 eyeDirection = {0,0,0};
			float targetAdjustFov = 0;

			XMFLOAT3 eyeDirectionLerp = {0,0,0};
			float padding2 = 0;
			XMFLOAT3 eyeTranslationLerp = { 0, 0, 0 };
			float padding3 = 0;
			XMFLOAT3 CurrWeaponPos = { 0, 0, 0 };
			float padding4 = 0;
			XMFLOAT3 CurrRootPos = { 0, 0, 0 };
			float padding5 = 0;
			XMFLOAT4X4 CameraRotation = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

			XMFLOAT2 FTS_ScreenPos = { 0, 0 };
			XMFLOAT2 reticle_Offset = { 0, 0 };

			XMFLOAT4X4 projMat;	
			XMFLOAT4 rect;

			// Mirrors the FishEye block appended to the HLSL cbuffer.
			float FishEyeStrength = 0;
			float FishEyePower = 2;
			XMFLOAT2 padding6 = { 0, 0 };
		};
		static_assert(
			sizeof(ScopeEffectShaderData) == 352,
			"ScopeEffectData must match Triangle.hlsli");

		struct GameConstBuffer
		{
			RE::NiPoint3 virDir;
			RE::NiPoint3 lastVirDir;
			RE::NiPoint3 VirDirLerp;
			RE::NiPoint3 VirTransLerp;
			RE::NiPoint3 weaponPos;
			RE::NiPoint3 rootPos;
			RE::NiPoint3 ftsScreenPos;
			RE::NiMatrix3 camMat;
			RE::NiMatrix3 ftsLocalMat;
			RE::NiMatrix3 ftsWorldMat;
			float deltaZoom;
		};

		struct LensProjectionSnapshot
		{
			float centerX = 0.0F;
			float centerY = 0.0F;
			// The physical mask center and authored reticle aim point are
			// intentionally separate. STS authors may offset the reticle while
			// keeping ScopeFade centered in the optic housing.
			float aimCenterX = 0.0F;
			float aimCenterY = 0.0F;
			// Projected radii are expressed in the same source-buffer pixels
			// as centerX and centerY. Keeping the two axes separate lets the
			// Menu Framework output scale each axis independently when an
			// upscaler or frame-generation proxy changes the displayed size.
			float radiusX = 0.0F;
			float radiusY = 0.0F;
			float sourceWidth = 0.0F;
			float sourceHeight = 0.0F;
			bool renderEnabled = false;
			bool automaticSTS = false;
			// The mask center follows the optical plane raw. Only size and alpha
			// use this ease value, which prevents both corner pop-in and trailing.
			float activationProgress = 0.0F;
			bool trackingReady = false;
		};

		struct ScreenSphereProjection
		{
			RE::NiPoint3 center{};
			float radiusX = 0.0F;
			float radiusY = 0.0F;
			bool valid = false;
		};

	public:

		D3D11_HOOK_API void ImplHookDX11_Init(HMODULE hModule, void* hwnd);
		bool InstallVerificationTAAHook();
		void EnableRender(bool flag)
		{
			// HookedUpdate publishes this gate from the game thread. The TAA
			// callback consumes it on Fallout 4's render thread, so a plain
			// bool would be an undefined cross-thread data race.
			isEnableRender.store(flag, std::memory_order_release);
		}
		void Render();
		void RenderFromFramework();
		void SetFrameworkRenderAnchor(bool enabled);
		bool CaptureVerificationTAASource();
		void PublishLensProjection(
			float centerX,
			float centerY,
			float aimCenterX,
			float aimCenterY,
			float radiusX,
			float radiusY,
			bool automaticSTS,
			float activationProgress);
		void InvalidateLensProjection();
		[[nodiscard]] LensProjectionSnapshot GetLensProjectionSnapshot() const;
		// Publish the exact renderer buffers owned by STS's required
		// ScopeFade. DrawIndexedHook compares only opaque buffer identity and
		// suballocation metadata. It never dereferences a scene-graph object
		// on the render thread.
		void PublishAutomaticSTSGeometry(
			RE::NiAVObject* renderSurface,
			RE::NiAVObject* reticleSurface,
			RE::NiAVObject* aimingHousingSurface);
		void InvalidateAutomaticSTSGeometry();
		// Publish the player's gun state from the game thread. Stage 4 draw
		// telemetry consumes only the numeric value on the render thread.
		void PublishAutomaticSTSGunState(std::uint32_t gunState) noexcept;
		bool InitEffect();
		bool InitLegacyEffect();
		bool InitResource();
		void OnResize();
		void ReleaseSizeDependentResources();
		void UpdateScene(ScopeData::FTSData*);
		void CreateBlender();
		void QueryChangeReticleTexture();
		void ResetZoomDelta();
		void AdjustZoomDelta(float delta);
		void SetZoom(float zoom);
		void ScreenTextureMod();
		void RenderToReticleTexture();
		void RenderToReticleTextureNew(UINT IndexCount, UINT StartIndexLocation, INT BaseVertexLocation);
		void MapScopeEffectBuffer(ScopeEffectShaderData);

		RE::NiPoint3 WorldToScreen(RE::NiAVObject* cam, RE::NiAVObject* obj, float fov);
		RE::NiPoint3 WorldPointToScreen(
			RE::NiAVObject* cam,
			const RE::NiPoint3& worldPoint,
			float fov);
		[[nodiscard]] ScreenSphereProjection ProjectWorldSphereToScreen(
			RE::NiAVObject* cam,
			const RE::NiPoint3& worldCenter,
			float worldRadius,
			float fov);

	private:
		D3D() {}
		~D3D() {}

		D3D(const D3D&) = delete;
		D3D(D3D&&) = delete;

		D3D& operator=(const D3D&) = delete;
		D3D& operator=(D3D&&) = delete;

		static HRESULT __stdcall PresentHook(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);
		static HRESULT __stdcall ResizeBuffersHook(
			IDXGISwapChain* swapChain,
			UINT bufferCount,
			UINT width,
			UINT height,
			DXGI_FORMAT newFormat,
			UINT flags);
		static void __stdcall DrawIndexedHook(ID3D11DeviceContext* pContext, UINT IndexCount, UINT StartIndexLocation, INT BaseVertexLocation);
		static void __stdcall DrawIndexedInstancedHook(
			ID3D11DeviceContext* pContext,
			UINT IndexCountPerInstance,
			UINT InstanceCount,
			UINT StartIndexLocation,
			INT BaseVertexLocation,
			UINT StartInstanceLocation);
		static bool CreateAndEnableHook(void* target, void* hook, void** original, const char* hookName);

		DWORD __stdcall HookDX11_Init();
		bool CaptureVerificationSource(
			ID3D11Texture2D* sourceTexture,
			const char* anchorName,
			ID3D11Device* captureDevice = nullptr,
			ID3D11DeviceContext* captureContext = nullptr);
		bool InitGeometryProbeEffect();

		struct VSConstantBufferSlot
		{
			UINT StartSlot;
			UINT NumBuffers;
			ID3D11Buffer* const* ppBuffers;
		};

		void SetupCommonRenderState(
			ID3D11VertexShader* vs, ID3D11ClassInstance* const* vsClassInstances, UINT vsClassInstancesCount,
			ID3D11PixelShader* ps, ID3D11InputLayout* inputLayout, ID3D11BlendState* blendState, const std::vector<VSConstantBufferSlot>& vsCBSlots,
			ID3D11Buffer* indexBuffer,
			DXGI_FORMAT indexFormat,
			UINT indexOffset,
			ID3D11Buffer* const* vertexBuffers,
			const UINT* strides,
			const UINT* offsets,
			UINT numVertexBuffers,ID3D11RenderTargetView* backBufferRTV);

		void LoadAimTexture(const std::string& path);
		template <typename T>
		void UpdateConstantBuffer(const ComPtr<ID3D11Buffer>& buffer, const T& data);
		void UpdateGameConstants(const GameConstBuffer& src, ScopeEffectShaderData& dst);

		struct OldFuncs
		{
			D3D11PresentHook phookD3D11Present = nullptr;
			ResizeBuffers resizeBuffers = nullptr;
			D3D11DrawIndexedHook phookD3D11DrawIndexed = nullptr;
			D3D11DrawIndexedInstancedHook phookD3D11DrawIndexedInstanced =
				nullptr;
		};

	public:
		void SetNVG(int);
		void SetGameConstData(GameConstBuffer);
		void SetScopeEffect(bool);
		bool GetScopeEffect();
		void InitPlayerData(RE::PlayerCharacter*, RE::PlayerCamera*);
		void StartScope(bool flag);
		void SetFinishAimAnim(bool flag);
		void SetInterfaceTextRefresh(bool flag);
		void QueryRender(bool flag) { bQueryRender = flag; }
		bool GetRenderState() { return bQueryRender ; }
		void SetIsInGame(bool flag) { bIsInGame = flag; }
		GameConstBuffer* GetGameConstBuffer() { return &gameConstBuffer; }

	public:
		static bool bLegacyMode;
		static bool isEnableScopeEffect;
		static bool bEnableEditMode;
		static bool bRefreshChar;
		// While the customization menu is in edit mode these bound the mouse
		// wheel zoom instead of the saved profile values, so the menu's
		// magnification sliders preview live before the profile is saved.
		static float editZoomMin;
		static float editZoomMax;
		static std::atomic_bool isEnableRender;
		static bool frameworkRenderAnchor;
		static std::atomic<float> projectedLensX;
		static std::atomic<float> projectedLensY;
		static std::atomic<float> projectedAimX;
		static std::atomic<float> projectedAimY;
		static std::atomic<float> projectedLensRadiusX;
		static std::atomic<float> projectedLensRadiusY;
		static std::atomic<float> projectedActivationProgress;
		static std::atomic<float> projectedSourceWidth;
		static std::atomic<float> projectedSourceHeight;
		static std::atomic_bool projectedAutomaticSTS;
		static std::atomic_bool projectedTrackingReady;
		static std::atomic<std::uintptr_t> automaticSTSVertexBuffer;
		static std::atomic<std::uintptr_t> automaticSTSIndexBuffer;
		static std::atomic_uint32_t automaticSTSIndexCount;
		static std::atomic_uint32_t automaticSTSVertexStride;
		static std::atomic_uint32_t automaticSTSVertexDataOffset;
		static std::atomic_uint32_t automaticSTSIndexDataOffset;
		static std::atomic_bool automaticSTSGeometryReady;
		static std::atomic<std::uintptr_t> automaticSTSReticleVertexBuffer;
		static std::atomic<std::uintptr_t> automaticSTSReticleIndexBuffer;
		static std::atomic_uint32_t automaticSTSReticleIndexCount;
		static std::atomic_uint32_t automaticSTSReticleVertexStride;
		static std::atomic_uint32_t automaticSTSReticleVertexDataOffset;
		static std::atomic_uint32_t automaticSTSReticleIndexDataOffset;
		static std::atomic_bool automaticSTSReticleGeometryReady;
		static std::atomic<std::uintptr_t> automaticSTSHousingVertexBuffer;
		static std::atomic<std::uintptr_t> automaticSTSHousingIndexBuffer;
		static std::atomic_uint32_t automaticSTSHousingIndexCount;
		static std::atomic_uint32_t automaticSTSHousingVertexStride;
		static std::atomic_uint32_t automaticSTSHousingVertexDataOffset;
		static std::atomic_uint32_t automaticSTSHousingIndexDataOffset;
		static std::atomic_bool automaticSTSHousingGeometryReady;
		static std::atomic_uint32_t automaticSTSScopeFadeDrawsThisFrame;
		static std::atomic_uint32_t automaticSTSReticleDrawsThisFrame;
		static std::atomic_uint32_t automaticSTSHousingDrawsThisFrame;
		static std::atomic_uint32_t
			automaticSTSScopeFadeInstancedDrawsThisFrame;
		static std::atomic_uint32_t
			automaticSTSReticleInstancedDrawsThisFrame;
		static std::atomic_uint32_t
			automaticSTSHousingInstancedDrawsThisFrame;
		static std::atomic_uint32_t automaticSTSDrawOrdinalThisFrame;
		static std::atomic_uint32_t automaticSTSLastScopeFadeOrdinal;
		static std::atomic_uint32_t automaticSTSLastReticleOrdinal;
		static std::atomic_uint32_t automaticSTSLastHousingOrdinal;
		static std::atomic_uint32_t automaticSTSGunState;

	private:
		static OldFuncs oldFuncs;

		static bool bStartScope;
		static bool bFinishAimAnim;
		static int bEnableNVG;
		static bool bQueryRender;
		static bool bIsInGame;

	private:
		ComPtr<ID3D11InputLayout> m_pVertexLayout;
		ComPtr<ID3D11Buffer> m_pVertexBuffer;
		ComPtr<ID3D11Buffer> m_pIndexBuffer;

		ComPtr<ID3D11Buffer> m_pConstantBufferData = nullptr;
		ComPtr<ID3D11Buffer> m_pScopeEffectBuffer = nullptr;
		ComPtr<ID3D11Buffer> m_VSBuffer = nullptr;
		ComPtr<ID3D11Buffer> m_VSOutBuffer = nullptr;

		ComPtr<ID3D11VertexShader> m_pVertexShader;
		ComPtr<ID3D11PixelShader> m_pPixelShader;
		ComPtr<ID3D11PixelShader> m_outPutPixelShader;
		ComPtr<ID3D11VertexShader> m_outPutVertexShader;

		ComPtr<ID3D11PixelShader> m_pPixelShader_Legacy;
		ComPtr<ID3D11PixelShader> m_pPixelShader_AutoSTS;
		ComPtr<ID3D11GeometryShader> m_pGeometryShader_STSGeometryFill;
		ComPtr<ID3D11PixelShader> m_pPixelShader_STSGeometryProbe;
		ComPtr<ID3D11PixelShader> m_outPutPixelShader_Legacy;
		ComPtr<ID3D11VertexShader> m_pVertexShader_Legacy;

		ComPtr<ID3D11Texture2D> m_pDepthStencilBuffer; 
		ComPtr<ID3D11RenderTargetView> m_pRenderTargetView;
		ComPtr<ID3D11DepthStencilView> m_pDepthStencilView;
		ComPtr<ID3D11SamplerState> m_pSamplerState; 

		ComPtr<ID3D11BlendState> BSAlphaToCoverage; 
		ComPtr<ID3D11BlendState> BSTransparent; 

		ComPtr<ID3D11Texture2D> mTextDDS;
		ComPtr<ID3D11Resource> mTextDDS_Res;

		ComPtr<ID3D11Texture2D> mBackBuffer;
		ComPtr<ID3D11ShaderResourceView> mTextDDS_SRV;
		ComPtr<ID3D11ShaderResourceView> mShaderResourceView;


		ScopeEffectShaderData scopeData;
		GameConstBuffer gameConstBuffer;
		ConstBufferData constBufferData;
		
		ComPtr<ID3D11Texture2D> mRTRenderTargetTexture;
		ComPtr<ID3D11RenderTargetView> mRTRenderTargetView;
		ComPtr<ID3D11ShaderResourceView> mRTShaderResourceView;

		ComPtr<ID3D11Texture2D> mCurRTTexture;
		ComPtr<ID3D11Texture2D> rtTexture2D;
		D3D11_TEXTURE2D_DESC rtTextureDesc;
	};
}
