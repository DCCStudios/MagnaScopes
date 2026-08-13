#pragma once

#include "ScopeProfile.h"
#include <DirectXMath.h>
#include <REX/W32/COMPTR.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <array>
#include <mutex>
#include <vector>
#include <wrl/client.h>

#define D3D11_HOOK_API
#define SAFE_RELEASE(p)     \
	{                       \
		if ((p)) {          \
			(p)->Release(); \
			(p) = nullptr;  \
		}                   \
	}
#ifndef HR
#	define HR(x)                                                        \
		{                                                                \
			HRESULT hr = (x);                                            \
			if (FAILED(hr)) {                                            \
				logger::error("[-] {}, {}, {}", __FILE__, __LINE__, hr); \
			}                                                            \
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

	// Breadcrumbs for the save-load hang. The log ends mid-frame with no
	// error on every occurrence, which identifies nothing; these atomics are
	// written at the suspect points and a watchdog thread reports where
	// everything stopped, through a raw file handle so a wedged logger cannot
	// swallow the report. Diagnostic only -- nothing reads these for logic.
	namespace HangDiag
	{
		extern std::atomic<std::uint64_t> presentTicks;
		extern std::atomic<std::uint64_t> updateTicks;
		extern std::atomic<std::uint64_t> dispatchTicks;
		// See kPresentPhaseNames / kUpdatePhaseNames in hooking.cpp.
		extern std::atomic<int> presentPhase;
		extern std::atomic<int> updatePhase;
		// Which call site is waiting on / holding mScopeFadeGeometryMutex,
		// and from which thread. 0 means none.
		extern std::atomic<int> geometryWaitSite;
		extern std::atomic<std::uint32_t> geometryWaitThread;
		extern std::atomic<int> geometryHoldSite;
		extern std::atomic<std::uint32_t> geometryHoldThread;
		extern std::atomic<int> ringWaitSite;
		extern std::atomic<std::uint32_t> ringWaitThread;
		extern std::atomic<int> ringHoldSite;
		extern std::atomic<std::uint32_t> ringHoldThread;
		// The last render-side phase MagnaScope entered, when it entered and
		// left, and on which thread. presentPhase alone reports "idle", which
		// is true of a hang that began in our code a millisecond after we
		// returned and equally true of one that began a minute later with no
		// involvement from us. Only the timestamps separate those, and that
		// distinction is the whole question in a hang where MagnaScope appears
		// on no thread's call stack.
		extern std::atomic<int> lastRenderPhase;
		extern std::atomic<std::uint64_t> lastRenderPhaseEnterMs;
		extern std::atomic<std::uint64_t> lastRenderPhaseLeaveMs;
		extern std::atomic<std::uint32_t> lastRenderPhaseThread;
		void ArmWatchdog();
	}

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
			float width = 0.0F;
			float height = 0.0F;
			float scopeFadeMagnification = 1.0F;
			float aimOffsetValid = 0.0F;

			float aimOffsetX = 0.0F;
			float aimOffsetY = 0.0F;
			float imageDenoise = 0.0F;
			float imageSharpen = 0.0F;

			float fishEyeStrength = 0.0F;
			float fishEyePower = 2.0F;
			float lensRadiusX = 0.0F;
			float lensRadiusY = 0.0F;

			float aimCenterX = 0.0F;
			float aimCenterY = 0.0F;
			float reticleMagnification = 1.0F;
			float activationProgress = 0.0F;

			float edgeRefractionStrength = 0.0F;
			float edgeRefractionWidth = 0.15F;
			float edgeChromaticAberration = 0.0F;
			float sceneParallaxStrength = 0.0F;

			float eyeOffsetX = 0.0F;
			float eyeOffsetY = 0.0F;
			// Scales transient ScopeFade-local eye motion without changing
			// the settled optical center. Zero disables the lag effect, one
			// uses the measured motion, and larger values exaggerate it.
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

			// Late automatic-STS reticle controls. Keeping these in b4 lets the
			// isolated reticle composite remain independent of scene zoom.
			float reticleSize = 4.0F;
			float reticleOffsetX = 0.0F;
			float reticleOffsetY = 0.0F;
			// Signed camera-local fore/aft displacement as a fraction of the
			// continuously recentered camera-to-ScopeFade distance. This occupies
			// the former unused padding slot, preserving the cbuffer ABI size.
			float eyeReliefDelta = 0.0F;
			// Reticle shadow and parallax are independent of scene optical
			// effects. A zero shadow strength keeps an authored STS reticle
			// visible even when the exit pupil darkens the scene behind it.
			float reticleShadowStrength = 0.0F;
			float reticleParallaxStrength = 1.0F;
			float lensCenterX = 0.0F;
			float lensCenterY = 0.0F;

			// User placement of the optical assembly inside the authored
			// housing, in aperture radii along the optic's own X/Z axes, plus
			// a multiplier for the lit-image radius. MagnaScope owns this
			// buffer's creation, so the row is appended rather than stolen
			// from an existing field.
			float lensOffsetX = 0.0F;
			float lensOffsetY = 0.0F;
			float lensScale = 1.0F;

			// Breathing sway. The phase is integrated on the game thread so a
			// rate change bends the curve forward from where it already was,
			// instead of jumping the image to wherever a rescaled absolute
			// time happens to land.
			float breathPhase = 0.0F;

			float breathSway = 0.0F;
			float breathDrift = 0.0F;
			float breathFigure = 0.25F;
			float breathHold = 0.0F;

			float breathPupilFollow = 1.0F;
			// Viewport the reticle layer was CAPTURED with, over the viewport
			// the composite renders at. 1.0 when they agree.
			//
			// They stop agreeing whenever the game rasterizes the weapon pass
			// into a sub-rectangle of the render target -- dynamic resolution
			// being the common case. The capture inherits that smaller
			// viewport, so the reticle lands in the private layer at subrect
			// coordinates; the game upscales its own subrect to the output but
			// nothing upscales the private layer, and compositing it 1:1 drew
			// the reticle uniformly scaled toward the top-left corner --
			// measured at 0.60x in one session and 0.66x in another, floating
			// in open air beside the optic.
			float reticleCaptureScaleX = 1.0F;
			float reticleCaptureScaleY = 1.0F;
			// Measured inner-rim over outer-rim ratio of the active aperture
			// annulus. The fill geometry shader derives lens coordinates and
			// fabricates the centre fan from this; it used to hardcode 0.5,
			// and authored ScopeFade rings do not sit at 0.5 -- the measured
			// corpus reads 0.497, which planted every wedge's fabricated apex
			// on a ~3-pixel circle around the true centre instead of one
			// point. Adjacent fan triangles then cracked and overlapped, and
			// each wedge sampled through a slightly different frame: the
			// radial star and kinked edges visible around the lens centre
			// under magnification.
			float apertureInnerRatio = 0.5F;
		};
		static_assert(
			sizeof(ConstBufferData) == 208,
			"ScopeFade constant buffer must match Triangle.hlsli");

	public:
		struct ScopeEffectShaderData
		{
			float camDepth = 1;
			float GameFov = 90;
			float ScopeEffect_Zoom = 1.5F;
			float parallax_Radius = 2.0F;

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

			XMFLOAT3 eyeDirection = { 0, 0, 0 };
			float targetAdjustFov = 0;

			XMFLOAT3 eyeDirectionLerp = { 0, 0, 0 };
			float padding2 = 0;
			XMFLOAT3 eyeTranslationLerp = { 0, 0, 0 };
			float padding3 = 0;
			XMFLOAT3 CurrWeaponPos = { 0, 0, 0 };
			float padding4 = 0;
			XMFLOAT3 CurrRootPos = { 0, 0, 0 };
			float padding5 = 0;
			XMFLOAT4X4 CameraRotation = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

			XMFLOAT2 ScopeScreenPos = { 0, 0 };
			XMFLOAT2 reticle_Offset = { 0, 0 };

			XMFLOAT4X4 projMat;
			XMFLOAT4 rect;

			// Mirrors the FishEye and depth-separation block appended to the HLSL
			// cbuffer. X is the scene distance behind the aperture and Y is the
			// independent exit-pupil shadow distance.
			float FishEyeStrength = 0;
			float FishEyePower = 2;
			XMFLOAT2 scopeDepth = { 1.0F, 1.0F };

			// X is how much of the aperture's screen motion the magnified image
			// declines to follow (0 locked, 1 world-static). Y is fore/aft
			// apparent-size breathing, deliberately independent of scopeDepth.x
			// so lateral parallax can be tuned without camera yaw reading as
			// depth. Z and W pad the block to the 16-byte cbuffer granularity.
			XMFLOAT4 scopeDepthSeparation = { 0.0F, 0.0F, 0.0F, 0.0F };
		};
		static_assert(
			sizeof(ScopeEffectShaderData) == 368,
			"ScopeEffectData must match Triangle.hlsli");

		struct GameConstBuffer
		{
			RE::NiPoint3 virDir;
			RE::NiPoint3 lastVirDir;
			RE::NiPoint3 VirDirLerp;
			RE::NiPoint3 VirTransLerp;
			RE::NiPoint3 weaponPos;
			RE::NiPoint3 rootPos;
			RE::NiPoint3 scopeScreenPos;
			RE::NiMatrix3 camMat;
			RE::NiMatrix3 scopeLocalMat;
			RE::NiMatrix3 scopeWorldMat;
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
			// Physical eye-box telemetry is measured in ScopeFade-local space
			// after Fallout has updated the first-person rig. X and Y are display
			// axes normalized by the projected aperture radius. Keeping them out
			// of ScopeFade's rotating local basis makes the response independent
			// of camera pitch, compass heading, and authored lens roll.
			float eyeOffsetX = 0.0F;
			float eyeOffsetY = 0.0F;
			// Signed change in camera-to-lens distance relative to the settled
			// ADS calibration, normalized by that settled forward distance.
			float eyeReliefDelta = 0.0F;
			// Screen-pixel displacement produced by one local aperture radius
			// along ScopeFade X and Z. These basis vectors preserve tilted or
			// off-center authored optics instead of assuming screen axes.
			float lensBasisXX = 0.0F;
			float lensBasisXY = 0.0F;
			float lensBasisZX = 0.0F;
			float lensBasisZY = 0.0F;
			// The authored aim point in ScopeFade lens coordinates, where 1.0
			// is the outer rim -- the same unit ScopeGeometryFill_GS assigns to
			// the annulus, and the unit the magnify shader's pivot expects.
			//
			// Solved on the game thread in the aperture's own local frame. It
			// used to be recovered here instead, by inverting the basis above,
			// and that basis is not conditioned for inversion: its two columns
			// foreshorten by different amounts as the optic turns and its
			// determinant passes through zero. A measured 39-pixel
			// reticle-to-centre delta came out as 1.2 aperture radii on one
			// frame and 6.6 on another, which pinned the magnification pivot at
			// the rim and flipped it side to side as the player panned. The
			// aperture's local frame is orthonormal up to scale whatever the
			// camera is doing, so it has no such degeneracy.
			float aimLensX = 0.0F;
			float aimLensY = 0.0F;
			bool aimLensValid = false;
			// Continuous [0,1] weight used to form the physical shadow without
			// a one-frame pop when ADS calibration becomes usable.
			float physicalEyeBoxBlend = 0.0F;
			bool physicalEyeBoxReady = false;
			bool trackingReady = false;
		};

		struct PhysicalEyeBoxSample
		{
			float eyeOffsetX = 0.0F;
			float eyeOffsetY = 0.0F;
			float eyeReliefDelta = 0.0F;
			float lensBasisXX = 0.0F;
			float lensBasisXY = 0.0F;
			float lensBasisZX = 0.0F;
			float lensBasisZY = 0.0F;
			// The authored aim point in lens coordinates, rim == 1.0. See
			// LensProjectionSnapshot::aimLensX.
			float aimLensX = 0.0F;
			float aimLensY = 0.0F;
			bool aimLensValid = false;
			float blend = 0.0F;
			// Diagnostic only: magnitude of this frame's aperture screen
			// excursion, so a log line shows how much of the published travel
			// came from the optic actually moving.
			float apertureExcursion = 0.0F;
			// Current projected aperture radius over its settled radius.
			float apertureScaleRatio = 1.0F;
			// Diagnostic only: foreshortening-robust projected radius.
			float apertureProjectedRadius = 0.0F;
			bool valid = false;
		};

		struct ScreenSphereProjection
		{
			RE::NiPoint3 center{};
			float radiusX = 0.0F;
			float radiusY = 0.0F;
			bool valid = false;
		};

		// A ScopeFade-equivalent aperture generated for a scope that ships no
		// authored ScopeFade, sized from the selected mesh's measured vertex
		// radius and placed on its transform.
		//
		// It is deliberately generated in ScopeFade's own topology -- 24
		// segments, an outer ring and an inner ring at exactly half radius --
		// so that ScopeGeometryFill_GS and ScopeGeometryMagnify_PS consume it
		// completely unchanged. A synthesized aperture then looks the same as
		// an authored one by construction rather than by matching two separate
		// implementations against each other.
		//
		// Stored as NDC plus the perspective W each point was divided by. The
		// render thread rebuilds true clip-space positions from that, which is
		// what gives the magnify shader a genuine per-vertex 1/w for its
		// projective centre solve. NDC rather than pixels because the
		// composite may run at a different resolution than the projection.
		static constexpr std::size_t kSynthApertureSegments = 24U;
		// 24 outer then 24 inner, and 48 triangles of three indices -- the
		// exact counts of an authored ScopeFade, because it is the same mesh.
		static constexpr std::size_t kSynthApertureVertexCount =
			kSynthApertureSegments * 2U;
		static constexpr std::size_t kSynthApertureIndexCount =
			kSynthApertureSegments * 6U;
		// The game-format vertex the captured placement path feeds to the
		// game's own vertex shader: half4 position, half2 UV, 4-byte normal,
		// 4-byte tangent. Every candidate mesh measured so far shares this
		// exact 20-byte layout (vertexDesc 0x0001B00000430205).
		static constexpr std::size_t kSynthApertureGameVertexStride = 20U;
		struct SynthesizedApertureRing
		{
			bool valid = false;
			float centerNdcX = 0.0F;
			float centerNdcY = 0.0F;
			float centerW = 0.0F;
			float rimNdcX[kSynthApertureSegments]{};
			float rimNdcY[kSynthApertureSegments]{};
			float rimW[kSynthApertureSegments]{};
		};

		// The synthesized aperture in the source mesh's own model space. The
		// render thread turns this into a ScopeFade-shaped ring and pushes it
		// through the pipeline state captured from that mesh's real draw, so
		// the game's own vertex shader places it -- correct FOV,
		// foreshortening and roll included, with nothing projected CPU-side.
		struct SynthesizedApertureFrame
		{
			bool valid = false;
			// Model space, pre-scale: the captured transform constants carry
			// the node's world transform including scale, exactly as they did
			// for the mesh's own vertices.
			float centerX = 0.0F;
			float centerY = 0.0F;
			float centerZ = 0.0F;
			float radius = 0.0F;
			// 0, 1 or 2: which local axis is the optical axis.
			int opticalAxis = 1;
		};

	public:
		// Game thread only: reads the aperture's transform. Publishes the ring
		// for the render thread, which never follows a scene pointer.
		bool PublishSynthesizedApertureRing(
			RE::NiAVObject* camera,
			RE::NiAVObject* aperture,
			const RE::NiPoint3& worldCenter,
			float worldRadius,
			int opticalAxis,
			float fov);
		static void InvalidateSynthesizedApertureRing() noexcept;
		static SynthesizedApertureRing AcquireSynthesizedApertureRing() noexcept;
		// Game thread: publish the synthesis source mesh's draw identity for
		// the classifier plus the model-space ring frame. The mesh is never
		// suppressed -- it is real geometry that should keep drawing; its draw
		// is only borrowed for one frame's worth of placement state.
		bool PublishSynthesisPlacement(
			RE::NiAVObject* shape,
			const RE::NiPoint3& localCenter,
			float localRadius,
			int opticalAxis);
		static void InvalidateSynthesisPlacement() noexcept;
		static SynthesizedApertureFrame AcquireSynthesizedApertureFrame() noexcept;
		// Render thread, called by the draw classifier with
		// mScopeFadeGeometryMutex already held: snapshot the vertex shader,
		// input layout and transform constants of the matched placement draw.
		bool CaptureSynthesisPlacementLocked(ID3D11DeviceContext* context);

		// Published placement identity, matched the same way as the ScopeFade
		// identity: opaque buffer pointers plus suballocation offsets.
		static std::atomic<std::uintptr_t> synthPlacementVertexBuffer;
		static std::atomic<std::uintptr_t> synthPlacementIndexBuffer;
		static std::atomic_uint32_t synthPlacementIndexCount;
		static std::atomic_uint32_t synthPlacementVertexStride;
		static std::atomic_uint32_t synthPlacementVertexDataOffset;
		static std::atomic_uint32_t synthPlacementIndexDataOffset;

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
			float activationProgress,
			const PhysicalEyeBoxSample& physicalEyeBox);
		void InvalidateLensProjection();
		[[nodiscard]] LensProjectionSnapshot GetLensProjectionSnapshot() const;
		// Publish the exact renderer buffers owned by STS's required
		// ScopeFade. DrawIndexedHook compares only opaque buffer identity and
		// suballocation metadata. It never dereferences a scene-graph object
		// on the render thread.
		void PublishAutomaticSTSGeometry(
			RE::NiAVObject* renderSurface,
			const std::vector<RE::NiAVObject*>& reticleSurfaces,
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
		void UpdateScene(ScopeData::ScopeProfile*);
		void CreateBlender();
		void QueryChangeReticleTexture();
		void ResetZoomDelta();
		void AdjustZoomDelta(float delta);
		void SetZoom(float zoom);
		void ScreenTextureMod();
		// Returns true only when this call accounts for a visible composite.
		// Automatic STS profiles replay only the exact authored ScopeFade
		// geometry and fail
		// closed if that draw was not captured.
		bool RenderToReticleTexture();
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

	public:
		// How many distinct implementations of a single draw entry we are
		// prepared to hook at once. This is headroom, not a measurement: one
		// 2026-08-04 session presented three implementations of DrawIndexed
		// and three of DrawIndexedInstanced, and the addresses differ between
		// runs, so the arity cannot be assumed.
		static constexpr std::size_t kDrawHookSlots = 6;

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
		// One detour per hooked target, because MinHook gives each target its
		// own trampoline and a shared detour cannot tell which one it was
		// entered through. The slot index is a template parameter so the
		// detour/trampoline pairing is fixed at compile time; every
		// instantiation forwards into the same dispatch.
		template <std::size_t Slot>
		static void __stdcall DrawIndexedSlotHook(ID3D11DeviceContext* pContext, UINT IndexCount, UINT StartIndexLocation, INT BaseVertexLocation);
		template <std::size_t Slot>
		static void __stdcall DrawIndexedInstancedSlotHook(
			ID3D11DeviceContext* pContext,
			UINT IndexCountPerInstance,
			UINT InstanceCount,
			UINT StartIndexLocation,
			INT BaseVertexLocation,
			UINT StartInstanceLocation);
		// Detour addresses and trampoline storage, one entry per slot and in
		// the same order. Handing the binder both as plain pointers keeps it a
		// free function rather than something that has to reach into OldFuncs.
		static void* const* DrawIndexedDetourSlots();
		static void** DrawIndexedOriginalSlots();
		static void* const* DrawIndexedInstancedDetourSlots();
		static void** DrawIndexedInstancedOriginalSlots();
		static void DrawIndexedDispatch(
			ID3D11DeviceContext* pContext,
			UINT IndexCount,
			UINT StartIndexLocation,
			INT BaseVertexLocation,
			D3D11DrawIndexedHook original);
		static void DrawIndexedInstancedDispatch(
			ID3D11DeviceContext* pContext,
			UINT IndexCountPerInstance,
			UINT InstanceCount,
			UINT StartIndexLocation,
			INT BaseVertexLocation,
			UINT StartInstanceLocation,
			D3D11DrawIndexedInstancedHook original);
		static bool CreateAndEnableHook(void* target, void* hook, void** original, const char* hookName);

		DWORD __stdcall HookDX11_Init();
		bool CaptureVerificationSource(
			ID3D11Texture2D* sourceTexture,
			const char* anchorName,
			ID3D11Device* captureDevice = nullptr,
			ID3D11DeviceContext* captureContext = nullptr);
		bool InitGeometryProbeEffect();
		bool EnsureGeometryProbeDeviceResources(ID3D11Device* device);
		bool PrepareScopeFadeSceneSource(
			ID3D11DeviceContext* context,
			ID3D11RenderTargetView* renderTarget,
			ID3D11ShaderResourceView* preferredWorldSource = nullptr);
		// Records the exact STS ScopeFade draw while Fallout is rendering the
		// first-person weapon. Once capture succeeds, the caller suppresses the
		// authored lens-color draw so the coherent late source does not contain
		// glass that would be sampled and drawn a second time. This matches
		// MagnaScope's aperture ownership contract.
		bool CaptureAutomaticSTSScopeFadeReplay(
			ID3D11DeviceContext* context,
			UINT indexCount,
			UINT startIndexLocation,
			INT baseVertexLocation,
			ID3D11Buffer* vertexBuffer,
			UINT vertexStride,
			UINT vertexOffset,
			ID3D11Buffer* indexBuffer,
			DXGI_FORMAT indexFormat,
			UINT indexOffset);
		// Replays the captured ScopeFade aperture at a verified late color
		// anchor. The scene source already contains a coherent weapon and world,
		// so increasing magnification cannot expose a depth-only weapon
		// silhouette or unresolved deferred-lighting pixels.
		bool ReplayAutomaticSTSScopeFade(
			ID3D11ShaderResourceView* sceneSource,
			ID3D11RenderTargetView* compositeTarget);
		// The same optical draw for a scope that ships no authored ScopeFade,
		// using a ring generated at the selected mesh's measured radius. It
		// runs the identical fill and magnify shaders, so the result is the
		// same optic rather than a second approximation of one.
		bool DrawSynthesizedAperture(
			ID3D11ShaderResourceView* sceneSource,
			ID3D11RenderTargetView* compositeTarget);
		// Records the exact authored STS reticle pipeline without drawing it
		// into the scene source. Replaying that draw after ScopeFade keeps a 1x
		// reticle independent from scene magnification and preserves black
		// markings that cannot be reconstructed from an RGBA coverage layer.
		bool CaptureAutomaticSTSReticleReplay(
			ID3D11DeviceContext* context,
			UINT indexCount,
			UINT startIndexLocation,
			INT baseVertexLocation);
		bool ReplayAutomaticSTSReticle(
			ID3D11RenderTargetView* compositeTarget);
		void ClearAutomaticSTSReticleReplay() noexcept;
		// Draws the authored STS reticle into a private transparent layer at
		// its original pipeline position. The coherent late scene therefore
		// remains reticle-free, while the layer can be composited after the
		// magnified ScopeFade replay without magnifying the reticle.
		bool BeginAutomaticSTSReticleLayerCapture(
			ID3D11DeviceContext* context,
			ID3D11RenderTargetView* sourceTarget,
			ID3D11DepthStencilView* sourceDepth,
			bool whiteBackground);
		bool ApplyAutomaticSTSReticleColorSuppression(
			ID3D11DeviceContext* context);
		// Serializes the complete black/white capture transaction against late
		// composition, Present cleanup, resize, and device-resource teardown.
		// Callers must hold this lock across both authored draws and Complete.
		std::unique_lock<std::mutex>
			LockAutomaticSTSReticleLayerCapture();
		void CompleteAutomaticSTSReticleLayerCapture(
			bool captured,
			std::uint64_t frameGeneration) noexcept;
		bool CompositeAutomaticSTSReticleLayer(
			ID3D11RenderTargetView* compositeTarget);
		void ClearAutomaticSTSReticleLayer() noexcept;
		void ClearAutomaticSTSScopeFadeReplay() noexcept;

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
			UINT numVertexBuffers, ID3D11RenderTargetView* backBufferRTV);

		void LoadAimTexture(const std::string& path);
		template <typename T>
		void UpdateConstantBuffer(const ComPtr<ID3D11Buffer>& buffer, const T& data);
		void UpdateGameConstants(const GameConstBuffer& src, ScopeEffectShaderData& dst);

		struct OldFuncs
		{
			D3D11PresentHook phookD3D11Present = nullptr;
			ResizeBuffers resizeBuffers = nullptr;
			// Fallout's draw entries move between d3d11's own functions and
			// wrappers other integrations install, so every implementation the
			// slot presents stays hooked at once and each needs its own
			// trampoline. Rebinding to whichever the vtable held cannot win a
			// race that never stops. Index here is the detour's slot index.
			std::array<D3D11DrawIndexedHook, kDrawHookSlots> drawIndexedSlots{};
			std::array<D3D11DrawIndexedInstancedHook, kDrawHookSlots>
				drawIndexedInstancedSlots{};
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
		bool GetRenderState() { return bQueryRender; }
		void SetIsInGame(bool flag) { bIsInGame = flag; }
		GameConstBuffer* GetGameConstBuffer() { return &gameConstBuffer; }

	public:
		static bool bLegacyMode;
		static bool isEnableScopeEffect;
		static std::atomic_bool bEnableEditMode;
		static std::atomic_bool bRefreshChar;
		// While the customization menu is in edit mode these bound the mouse
		// wheel zoom instead of the saved profile values, so the menu's
		// magnification sliders preview live before the profile is saved.
		static float editZoomMin;
		static float editZoomMax;
		// Copied scalar consumed by the exact ScopeFade replacement shader.
		// This avoids sharing game objects with the renderer thread.
		static std::atomic<float> scopeFadeMagnification;
		static std::atomic<float> scopeImageDenoise;
		static std::atomic<float> scopeImageSharpen;
		static std::atomic<float> scopeFishEyeStrength;
		static std::atomic<float> scopeFishEyePower;
		static std::atomic<float> scopeEdgeRefractionStrength;
		static std::atomic<float> scopeEdgeRefractionWidth;
		static std::atomic<float> scopeEdgeChromaticAberration;
		// Independent STS reticle vertex scale. 1 preserves the authored mesh
		// size regardless of scene magnification.
		static std::atomic<float> scopeReticleMagnification;
		static std::atomic<float> scopeReticleSize;
		static std::atomic<float> scopeReticleOffsetX;
		static std::atomic<float> scopeReticleOffsetY;
		static std::atomic<float> scopeReticleShadowStrength;
		static std::atomic<float> scopeReticleParallaxStrength;
		static std::atomic<float> scopeEyeBoxRadius;
		static std::atomic<float> scopeVignetteReach;
		static std::atomic<float> scopeVignetteSharpness;
		static std::atomic<float> scopeEyeBoxMaxTravel;
		static std::atomic<float> scopeSceneParallaxStrength;
		static std::atomic<float> scopeOpticalLagStrength;
		static std::atomic<float> scopeSceneDepth;
		static std::atomic<float> scopeShadowDepth;
		// Fraction of the aperture's screen motion the magnified image
		// declines to follow, fore/aft apparent-size breathing, and how
		// quickly the optic settles back to centre.
		static std::atomic<float> scopeImageStillness;
		static std::atomic<float> scopeAxialBreathing;
		static std::atomic<float> scopeRecenterSpeed;
		// How much player translation drives lens lag relative to turning.
		// Separate from the angular response because the two measure very
		// different quantities and cannot share one gain.
		static std::atomic<float> scopeStrafeLag;
		static std::atomic<float> scopeApertureScaleRatio;
		static std::atomic<float> scopeTubeDepth;
		// Where the sight picture sits inside the authored housing, and how
		// large it is. STS publishes the ScopeFade mesh's own centre and
		// radius, which need not be where a given scope model wants the
		// optical image.
		static std::atomic<float> scopeLensOffsetX;
		static std::atomic<float> scopeLensOffsetY;
		static std::atomic<float> scopeLensScale;
		// Breathing sway. The phase is integrated by the game thread; the rest
		// are authored per scope.
		static std::atomic<float> scopeBreathPhase;
		static std::atomic<float> scopeBreathRate;
		static std::atomic<float> scopeBreathSway;
		static std::atomic<float> scopeBreathDrift;
		static std::atomic<float> scopeBreathFigure;
		static std::atomic<float> scopeBreathHold;
		static std::atomic<float> scopeBreathPupilFollow;
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
		static std::atomic<float> projectedEyeOffsetX;
		static std::atomic<float> projectedEyeOffsetY;
		static std::atomic<float> projectedEyeReliefDelta;
		static std::atomic<float> projectedLensBasisXX;
		static std::atomic<float> projectedLensBasisXY;
		static std::atomic<float> projectedLensBasisZX;
		static std::atomic<float> projectedLensBasisZY;
		// The authored aim point already expressed in ScopeFade lens
		// coordinates, solved in the aperture's own local frame. See
		// LensProjectionSnapshot::aimLensValid for why the basis above cannot
		// be inverted to recover it.
		static std::atomic<float> projectedAimLensX;
		static std::atomic<float> projectedAimLensY;
		static std::atomic_bool projectedAimLensValid;
		// Viewport bound when the reticle layer was captured this frame, in
		// pixels. Zero until a capture has run. Written on the render thread
		// during capture, read on the same thread when the composite fills its
		// constant buffer; atomic because the resolution fill also runs from
		// other composite entry points.
		static std::atomic<float> reticleCaptureViewportWidth;
		static std::atomic<float> reticleCaptureViewportHeight;
		// Measured inner/outer rim ratio of the active aperture annulus,
		// published by the game thread with the rest of the selection. 0.5
		// when unmeasured or when the synthesized ring (which is built at
		// exactly half) is the active geometry.
		static std::atomic<float> scopeApertureInnerRatio;
		static std::atomic<float> projectedPhysicalEyeBoxBlend;
		static std::atomic_bool projectedPhysicalEyeBoxReady;
		static std::atomic_bool projectedTrackingReady;
		// Odd while a game-thread publication is in progress and even when a
		// complete lens/eye snapshot is available. The render thread retries
		// if the value changes, preventing recoil from tearing center, basis,
		// and eye displacement across different frames.
		static std::atomic_uint64_t projectedLensSequence;
		static std::atomic<std::uintptr_t> automaticSTSVertexBuffer;
		static std::atomic<std::uintptr_t> automaticSTSIndexBuffer;
		static std::atomic_uint32_t automaticSTSIndexCount;
		static std::atomic_uint32_t automaticSTSVertexStride;
		static std::atomic_uint32_t automaticSTSVertexDataOffset;
		static std::atomic_uint32_t automaticSTSIndexDataOffset;
		static std::atomic_bool automaticSTSGeometryReady;
		// False when the selected aperture is not a 48-vertex annulus, so the
		// exact replay is impossible rather than merely unavailable this frame.
		// The composite needs that distinction: a transient capture failure
		// must keep the ordinary STS draw, but a permanently ineligible
		// aperture should hand over to the screen-space path instead of
		// leaving the scope with no magnification at all.
		static std::atomic_bool automaticSTSApertureSupportsExactReplay;
		static std::atomic<std::uintptr_t> automaticSTSReticleVertexBuffer;
		static std::atomic<std::uintptr_t> automaticSTSReticleIndexBuffer;
		static std::atomic_uint32_t automaticSTSReticleIndexCount;
		static std::atomic_uint32_t automaticSTSReticleVertexCount;
		static std::atomic_uint32_t automaticSTSReticleVertexStride;
		static std::atomic_uint32_t automaticSTSReticleVertexDataOffset;
		static std::atomic_uint32_t automaticSTSReticleIndexDataOffset;
		static std::atomic_uint64_t automaticSTSReticleVertexDescriptor;
		// Changes whenever the selected reticle identity or packed layout
		// changes. Render-thread caches use this generation rather than
		// retaining any scene-graph pointer across equip/reload boundaries.
		static std::atomic_uint64_t automaticSTSReticleGeometryGeneration;
		static std::atomic_bool automaticSTSReticleGeometryReady;
		// An STS reticle is a subtree, not necessarily one Reticle:0 shape.
		// Each entry is published as opaque D3D identity so DrawIndexed can
		// remove every authored reticle subdraw from the scene capture and
		// reconstruct the complete group exactly once after magnification.
		// Atomic fields plus the odd/even sequence avoid render-thread scene
		// pointers, locks, and torn equipment-change snapshots.
		static constexpr std::size_t kMaxAutomaticSTSReticleGeometries = 32U;
		struct AutomaticSTSReticleGeometryIdentity
		{
			std::atomic<std::uintptr_t> vertexBuffer{ 0U };
			std::atomic<std::uintptr_t> indexBuffer{ 0U };
			std::atomic_uint32_t indexCount{ 0U };
			std::atomic_uint32_t vertexStride{ 0U };
			std::atomic_uint32_t vertexDataOffset{ 0U };
			std::atomic_uint32_t indexDataOffset{ 0U };
		};
		static std::array<
			AutomaticSTSReticleGeometryIdentity,
			kMaxAutomaticSTSReticleGeometries>
			automaticSTSReticleGeometries;
		static std::atomic_uint32_t automaticSTSReticleGeometryCount;
		static std::atomic_uint64_t automaticSTSReticleSetSequence;
		static std::atomic<std::uintptr_t> automaticSTSHousingVertexBuffer;
		static std::atomic<std::uintptr_t> automaticSTSHousingIndexBuffer;
		static std::atomic_uint32_t automaticSTSHousingIndexCount;
		static std::atomic_uint32_t automaticSTSHousingVertexStride;
		static std::atomic_uint32_t automaticSTSHousingVertexDataOffset;
		static std::atomic_uint32_t automaticSTSHousingIndexDataOffset;
		static std::atomic_bool automaticSTSHousingGeometryReady;
		static std::atomic_uint32_t automaticSTSScopeFadeDrawsThisFrame;
		// Draws carrying ScopeFade's exact index count and stride, whatever
		// buffer they came from. Fallout pools unrelated meshes into the same
		// buffers, so a plain near-miss sample is dominated by world geometry
		// and cannot answer the one question that matters: whether See Through
		// Scopes submitted the aperture at all. Zero here every frame means it
		// did not; non-zero means it did and the identity match rejected it.
		static std::atomic_uint32_t automaticSTSScopeFadeShapedDrawsThisFrame;
		// Draws reaching the automatic-profile branch regardless of the
		// readiness gate. Compared against the gated ordinal, this says whether
		// a frame that never sees ScopeFade stopped being delivered draws or
		// stopped being allowed to inspect them.
		static std::atomic_uint32_t automaticSTSObservedDrawsThisFrame;
		static std::atomic_uint32_t
			automaticSTSObservedInstancedDrawsThisFrame;
		// Published at Present from the exact ScopeFade draw count. The game
		// thread uses this previous-frame fact to begin optical blending only
		// after the ScopeAiming branch is genuinely visible.
		static std::atomic_bool automaticSTSScopeFadeVisibleLastFrame;
		// The synthesized aperture's equivalent of the signal above. A matched
		// placement draw is proof the optic rendered this frame, exactly as a
		// matched ScopeFade draw is -- and unlike the CPU-projected ring, it
		// does not depend on a projection that can quietly fail.
		static std::atomic_uint32_t automaticSTSPlacementDrawsThisFrame;
		static std::atomic_bool automaticSTSPlacementVisibleLastFrame;
		static std::atomic_uint32_t automaticSTSReticleDrawsThisFrame;
		static std::atomic_uint32_t automaticSTSHousingDrawsThisFrame;
		static std::atomic_uint32_t
			automaticSTSScopeFadeInstancedDrawsThisFrame;
		static std::atomic_uint32_t
			automaticSTSReticleInstancedDrawsThisFrame;
		static std::atomic_uint32_t
			automaticSTSHousingInstancedDrawsThisFrame;
		// Geometry metadata being ready does not prove that Fallout submitted
		// that geometry with DrawIndexed in the current frame. In particular,
		// DrawIndexedInstanced is observed for diagnostics but is deliberately
		// not shader-replaced. Only the exact replacement path sets this flag,
		// so a missing or unsupported draw falls back to the fullscreen pass.
		static std::atomic_bool
			automaticSTSExactScopeFadeReplacementThisFrame;
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
		ComPtr<ID3D11PixelShader> m_pPixelShader_STSGeometryMagnify;
		ComPtr<ID3D11PixelShader> m_pPixelShader_STSGeometryProbe;
		ComPtr<ID3D11PixelShader> m_outPutPixelShader_Legacy;
		ComPtr<ID3D11VertexShader> m_pVertexShader_Legacy;
		// Compiled ScopeFade bytecode is device independent. Keeping it alive
		// lets the exact DrawIndexed hook instantiate device children on the
		// device returned by that draw context, which is required when ENB or
		// another proxy exposes a different device interface than Fallout's
		// renderer singleton.
		ComPtr<ID3DBlob> mScopeFadeProbePixelBytecode;
		ComPtr<ID3DBlob> mScopeFadeMagnifyPixelBytecode;
		ComPtr<ID3DBlob> mScopeFadeFillGeometryBytecode;
		ComPtr<ID3DBlob> mReticleLayerPixelBytecode;
		// The synthesized aperture's own device children. Its vertex buffer is
		// rewritten every composite because the ring is reprojected every
		// frame; the index buffer never changes, because the topology it
		// describes is ScopeFade's and that is the whole point.
		ComPtr<ID3DBlob> mScopeApertureSynthVertexBytecode;
		ComPtr<ID3D11VertexShader> m_pVertexShader_ApertureSynth;
		ComPtr<ID3D11InputLayout> mApertureSynthInputLayout;
		ComPtr<ID3D11Buffer> mApertureSynthVertexBuffer;
		ComPtr<ID3D11Buffer> mApertureSynthIndexBuffer;
		// The composite pass's rasterizer state, built once instead of per
		// draw. The device it was created on is retained alongside it so an
		// upscaler or frame-generation proxy swapping the device rebuilds it
		// rather than binding a state that belongs to a dead device.
		ComPtr<ID3D11RasterizerState> mCompositeRasterizerState;
		ComPtr<ID3D11Device> mCompositeRasterizerDevice;
		ComPtr<ID3D11Device> mScopeFadeResourceDevice;
		std::atomic_uint64_t mScopeFadeResourceGeneration{ 1U };
		ComPtr<ID3D11Buffer> mScopeFadeResolutionBuffer;
		ComPtr<ID3D11SamplerState> mScopeFadeSampler;
		// DrawIndexed can be reached through render wrappers that expose
		// different device interfaces over time. Serialize the exact
		// ScopeFade substitution so a device-resource rebind cannot race a
		// draw that is still using the previous device children.
		std::mutex mScopeFadeGeometryMutex;

		struct AutomaticSTSScopeFadeReplay
		{
			std::uint64_t generation = 0U;
			std::uint64_t resourceGeneration = 0U;
			ComPtr<ID3D11VertexShader> vertexShader;
			ComPtr<ID3D11InputLayout> inputLayout;
			ComPtr<ID3D11Buffer> vertexBuffer;
			ComPtr<ID3D11Buffer> indexBuffer;
			std::array<ComPtr<ID3D11Buffer>, 3> vertexConstantBuffers;
			UINT vertexStride = 0;
			UINT vertexOffset = 0;
			DXGI_FORMAT indexFormat = DXGI_FORMAT_UNKNOWN;
			UINT indexOffset = 0;
			UINT indexCount = 0;
			UINT startIndexLocation = 0;
			INT baseVertexLocation = 0;
			D3D11_PRIMITIVE_TOPOLOGY topology =
				D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
			bool ready = false;
		};
		AutomaticSTSScopeFadeReplay mAutomaticSTSScopeFadeReplay;
		// Placement state captured from the synthesis source mesh's own draw.
		// Same packet shape as the ScopeFade replay, but its vertex/index
		// buffers are unused: the synthesized ring supplies its own geometry
		// and only borrows the shader, layout and transform constants.
		AutomaticSTSScopeFadeReplay mAutomaticSTSPlacementReplay;

		struct AutomaticSTSReticleReplay
		{
			std::uint64_t generation = 0U;
			std::uint64_t resourceGeneration = 0U;
			ComPtr<ID3D11VertexShader> vertexShader;
			ComPtr<ID3D11GeometryShader> geometryShader;
			ComPtr<ID3D11PixelShader> pixelShader;
			ComPtr<ID3D11InputLayout> inputLayout;
			ComPtr<ID3D11Buffer> vertexBuffer;
			ComPtr<ID3D11Buffer> indexBuffer;
			std::array<ComPtr<ID3D11Buffer>, 14> vertexConstantBuffers;
			std::array<ComPtr<ID3D11Buffer>, 14> pixelConstantBuffers;
			std::array<ComPtr<ID3D11ShaderResourceView>, 16>
				vertexShaderResources;
			std::array<ComPtr<ID3D11ShaderResourceView>, 16>
				pixelShaderResources;
			std::array<ComPtr<ID3D11SamplerState>, 16> vertexSamplers;
			std::array<ComPtr<ID3D11SamplerState>, 16> pixelSamplers;
			ComPtr<ID3D11RasterizerState> rasterizerState;
			ComPtr<ID3D11BlendState> blendState;
			ComPtr<ID3D11DepthStencilState> depthDisabledState;
			std::array<D3D11_VIEWPORT,
				D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE>
				viewports{};
			std::array<D3D11_RECT,
				D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE>
				scissorRects{};
			UINT viewportCount = 0U;
			UINT scissorCount = 0U;
			std::array<float, 4> blendFactor{};
			UINT sampleMask = 0xFFFFFFFFU;
			UINT vertexStride = 0U;
			UINT vertexOffset = 0U;
			DXGI_FORMAT indexFormat = DXGI_FORMAT_UNKNOWN;
			UINT indexOffset = 0U;
			UINT indexCount = 0U;
			UINT startIndexLocation = 0U;
			INT baseVertexLocation = 0;
			D3D11_PRIMITIVE_TOPOLOGY topology =
				D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
			bool ready = false;
		};
		AutomaticSTSReticleReplay mAutomaticSTSReticleReplay;
		std::mutex mAutomaticSTSReticleReplayMutex;

		// The paired reticle layers contain the authored reticle draw over
		// black and white backgrounds. Their difference describes how the
		// original authored blend attenuates the destination, so the late
		// composite can reproduce transparent black backing, black line work,
		// and emissive dots without guessing the reticle texture's alpha mode.
		// Both layers are recreated with the active render-target dimensions
		// and format, and are valid for exactly one presented frame.
		ComPtr<ID3D11Texture2D> mAutomaticSTSReticleLayerTexture;
		ComPtr<ID3D11RenderTargetView> mAutomaticSTSReticleLayerRTV;
		ComPtr<ID3D11ShaderResourceView> mAutomaticSTSReticleLayerSRV;
		ComPtr<ID3D11Texture2D> mAutomaticSTSReticleLayerWhiteTexture;
		ComPtr<ID3D11RenderTargetView> mAutomaticSTSReticleLayerWhiteRTV;
		ComPtr<ID3D11ShaderResourceView> mAutomaticSTSReticleLayerWhiteSRV;
		ComPtr<ID3D11PixelShader> m_pPixelShader_STSReticleLayer;
		ComPtr<ID3D11BlendState> mAutomaticSTSReticleLayerCompositeBlend;
		// After private black/white capture, replay the authored draw once with
		// color target zero disabled. That retains its depth, stencil, and any
		// auxiliary MRT side effects while preventing the reticle from entering
		// the scene image that receives optical magnification.
		ComPtr<ID3D11BlendState> mAutomaticSTSReticleSuppressionSourceBlend;
		ComPtr<ID3D11BlendState> mAutomaticSTSReticleColorSuppressionBlend;
		bool mAutomaticSTSReticleSuppressionSourceWasNull = false;
		// Reticle shaders are captured twice against black and white. Replaying
		// either authored draw against the live depth buffer must not consume or
		// mutate depth/stencil state before the game's ordinary frame continues.
		// Cache a write-disabled clone of the authored state while preserving all
		// of its comparison functions and the caller's stencil reference.
		ComPtr<ID3D11DepthStencilState>
			mAutomaticSTSReticleLayerReadOnlyDepthState;
		std::mutex mAutomaticSTSReticleLayerMutex;
		std::uint64_t mAutomaticSTSReticleLayerCaptureGeneration = 0U;
		std::uint64_t mAutomaticSTSReticleLayerGeneration = 0U;
		std::uint64_t mAutomaticSTSReticleLayerResourceGeneration = 0U;
		bool mAutomaticSTSReticleLayerReady = false;

		ComPtr<ID3D11Texture2D> m_pDepthStencilBuffer;
		ComPtr<ID3D11RenderTargetView> m_pRenderTargetView;
		ComPtr<ID3D11DepthStencilView> m_pDepthStencilView;
		ComPtr<ID3D11SamplerState> m_pSamplerState;

		ComPtr<ID3D11BlendState> BSAlphaToCoverage;
		ComPtr<ID3D11BlendState> BSTransparent;
		// Exact ScopeFade replacement writes RGB without blending it over the
		// already-rendered 1x destination, while leaving destination alpha
		// untouched for ENB/upscaler metadata.
		ComPtr<ID3D11BlendState> BSScopeFadeReplaceRGB;

		ComPtr<ID3D11Texture2D> mTextDDS;
		ComPtr<ID3D11Resource> mTextDDS_Res;

		ComPtr<ID3D11Texture2D> mBackBuffer;
		ComPtr<ID3D11ShaderResourceView> mTextDDS_SRV;
		ComPtr<ID3D11ShaderResourceView> mShaderResourceView;
		// A private copy of the color target as it exists immediately before
		// ScopeFade draws. The copy prevents the D3D11 read/write hazard that
		// would occur if the geometry pixel shader sampled the active RTV.
		ComPtr<ID3D11Texture2D> mScopeFadeSceneTexture;
		ComPtr<ID3D11ShaderResourceView> mScopeFadeSceneSRV;

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
