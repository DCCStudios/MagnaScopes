#include "PCH.h"

#include "WorldOnlyScopeRenderer.h"

#include <cstring>
#include <d3d11_4.h>
#include <iomanip>
#include <MinHook.h>
#include <sstream>
#include <wrl/client.h>

namespace MagnaScope
{
	namespace
	{
		using Microsoft::WRL::ComPtr;

		using RenderPreUIFn = void(__fastcall*)(std::uint64_t);
		using RenderBatchesFn =
			void(__fastcall*)(RE::BSShaderAccumulator*, int, bool, int);
		using DoZPrePassFn = void(__fastcall*)(
			std::uint64_t,
			RE::NiCamera*,
			RE::NiCamera*,
			float,
			float,
			float,
			float);
		using DoUmbraQueryFn = void(__fastcall*)(std::uint64_t);

		// These IDs and signatures are OG-only. They were re-derived from the
		// local True See Through Scopes implementation and checked against the
		// current CommonLibF4 declarations before being copied here. They must
		// never be resolved on NG/AE.
		constexpr std::uint64_t kRenderPreUIID = 984743;
		constexpr std::uint64_t kRenderBatchesID = 1048494;
		constexpr std::uint64_t kDoZPrePassID = 1491502;
		constexpr std::uint64_t kDoUmbraQueryID = 1264353;
		constexpr std::uint64_t kRenderZPrePassID = 901559;
		constexpr std::uint64_t kFirstPersonAccumulatorID = 1430301;
		constexpr std::uint64_t kFirstPersonZCountID = 163482;
		constexpr std::uint64_t kFirstPersonAlphaZCountID = 382658;

		// Address Library 1.10.163 corpus values. Requiring both the ID lookup
		// and exact loaded RVA catches a wrong database, a neighboring-ID
		// lookup, and accidental use on a different executable revision.
		constexpr std::uintptr_t kRenderPreUIRVA = 0x2857480;
		constexpr std::uintptr_t kRenderBatchesRVA = 0x282EF70;
		constexpr std::uintptr_t kDoZPrePassRVA = 0x1D12980;
		constexpr std::uintptr_t kDoUmbraQueryRVA = 0x284F1D0;
		constexpr std::uintptr_t kRenderZPrePassRVA = 0x1D1B730;
		constexpr std::uintptr_t kFirstPersonAccumulatorRVA = 0x6723358;
		constexpr std::uintptr_t kFirstPersonZCountRVA = 0x609DBDC;
		constexpr std::uintptr_t kFirstPersonAlphaZCountRVA = 0x609DBE4;

		constexpr std::size_t kLoggedPrologueBytes = 16;
		constexpr std::array<std::uint8_t, kLoggedPrologueBytes>
			kRenderPreUIPrologue{
				0x4C, 0x8B, 0xDC, 0x56, 0x48, 0x83, 0xEC, 0x70,
				0x0F, 0xB6, 0x05, 0xFD, 0x1E, 0x07, 0x01, 0x33
			};
		constexpr std::array<std::uint8_t, kLoggedPrologueBytes>
			kRenderBatchesPrologue{
				0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
				0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57
			};
		constexpr std::array<std::uint8_t, kLoggedPrologueBytes>
			kDoZPrePassPrologue{
				0x48, 0x8B, 0xC4, 0x55, 0x53, 0x41, 0x57, 0x48,
				0x8D, 0x6C, 0x24, 0x90, 0x48, 0x81, 0xEC, 0x70
			};
		constexpr std::array<std::uint8_t, kLoggedPrologueBytes>
			kDoUmbraQueryPrologue{
				0x48, 0x81, 0xEC, 0x98, 0x00, 0x00, 0x00, 0xE8,
				0x54, 0xAC, 0xFB, 0xFF, 0x84, 0xC0, 0x0F, 0x84
			};

		constexpr std::size_t kMainColorTarget = 4;
		constexpr std::size_t kMainDepthTarget = 2;

		constexpr std::array<std::size_t, 4> kInitialColorClearTargets{
			0, 1, 2, 3
		};
		constexpr std::array<std::size_t, 15> kGBufferClearTargets{
			5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19
		};

		// Render_PreUI touches these RTs while constructing the deferred and
		// forward frame. RT4/DS2 are privately substituted. Every real target
		// that we explicitly clear, plus the additional targets known to be
		// mutated by the TTS second pass, is copied out and restored.
		constexpr std::array<std::size_t, 29> kScratchColorTargets{
			0, 1, 2, 3,
			5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
			20, 22, 23, 24, 28, 29, 39, 57, 58, 59
		};
		constexpr std::array<std::size_t, 1> kScratchDepthTargets{ 8 };

		template <std::size_t NeedleCount, std::size_t HaystackCount>
		consteval bool ContainsEvery(
			const std::array<std::size_t, NeedleCount>& needles,
			const std::array<std::size_t, HaystackCount>& haystack)
		{
			for (const auto needle : needles) {
				bool found = false;
				for (const auto candidate : haystack) {
					found |= candidate == needle;
				}
				if (!found) {
					return false;
				}
			}
			return true;
		}

		static_assert(
			ContainsEvery(
				kInitialColorClearTargets,
				kScratchColorTargets) &&
				ContainsEvery(kGBufferClearTargets, kScratchColorTargets),
			"Every real color target cleared by the auxiliary pass must be backed up");
		static_assert(
			kScratchDepthTargets[0] == 8,
			"The reversed-Z shadow depth target must remain in the backup set");
		static_assert(
			[] {
				for (const auto index : kScratchColorTargets) {
					if (index == kMainColorTarget || index >= 101) {
						return false;
					}
				}
				return true;
			}(),
			"RT4 is private and must not be copied through the real scratch set");

		thread_local bool g_auxiliaryPassActive = false;
		thread_local WorldOnlyScopeRenderer::Phase g_renderPhase =
			WorldOnlyScopeRenderer::Phase::kIdle;
		thread_local std::uint64_t g_renderPhaseGeneration = 0;
		thread_local std::uint32_t g_firstPersonCaptureArmDepth = 0;
		thread_local std::uint64_t g_firstPersonCaptureFrame = 0;

		struct TextureIdentity
		{
			ID3D11Texture2D* pointer = nullptr;  // Identity only; never dereferenced.
			D3D11_TEXTURE2D_DESC description{};
		};

		template <class View, class Description>
		struct ViewIdentity
		{
			View* pointer = nullptr;  // Identity only; never dereferenced.
			Description description{};
		};

		struct ColorTargetIdentity
		{
			TextureIdentity texture;
			TextureIdentity copyTexture;
			ViewIdentity<ID3D11RenderTargetView, D3D11_RENDER_TARGET_VIEW_DESC>
				renderTargetView;
			ViewIdentity<
				ID3D11ShaderResourceView,
				D3D11_SHADER_RESOURCE_VIEW_DESC>
				shaderResourceView;
			ViewIdentity<
				ID3D11ShaderResourceView,
				D3D11_SHADER_RESOURCE_VIEW_DESC>
				copyShaderResourceView;
			ViewIdentity<
				ID3D11UnorderedAccessView,
				D3D11_UNORDERED_ACCESS_VIEW_DESC>
				unorderedAccessView;
		};

		struct DepthTargetIdentity
		{
			TextureIdentity texture;
			std::array<
				ViewIdentity<
					ID3D11DepthStencilView,
					D3D11_DEPTH_STENCIL_VIEW_DESC>,
				4>
				depthViews;
			std::array<
				ViewIdentity<
					ID3D11DepthStencilView,
					D3D11_DEPTH_STENCIL_VIEW_DESC>,
				4>
				readOnlyDepthViews;
			std::array<
				ViewIdentity<
					ID3D11DepthStencilView,
					D3D11_DEPTH_STENCIL_VIEW_DESC>,
				4>
				readOnlyStencilViews;
			std::array<
				ViewIdentity<
					ID3D11DepthStencilView,
					D3D11_DEPTH_STENCIL_VIEW_DESC>,
				4>
				readOnlyDepthStencilViews;
			ViewIdentity<
				ID3D11ShaderResourceView,
				D3D11_SHADER_RESOURCE_VIEW_DESC>
				depthShaderResourceView;
			ViewIdentity<
				ID3D11ShaderResourceView,
				D3D11_SHADER_RESOURCE_VIEW_DESC>
				stencilShaderResourceView;
		};

		struct PrivateRenderTarget
		{
			ComPtr<ID3D11Texture2D> texture;
			ComPtr<ID3D11Texture2D> copyTexture;
			ComPtr<ID3D11RenderTargetView> renderTargetView;
			ComPtr<ID3D11ShaderResourceView> shaderResourceView;
			ComPtr<ID3D11ShaderResourceView> copyShaderResourceView;
			ComPtr<ID3D11UnorderedAccessView> unorderedAccessView;

			ColorTargetIdentity sourceIdentity;
			bool valid = false;

			void Reset() noexcept
			{
				*this = {};
			}
		};

		struct PrivateDepthTarget
		{
			ComPtr<ID3D11Texture2D> texture;
			std::array<ComPtr<ID3D11DepthStencilView>, 4> depthViews;
			std::array<ComPtr<ID3D11DepthStencilView>, 4> readOnlyDepthViews;
			std::array<ComPtr<ID3D11DepthStencilView>, 4> readOnlyStencilViews;
			std::array<ComPtr<ID3D11DepthStencilView>, 4>
				readOnlyDepthStencilViews;
			ComPtr<ID3D11ShaderResourceView> depthShaderResourceView;
			ComPtr<ID3D11ShaderResourceView> stencilShaderResourceView;

			DepthTargetIdentity sourceIdentity;
			bool valid = false;

			void Reset() noexcept
			{
				*this = {};
			}
		};

		struct TextureBackup
		{
			// Retaining the source prevents a renderer rebuild from freeing it
			// between validation and the paired restore copy.
			ComPtr<ID3D11Texture2D> source;
			ComPtr<ID3D11Texture2D> backup;
		};

		struct State
		{
			std::atomic_bool requested{ false };
			std::atomic_bool installed{ false };
			std::atomic_bool passThroughOnly{ false };
			// Stage 5c-a is still forwarding-only. This flag adds observation
			// around the original calls without activating any dormant
			// auxiliary-render mutation.
			std::atomic_bool observationOnly{ false };
			std::atomic_bool outputReady{ false };
			std::atomic_uint64_t generation{ 0 };
			std::atomic_bool permanentlyDisabled{ false };
			std::atomic<WorldOnlyScopeRenderer::Phase> phase{
				WorldOnlyScopeRenderer::Phase::kIdle
			};
			std::atomic_bool invocationActive{ false };
			// Functional Stage 5c uses the ordinary frame only. Render_PreUI
			// snapshots the engine's first-person accumulator, RenderBatches
			// opens this small admission window around the matching original
			// call, and the exact D3D hook claims the first submitted draw.
			std::atomic<RE::BSShaderAccumulator*>
				primaryFirstPersonAccumulator{ nullptr };
			std::atomic_uint64_t captureFrameGeneration{ 0 };
			std::atomic_bool firstPersonCaptureClaimed{ false };
			// High bit opens admission to the auxiliary callback epoch. The
			// remaining bits are references held by lower-level callbacks.
			// Closing the high bit before waiting prevents a late callback from
			// entering after the owner observes a zero count.
			std::atomic_uint64_t auxiliaryCallbackGate{ 0 };
			std::atomic<RE::BSShaderAccumulator*>
				auxiliaryFirstPersonAccumulator{ nullptr };
			std::mutex resourceMutex;

			RenderPreUIFn renderPreUIOriginal = nullptr;
			RenderBatchesFn renderBatchesOriginal = nullptr;
			DoZPrePassFn doZPrePassOriginal = nullptr;
			DoUmbraQueryFn doUmbraQueryOriginal = nullptr;
			void* renderPreUITarget = nullptr;
			void* renderBatchesTarget = nullptr;
			void* doZPrePassTarget = nullptr;
			void* doUmbraQueryTarget = nullptr;

			std::atomic_uint64_t renderPreUIForwarded{ 0 };
			std::atomic_uint64_t renderBatchesForwarded{ 0 };
			std::atomic_uint64_t doZPrePassForwarded{ 0 };
			std::atomic_uint64_t doUmbraQueryForwarded{ 0 };

			RE::BSShaderAccumulator** firstPersonAccumulator = nullptr;
			std::uint32_t* firstPersonZCount = nullptr;
			std::uint32_t* firstPersonAlphaZCount = nullptr;

			// Observation state is intentionally scalar/atomic. RenderBatches
			// and DoZPrePass are known to execute on worker threads, so the
			// hooks must never share a mutable container or perform allocation
			// while recording a callback.
			std::atomic_uint64_t observationGeneration{ 0 };
			// Packed as (generation << 2) | phase. One acquire-load gives a
			// worker a coherent generation and owner phase.
			//   0: no observation has begun
			//   1: inside Render_PreUI
			//   2: Render_PreUI returned; post-return window
			std::atomic_uint64_t observationToken{ 0 };
			std::atomic_uint32_t observationRenderPreUIInFlight{ 0 };
			std::atomic_uint32_t observationLowerInFlight{ 0 };
			std::atomic_uint32_t observationRenderBatchesInFlight{ 0 };
			std::atomic_uint32_t observationZPrePassInFlight{ 0 };
			std::atomic_uint32_t observationUmbraInFlight{ 0 };
			std::atomic_uint32_t observationMaxLowerInFlight{ 0 };
			std::atomic_uint64_t observationOverlappingPrimary{ 0 };
			std::atomic_uint64_t observationLowerInFlightAtReturn{ 0 };
			std::atomic_uint64_t observationPostReturnCallbacks{ 0 };
			std::atomic_uint64_t observationCompletedAfterOwnerReturn{ 0 };
			std::atomic_uint64_t observationRenderBatchesCalls{ 0 };
			std::atomic_uint64_t observationFirstPersonBatchMatches{ 0 };
			std::atomic_uint64_t observationZPrePassCalls{ 0 };
			std::atomic_uint64_t observationUmbraCalls{ 0 };
			std::atomic_uint64_t observationInvalidAccumulatorSnapshots{ 0 };
			std::atomic_uint64_t observationImplausibleZCounts{ 0 };
			std::atomic_uint64_t observationOwnerThreadChanges{ 0 };
			std::atomic_uint64_t observationOwnerThreadCallbacks{ 0 };
			std::atomic_uint64_t observationWorkerThreadCallbacks{ 0 };
			std::array<std::atomic_uint32_t, 8>
				observationCallbackThreads{};
			std::atomic_uint64_t observationCallbackThreadOverflow{ 0 };
			std::atomic_uint64_t observationRT4IdentityChanges{ 0 };
			std::atomic_uint64_t observationDS2IdentityChanges{ 0 };
			std::atomic_uint32_t observationOwnerThread{ 0 };
			std::atomic_uint32_t observationRenderBatchesThread{ 0 };
			std::atomic_uint32_t observationZPrePassThread{ 0 };
			std::atomic_uint32_t observationUmbraThread{ 0 };
			std::atomic<RE::BSShaderAccumulator*>
				observationFirstPersonAccumulator{ nullptr };
			std::atomic<void*> observationRT4Identity{ nullptr };
			std::atomic<void*> observationDS2Identity{ nullptr };
			std::atomic_uint32_t observationLastOpaqueZCount{ 0 };
			std::atomic_uint32_t observationLastAlphaZCount{ 0 };

			ID3D11Device* deviceIdentity = nullptr;  // Borrowed.
			PrivateRenderTarget privateColor;
			PrivateDepthTarget privateDepth;
			std::vector<TextureBackup> scratchBackups;
			std::array<ColorTargetIdentity, kScratchColorTargets.size()>
				scratchColorIdentities;
			std::array<DepthTargetIdentity, kScratchDepthTargets.size()>
				scratchDepthIdentities;
			bool scratchIdentitiesValid = false;
		};

		State& GetState() noexcept
		{
			static State state;
			return state;
		}

		[[nodiscard]] bool SameDescription(
			const D3D11_TEXTURE2D_DESC& left,
			const D3D11_TEXTURE2D_DESC& right) noexcept
		{
			return left.Width == right.Width &&
			       left.Height == right.Height &&
			       left.MipLevels == right.MipLevels &&
			       left.ArraySize == right.ArraySize &&
			       left.Format == right.Format &&
			       left.SampleDesc.Count == right.SampleDesc.Count &&
			       left.SampleDesc.Quality == right.SampleDesc.Quality &&
			       left.Usage == right.Usage &&
			       left.BindFlags == right.BindFlags &&
			       left.CPUAccessFlags == right.CPUAccessFlags &&
			       left.MiscFlags == right.MiscFlags;
		}

		[[nodiscard]] bool CopyResourceCompatible(
			const D3D11_TEXTURE2D_DESC& left,
			const D3D11_TEXTURE2D_DESC& right) noexcept
		{
			return left.Width == right.Width &&
			       left.Height == right.Height &&
			       left.MipLevels == right.MipLevels &&
			       left.ArraySize == right.ArraySize &&
			       left.Format == right.Format &&
			       left.SampleDesc.Count == right.SampleDesc.Count &&
			       left.SampleDesc.Quality == right.SampleDesc.Quality;
		}

		template <class Description>
		[[nodiscard]] bool SameViewDescription(
			const Description& left,
			const Description& right) noexcept
		{
			return std::memcmp(&left, &right, sizeof(Description)) == 0;
		}

		[[nodiscard]] TextureIdentity CaptureTextureIdentity(
			ID3D11Texture2D* texture) noexcept
		{
			TextureIdentity identity;
			identity.pointer = texture;
			if (texture) {
				texture->GetDesc(&identity.description);
			}
			return identity;
		}

		template <class View, class Description>
		[[nodiscard]] ViewIdentity<View, Description> CaptureViewIdentity(
			View* view) noexcept
		{
			ViewIdentity<View, Description> identity;
			identity.pointer = view;
			if (view) {
				view->GetDesc(&identity.description);
			}
			return identity;
		}

		[[nodiscard]] bool SameTextureIdentity(
			const TextureIdentity& left,
			const TextureIdentity& right) noexcept
		{
			return left.pointer == right.pointer &&
			       SameDescription(left.description, right.description);
		}

		template <class View, class Description>
		[[nodiscard]] bool SameViewIdentity(
			const ViewIdentity<View, Description>& left,
			const ViewIdentity<View, Description>& right) noexcept
		{
			return left.pointer == right.pointer &&
			       SameViewDescription(left.description, right.description);
		}

		[[nodiscard]] bool CaptureColorTargetIdentity(
			const RE::BSGraphics::RenderTarget& target,
			ColorTargetIdentity& identity) noexcept
		{
			auto* texture =
				reinterpret_cast<ID3D11Texture2D*>(target.texture);
			auto* copyTexture =
				reinterpret_cast<ID3D11Texture2D*>(target.copyTexture);
			const bool hasPrimaryView =
				target.rtView || target.srView || target.uaView;
			if ((hasPrimaryView && !texture) ||
				(target.copySRView && !copyTexture)) {
				return false;
			}

			identity.texture = CaptureTextureIdentity(texture);
			identity.copyTexture = CaptureTextureIdentity(copyTexture);
			identity.renderTargetView =
				CaptureViewIdentity<
					ID3D11RenderTargetView,
					D3D11_RENDER_TARGET_VIEW_DESC>(
					reinterpret_cast<ID3D11RenderTargetView*>(
						target.rtView));
			identity.shaderResourceView =
				CaptureViewIdentity<
					ID3D11ShaderResourceView,
					D3D11_SHADER_RESOURCE_VIEW_DESC>(
					reinterpret_cast<ID3D11ShaderResourceView*>(
						target.srView));
			identity.copyShaderResourceView =
				CaptureViewIdentity<
					ID3D11ShaderResourceView,
					D3D11_SHADER_RESOURCE_VIEW_DESC>(
					reinterpret_cast<ID3D11ShaderResourceView*>(
						target.copySRView));
			identity.unorderedAccessView =
				CaptureViewIdentity<
					ID3D11UnorderedAccessView,
					D3D11_UNORDERED_ACCESS_VIEW_DESC>(
					reinterpret_cast<ID3D11UnorderedAccessView*>(
						target.uaView));
			return true;
		}

		[[nodiscard]] bool SameColorTargetIdentity(
			const ColorTargetIdentity& left,
			const ColorTargetIdentity& right) noexcept
		{
			return SameTextureIdentity(left.texture, right.texture) &&
			       SameTextureIdentity(left.copyTexture, right.copyTexture) &&
			       SameViewIdentity(
					   left.renderTargetView,
					   right.renderTargetView) &&
			       SameViewIdentity(
					   left.shaderResourceView,
					   right.shaderResourceView) &&
			       SameViewIdentity(
					   left.copyShaderResourceView,
					   right.copyShaderResourceView) &&
			       SameViewIdentity(
					   left.unorderedAccessView,
					   right.unorderedAccessView);
		}

		[[nodiscard]] bool CaptureDepthTargetIdentity(
			const RE::BSGraphics::DepthStencilTarget& target,
			DepthTargetIdentity& identity) noexcept
		{
			auto* texture =
				reinterpret_cast<ID3D11Texture2D*>(target.texture);
			bool hasAnyView = target.srViewDepth || target.srViewStencil;
			for (std::size_t index = 0; index < 4; ++index) {
				hasAnyView |=
					target.dsView[index] ||
					target.dsViewReadOnlyDepth[index] ||
					target.dsViewReadOnlyStencil[index] ||
					target.dsViewReadOnlyDepthStencil[index];
			}
			if (hasAnyView && !texture) {
				return false;
			}

			identity.texture = CaptureTextureIdentity(texture);
			for (std::size_t index = 0; index < 4; ++index) {
				identity.depthViews[index] =
					CaptureViewIdentity<
						ID3D11DepthStencilView,
						D3D11_DEPTH_STENCIL_VIEW_DESC>(
						reinterpret_cast<ID3D11DepthStencilView*>(
							target.dsView[index]));
				identity.readOnlyDepthViews[index] =
					CaptureViewIdentity<
						ID3D11DepthStencilView,
						D3D11_DEPTH_STENCIL_VIEW_DESC>(
						reinterpret_cast<ID3D11DepthStencilView*>(
							target.dsViewReadOnlyDepth[index]));
				identity.readOnlyStencilViews[index] =
					CaptureViewIdentity<
						ID3D11DepthStencilView,
						D3D11_DEPTH_STENCIL_VIEW_DESC>(
						reinterpret_cast<ID3D11DepthStencilView*>(
							target.dsViewReadOnlyStencil[index]));
				identity.readOnlyDepthStencilViews[index] =
					CaptureViewIdentity<
						ID3D11DepthStencilView,
						D3D11_DEPTH_STENCIL_VIEW_DESC>(
						reinterpret_cast<ID3D11DepthStencilView*>(
							target.dsViewReadOnlyDepthStencil[index]));
			}
			identity.depthShaderResourceView =
				CaptureViewIdentity<
					ID3D11ShaderResourceView,
					D3D11_SHADER_RESOURCE_VIEW_DESC>(
					reinterpret_cast<ID3D11ShaderResourceView*>(
						target.srViewDepth));
			identity.stencilShaderResourceView =
				CaptureViewIdentity<
					ID3D11ShaderResourceView,
					D3D11_SHADER_RESOURCE_VIEW_DESC>(
					reinterpret_cast<ID3D11ShaderResourceView*>(
						target.srViewStencil));
			return true;
		}

		[[nodiscard]] bool SameDepthTargetIdentity(
			const DepthTargetIdentity& left,
			const DepthTargetIdentity& right) noexcept
		{
			if (!SameTextureIdentity(left.texture, right.texture) ||
				!SameViewIdentity(
					left.depthShaderResourceView,
					right.depthShaderResourceView) ||
				!SameViewIdentity(
					left.stencilShaderResourceView,
					right.stencilShaderResourceView)) {
				return false;
			}
			for (std::size_t index = 0; index < 4; ++index) {
				if (!SameViewIdentity(
						left.depthViews[index],
						right.depthViews[index]) ||
					!SameViewIdentity(
						left.readOnlyDepthViews[index],
						right.readOnlyDepthViews[index]) ||
					!SameViewIdentity(
						left.readOnlyStencilViews[index],
						right.readOnlyStencilViews[index]) ||
					!SameViewIdentity(
						left.readOnlyDepthStencilViews[index],
						right.readOnlyDepthStencilViews[index])) {
					return false;
				}
			}
			return true;
		}

		struct LoadedSection
		{
			std::string name;
			DWORD characteristics = 0;
			bool found = false;
		};

		[[nodiscard]] LoadedSection FindLoadedSection(
			HMODULE module,
			std::uintptr_t address) noexcept
		{
			LoadedSection result;
			if (!module || address < reinterpret_cast<std::uintptr_t>(module)) {
				return result;
			}

			const auto* dos =
				reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
			if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
				return result;
			}
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
				reinterpret_cast<const std::uint8_t*>(module) +
				dos->e_lfanew);
			if (nt->Signature != IMAGE_NT_SIGNATURE ||
				nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
				return result;
			}

			const auto rva =
				address - reinterpret_cast<std::uintptr_t>(module);
			const auto* sections = IMAGE_FIRST_SECTION(nt);
			for (std::uint16_t index = 0;
				index < nt->FileHeader.NumberOfSections;
				++index) {
				const auto& section = sections[index];
				const auto sectionSize = std::max(
					static_cast<std::uintptr_t>(
						section.Misc.VirtualSize),
					static_cast<std::uintptr_t>(
						section.SizeOfRawData));
				const auto sectionBegin =
					static_cast<std::uintptr_t>(
						section.VirtualAddress);
				if (rva < sectionBegin ||
					rva >= sectionBegin + sectionSize) {
					continue;
				}

				const auto* nameBegin =
					reinterpret_cast<const char*>(section.Name);
				const auto nameLength = std::find(
					nameBegin,
					nameBegin + IMAGE_SIZEOF_SHORT_NAME,
					'\0') -
					nameBegin;
				result.name.assign(nameBegin, nameLength);
				result.characteristics = section.Characteristics;
				result.found = true;
				return result;
			}
			return result;
		}

		[[nodiscard]] bool IsExecutableProtection(DWORD protection) noexcept
		{
			const DWORD baseProtection = protection & 0xFFU;
			return (protection & PAGE_GUARD) == 0 &&
			       (baseProtection == PAGE_EXECUTE ||
					baseProtection == PAGE_EXECUTE_READ ||
					baseProtection == PAGE_EXECUTE_READWRITE ||
					baseProtection == PAGE_EXECUTE_WRITECOPY);
		}

		[[nodiscard]] bool IsWritableProtection(DWORD protection) noexcept
		{
			const DWORD baseProtection = protection & 0xFFU;
			return (protection & PAGE_GUARD) == 0 &&
			       (baseProtection == PAGE_READWRITE ||
					baseProtection == PAGE_WRITECOPY ||
					baseProtection == PAGE_EXECUTE_READWRITE ||
					baseProtection == PAGE_EXECUTE_WRITECOPY);
		}

		[[nodiscard]] std::string FormatPrologue(
			const std::uint8_t* bytes)
		{
			std::ostringstream stream;
			stream << std::hex << std::uppercase << std::setfill('0');
			for (std::size_t index = 0;
				index < kLoggedPrologueBytes;
				++index) {
				if (index != 0) {
					stream << ' ';
				}
				stream << std::setw(2)
				       << static_cast<unsigned int>(bytes[index]);
			}
			return stream.str();
		}

		template <std::size_t Size>
		[[nodiscard]] bool MatchesKnownPrologue(
			const char* name,
			const void* target,
			const std::array<std::uint8_t, Size>& expected) noexcept
		{
			if (!target ||
				std::memcmp(target, expected.data(), expected.size()) != 0) {
				try {
					logger::error(
						"Stage 5b rejected {} because its first {} bytes no longer match the Stage 5a OG measurement",
						name,
						expected.size());
				} catch (...) {
				}
				return false;
			}
			return true;
		}

		template <class T>
		void AttachArray(
			std::span<ComPtr<T>> destination,
			std::span<T*> source) noexcept
		{
			for (std::size_t index = 0; index < destination.size(); ++index) {
				destination[index].Attach(source[index]);
			}
		}

		template <class T>
		void GetRawArray(
			std::span<const ComPtr<T>> source,
			std::span<T*> destination) noexcept
		{
			for (std::size_t index = 0; index < source.size(); ++index) {
				destination[index] = source[index].Get();
			}
		}

		// Complete immediate-context guard for the stages Render_PreUI may
		// mutate. Stream-output offsets cannot be queried by D3D11, therefore
		// an active SO target makes this guard non-restorable and the auxiliary
		// pass fails closed instead of guessing an offset.
		class PipelineStateGuard final
		{
		public:
			explicit PipelineStateGuard(ID3D11DeviceContext* context) :
				context_(context)
			{
				if (!context_) {
					return;
				}

				context_->IAGetInputLayout(inputLayout_.GetAddressOf());
				{
					std::array<ID3D11Buffer*,
						D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT>
						raw{};
					context_->IAGetVertexBuffers(
						0,
						static_cast<UINT>(raw.size()),
						raw.data(),
						vertexStrides_.data(),
						vertexOffsets_.data());
					AttachArray<ID3D11Buffer>(vertexBuffers_, raw);
				}
				context_->IAGetIndexBuffer(
					indexBuffer_.GetAddressOf(),
					&indexFormat_,
					&indexOffset_);
				context_->IAGetPrimitiveTopology(&topology_);

				CaptureShaderStages();

				context_->RSGetState(rasterizerState_.GetAddressOf());
				viewportCount_ = static_cast<UINT>(viewports_.size());
				context_->RSGetViewports(&viewportCount_, viewports_.data());
				scissorCount_ = static_cast<UINT>(scissors_.size());
				context_->RSGetScissorRects(&scissorCount_, scissors_.data());

				{
					std::array<ID3D11RenderTargetView*,
						D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT>
						raw{};
					ID3D11DepthStencilView* rawDepth = nullptr;
					context_->OMGetRenderTargets(
						static_cast<UINT>(raw.size()),
						raw.data(),
						&rawDepth);
					AttachArray<ID3D11RenderTargetView>(renderTargets_, raw);
					depthTarget_.Attach(rawDepth);
					for (UINT index = 0;
						index < static_cast<UINT>(raw.size());
						++index) {
						if (raw[index]) {
							renderTargetCount_ = index + 1;
						}
					}

					// Pixel-shader UAVs are output-merger bindings in D3D11;
					// there is no PSGetUnorderedAccessViews API. Capture every
					// legal OM UAV slot after the highest active RTV.
					outputUAVStartSlot_ = renderTargetCount_;
					outputUAVCount_ =
						D3D11_PS_CS_UAV_REGISTER_COUNT -
						outputUAVStartSlot_;
					if (outputUAVCount_ != 0) {
						std::array<
							ID3D11UnorderedAccessView*,
							D3D11_PS_CS_UAV_REGISTER_COUNT>
							rawUAVs{};
						context_
							->OMGetRenderTargetsAndUnorderedAccessViews(
								0,
								nullptr,
								nullptr,
								outputUAVStartSlot_,
								outputUAVCount_,
								rawUAVs.data());
						AttachArray<ID3D11UnorderedAccessView>(
							outputUAVs_,
							rawUAVs);
						for (const auto& view : outputUAVs_) {
							if (view) {
								// D3D11 does not expose hidden append/consume
								// counters. A bound UAV is therefore not a
								// losslessly restorable entry state.
								restorable_ = false;
							}
						}
					}
				}
				context_->OMGetBlendState(
					blendState_.GetAddressOf(),
					blendFactor_.data(),
					&sampleMask_);
				context_->OMGetDepthStencilState(
					depthState_.GetAddressOf(),
					&stencilReference_);

				context_->GetPredication(
					predicate_.GetAddressOf(),
					&predicateValue_);

				std::array<ID3D11Buffer*,
					D3D11_SO_BUFFER_SLOT_COUNT>
					streamOutput{};
				context_->SOGetTargets(
					static_cast<UINT>(streamOutput.size()),
					streamOutput.data());
				for (auto* target : streamOutput) {
					if (target) {
						target->Release();
						restorable_ = false;
					}
				}
				captured_ = restorable_;
			}

			~PipelineStateGuard()
			{
				if (!captured_ || !context_) {
					return;
				}

				// Remove every binding left by the nested world render before
				// recreating the known legal entry state. Without this, an
				// auxiliary output binding can silently evict an SRV restored
				// earlier in the sequence.
				context_->ClearState();

				context_->IASetInputLayout(inputLayout_.Get());
				{
					std::array<ID3D11Buffer*,
						D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT>
						raw{};
					GetRawArray<ID3D11Buffer>(vertexBuffers_, raw);
					context_->IASetVertexBuffers(
						0,
						static_cast<UINT>(raw.size()),
						raw.data(),
						vertexStrides_.data(),
						vertexOffsets_.data());
				}
				context_->IASetIndexBuffer(
					indexBuffer_.Get(),
					indexFormat_,
					indexOffset_);
				context_->IASetPrimitiveTopology(topology_);

				RestoreShaderStages();

				context_->RSSetState(rasterizerState_.Get());
				context_->RSSetViewports(viewportCount_, viewports_.data());
				context_->RSSetScissorRects(scissorCount_, scissors_.data());

				{
					std::array<ID3D11RenderTargetView*,
						D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT>
						raw{};
					GetRawArray<ID3D11RenderTargetView>(renderTargets_, raw);
					std::array<
						ID3D11UnorderedAccessView*,
						D3D11_PS_CS_UAV_REGISTER_COUNT>
						rawUAVs{};
					std::array<
						UINT,
						D3D11_PS_CS_UAV_REGISTER_COUNT>
						initialCounts{};
					initialCounts.fill(
						D3D11_KEEP_UNORDERED_ACCESS_VIEWS);
					GetRawArray<ID3D11UnorderedAccessView>(
						outputUAVs_,
						rawUAVs);
					if (outputUAVCount_ != 0) {
						context_
							->OMSetRenderTargetsAndUnorderedAccessViews(
								renderTargetCount_,
								raw.data(),
								depthTarget_.Get(),
								outputUAVStartSlot_,
								outputUAVCount_,
								rawUAVs.data(),
								initialCounts.data());
					} else {
						// UAVStartSlot is constrained to 0..7 by D3D11 even
						// when NumUAVs is zero. Eight active RTVs leave no
						// legal UAV start slot, so restore that boundary with
						// the RTV-only API.
						context_->OMSetRenderTargets(
							renderTargetCount_,
							raw.data(),
							depthTarget_.Get());
					}
				}
				context_->OMSetBlendState(
					blendState_.Get(),
					blendFactor_.data(),
					sampleMask_);
				context_->OMSetDepthStencilState(
					depthState_.Get(),
					stencilReference_);
				context_->SetPredication(predicate_.Get(), predicateValue_);
			}

			[[nodiscard]] bool IsRestorable() const noexcept
			{
				return captured_;
			}

			PipelineStateGuard(const PipelineStateGuard&) = delete;
			PipelineStateGuard& operator=(const PipelineStateGuard&) = delete;

		private:
			static constexpr UINT kConstantBuffers = 14;
			static constexpr UINT kShaderResources = 128;
			static constexpr UINT kSamplers = 16;
			static constexpr UINT kComputeUAVs =
				D3D11_PS_CS_UAV_REGISTER_COUNT;

			template <class Shader>
			struct ShaderStage
			{
				ComPtr<Shader> shader;
				std::array<ComPtr<ID3D11Buffer>, kConstantBuffers> buffers;
				std::array<ComPtr<ID3D11ShaderResourceView>, kShaderResources>
					resources;
				std::array<ComPtr<ID3D11SamplerState>, kSamplers> samplers;
			};

			template <class Stage, class ShaderGetter, class BufferGetter,
				class ResourceGetter, class SamplerGetter>
			void CaptureStage(
				Stage& stage,
				ShaderGetter&& shaderGetter,
				BufferGetter&& bufferGetter,
				ResourceGetter&& resourceGetter,
				SamplerGetter&& samplerGetter)
			{
				UINT classInstanceCount = 0;
				shaderGetter(
					stage.shader.GetAddressOf(),
					nullptr,
					&classInstanceCount);
				if (classInstanceCount != 0) {
					// The game does not normally use dynamic-linkage class
					// instances. Since restoring them requires retaining a
					// variable list per stage, reject such a frame.
					restorable_ = false;
				}

				std::array<ID3D11Buffer*, kConstantBuffers> rawBuffers{};
				bufferGetter(
					0,
					static_cast<UINT>(rawBuffers.size()),
					rawBuffers.data());
				AttachArray<ID3D11Buffer>(stage.buffers, rawBuffers);

				std::array<ID3D11ShaderResourceView*, kShaderResources>
					rawResources{};
				resourceGetter(
					0,
					static_cast<UINT>(rawResources.size()),
					rawResources.data());
				AttachArray<ID3D11ShaderResourceView>(
					stage.resources,
					rawResources);

				std::array<ID3D11SamplerState*, kSamplers> rawSamplers{};
				samplerGetter(
					0,
					static_cast<UINT>(rawSamplers.size()),
					rawSamplers.data());
				AttachArray<ID3D11SamplerState>(
					stage.samplers,
					rawSamplers);
			}

			template <class Stage, class ShaderSetter, class BufferSetter,
				class ResourceSetter, class SamplerSetter>
			void RestoreStage(
				const Stage& stage,
				ShaderSetter&& shaderSetter,
				BufferSetter&& bufferSetter,
				ResourceSetter&& resourceSetter,
				SamplerSetter&& samplerSetter)
			{
				shaderSetter(stage.shader.Get(), nullptr, 0);

				std::array<ID3D11Buffer*, kConstantBuffers> rawBuffers{};
				GetRawArray<ID3D11Buffer>(stage.buffers, rawBuffers);
				bufferSetter(
					0,
					static_cast<UINT>(rawBuffers.size()),
					rawBuffers.data());

				std::array<ID3D11ShaderResourceView*, kShaderResources>
					rawResources{};
				GetRawArray<ID3D11ShaderResourceView>(
					stage.resources,
					rawResources);
				resourceSetter(
					0,
					static_cast<UINT>(rawResources.size()),
					rawResources.data());

				std::array<ID3D11SamplerState*, kSamplers> rawSamplers{};
				GetRawArray<ID3D11SamplerState>(stage.samplers, rawSamplers);
				samplerSetter(
					0,
					static_cast<UINT>(rawSamplers.size()),
					rawSamplers.data());
			}

			void CaptureShaderStages()
			{
#define CAPTURE_STAGE(prefix, member)                                            \
	CaptureStage(                                                                \
		member,                                                                  \
		[this](auto... args) { context_->prefix##GetShader(args...); },          \
		[this](auto... args) { context_->prefix##GetConstantBuffers(args...); }, \
		[this](auto... args) { context_->prefix##GetShaderResources(args...); }, \
		[this](auto... args) { context_->prefix##GetSamplers(args...); })
				CAPTURE_STAGE(VS, vertexStage_);
				CAPTURE_STAGE(HS, hullStage_);
				CAPTURE_STAGE(DS, domainStage_);
				CAPTURE_STAGE(GS, geometryStage_);
				CAPTURE_STAGE(PS, pixelStage_);
				CAPTURE_STAGE(CS, computeStage_);
#undef CAPTURE_STAGE

				std::array<ID3D11UnorderedAccessView*, kComputeUAVs> raw{};
				context_->CSGetUnorderedAccessViews(
					0,
					static_cast<UINT>(raw.size()),
					raw.data());
				AttachArray<ID3D11UnorderedAccessView>(
					computeUAVs_,
					raw);
				for (const auto& view : computeUAVs_) {
					if (view) {
						// See the OM-UAV note above. Reject the frame rather
						// than guessing hidden counter state.
						restorable_ = false;
					}
				}
			}

			void RestoreShaderStages()
			{
#define RESTORE_STAGE(prefix, member)                                            \
	RestoreStage(                                                                \
		member,                                                                  \
		[this](auto... args) { context_->prefix##SetShader(args...); },          \
		[this](auto... args) { context_->prefix##SetConstantBuffers(args...); }, \
		[this](auto... args) { context_->prefix##SetShaderResources(args...); }, \
		[this](auto... args) { context_->prefix##SetSamplers(args...); })
				RESTORE_STAGE(VS, vertexStage_);
				RESTORE_STAGE(HS, hullStage_);
				RESTORE_STAGE(DS, domainStage_);
				RESTORE_STAGE(GS, geometryStage_);
				RESTORE_STAGE(PS, pixelStage_);
				RESTORE_STAGE(CS, computeStage_);
#undef RESTORE_STAGE

				std::array<ID3D11UnorderedAccessView*, kComputeUAVs> raw{};
				std::array<UINT, kComputeUAVs> initialCounts{};
				initialCounts.fill(D3D11_KEEP_UNORDERED_ACCESS_VIEWS);
				GetRawArray<ID3D11UnorderedAccessView>(computeUAVs_, raw);
				context_->CSSetUnorderedAccessViews(
					0,
					static_cast<UINT>(raw.size()),
					raw.data(),
					initialCounts.data());
			}

			ID3D11DeviceContext* context_ = nullptr;
			bool captured_ = false;
			bool restorable_ = true;

			ComPtr<ID3D11InputLayout> inputLayout_;
			std::array<ComPtr<ID3D11Buffer>,
				D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT>
				vertexBuffers_;
			std::array<UINT, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT>
				vertexStrides_{};
			std::array<UINT, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT>
				vertexOffsets_{};
			ComPtr<ID3D11Buffer> indexBuffer_;
			DXGI_FORMAT indexFormat_ = DXGI_FORMAT_UNKNOWN;
			UINT indexOffset_ = 0;
			D3D11_PRIMITIVE_TOPOLOGY topology_ =
				D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;

			ShaderStage<ID3D11VertexShader> vertexStage_;
			ShaderStage<ID3D11HullShader> hullStage_;
			ShaderStage<ID3D11DomainShader> domainStage_;
			ShaderStage<ID3D11GeometryShader> geometryStage_;
			ShaderStage<ID3D11PixelShader> pixelStage_;
			ShaderStage<ID3D11ComputeShader> computeStage_;
			std::array<ComPtr<ID3D11UnorderedAccessView>, kComputeUAVs>
				computeUAVs_;

			ComPtr<ID3D11RasterizerState> rasterizerState_;
			std::array<D3D11_VIEWPORT,
				D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE>
				viewports_{};
			UINT viewportCount_ = 0;
			std::array<D3D11_RECT,
				D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE>
				scissors_{};
			UINT scissorCount_ = 0;

			std::array<ComPtr<ID3D11RenderTargetView>,
				D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT>
				renderTargets_;
			UINT renderTargetCount_ = 0;
			ComPtr<ID3D11DepthStencilView> depthTarget_;
			UINT outputUAVStartSlot_ = 0;
			UINT outputUAVCount_ = 0;
			std::array<
				ComPtr<ID3D11UnorderedAccessView>,
				D3D11_PS_CS_UAV_REGISTER_COUNT>
				outputUAVs_;
			ComPtr<ID3D11BlendState> blendState_;
			std::array<float, 4> blendFactor_{};
			UINT sampleMask_ = 0;
			ComPtr<ID3D11DepthStencilState> depthState_;
			UINT stencilReference_ = 0;
			ComPtr<ID3D11Predicate> predicate_;
			BOOL predicateValue_ = FALSE;
		};

		class AuxiliaryFlagGuard final
		{
		public:
			explicit AuxiliaryFlagGuard(State& state) noexcept :
				state_(state),
				previousPhase_(g_renderPhase),
				previousGeneration_(g_renderPhaseGeneration)
			{
				auto expected =
					WorldOnlyScopeRenderer::Phase::kIdle;
				if (!state_.phase.compare_exchange_strong(
						expected,
						WorldOnlyScopeRenderer::Phase::kAuxiliary,
						std::memory_order_acq_rel,
						std::memory_order_acquire)) {
					return;
				}
				g_auxiliaryPassActive = true;
				g_renderPhase = WorldOnlyScopeRenderer::Phase::kAuxiliary;
				g_renderPhaseGeneration = 0;
				acquired_ = true;
			}
			~AuxiliaryFlagGuard()
			{
				if (!acquired_) {
					return;
				}
				g_auxiliaryPassActive = false;
				g_renderPhase = previousPhase_;
				g_renderPhaseGeneration = previousGeneration_;
				state_.phase.store(
					WorldOnlyScopeRenderer::Phase::kIdle,
					std::memory_order_release);
			}

			[[nodiscard]] bool IsAcquired() const noexcept
			{
				return acquired_;
			}

			AuxiliaryFlagGuard(const AuxiliaryFlagGuard&) = delete;
			AuxiliaryFlagGuard& operator=(const AuxiliaryFlagGuard&) = delete;

		private:
			State& state_;
			WorldOnlyScopeRenderer::Phase previousPhase_;
			std::uint64_t previousGeneration_ = 0;
			bool acquired_ = false;
		};

		class PrimaryEligibilityGuard final
		{
		public:
			PrimaryEligibilityGuard(
				State& state,
				std::uint64_t generation) noexcept :
				state_(state),
				previousPhase_(g_renderPhase),
				previousGeneration_(g_renderPhaseGeneration)
			{
				auto expected =
					WorldOnlyScopeRenderer::Phase::kIdle;
				if (!state_.phase.compare_exchange_strong(
						expected,
						WorldOnlyScopeRenderer::Phase::kPrimaryEligible,
						std::memory_order_acq_rel,
						std::memory_order_acquire)) {
					return;
				}
				g_renderPhase =
					WorldOnlyScopeRenderer::Phase::kPrimaryEligible;
				g_renderPhaseGeneration = generation;
				acquired_ = true;
			}

			~PrimaryEligibilityGuard()
			{
				if (!acquired_) {
					return;
				}
				// The private source belongs only to this ordinary primary
				// invocation. Invalidate it before returning to any later
				// draw or Present callback so a prior frame cannot be reused.
				state_.outputReady.store(false, std::memory_order_release);
				g_renderPhase = previousPhase_;
				g_renderPhaseGeneration = previousGeneration_;
				state_.phase.store(
					WorldOnlyScopeRenderer::Phase::kIdle,
					std::memory_order_release);
			}

			[[nodiscard]] bool IsAcquired() const noexcept
			{
				return acquired_;
			}

			PrimaryEligibilityGuard(const PrimaryEligibilityGuard&) = delete;
			PrimaryEligibilityGuard& operator=(
				const PrimaryEligibilityGuard&) = delete;

		private:
			State& state_;
			WorldOnlyScopeRenderer::Phase previousPhase_;
			std::uint64_t previousGeneration_ = 0;
			bool acquired_ = false;
		};

		class InvocationGuard final
		{
		public:
			explicit InvocationGuard(State& state) noexcept :
				state_(state)
			{
				bool expected = false;
				acquired_ = state_.invocationActive.compare_exchange_strong(
					expected,
					true,
					std::memory_order_acq_rel,
					std::memory_order_acquire);
			}

			~InvocationGuard()
			{
				if (acquired_) {
					state_.invocationActive.store(
						false,
						std::memory_order_release);
				}
			}

			[[nodiscard]] bool IsAcquired() const noexcept
			{
				return acquired_;
			}

			InvocationGuard(const InvocationGuard&) = delete;
			InvocationGuard& operator=(const InvocationGuard&) = delete;

		private:
			State& state_;
			bool acquired_ = false;
		};

		class FirstPersonZCountGuard final
		{
		public:
			FirstPersonZCountGuard(
				std::uint32_t* opaque,
				std::uint32_t* alpha) noexcept :
				opaque_(opaque),
				alpha_(alpha)
			{
				if (!opaque_ || !alpha_) {
					return;
				}
				savedOpaque_ = *opaque_;
				savedAlpha_ = *alpha_;
				*opaque_ = 0;
				*alpha_ = 0;
				acquired_ = true;
			}

			~FirstPersonZCountGuard()
			{
				if (acquired_) {
					*opaque_ = savedOpaque_;
					*alpha_ = savedAlpha_;
				}
			}

			FirstPersonZCountGuard(const FirstPersonZCountGuard&) = delete;
			FirstPersonZCountGuard& operator=(
				const FirstPersonZCountGuard&) = delete;

		private:
			std::uint32_t* opaque_ = nullptr;
			std::uint32_t* alpha_ = nullptr;
			std::uint32_t savedOpaque_ = 0;
			std::uint32_t savedAlpha_ = 0;
			bool acquired_ = false;
		};

		class RendererSlotGuard final
		{
		public:
			RendererSlotGuard(
				RE::BSGraphics::RendererData* rendererData,
				const PrivateRenderTarget& color,
				const PrivateDepthTarget& depth) :
				rendererData_(rendererData)
			{
				if (!rendererData_ || !color.valid || !depth.valid) {
					return;
				}
				savedColor_ =
					rendererData_->renderTargets[kMainColorTarget];
				savedDepth_ =
					rendererData_->depthStencilTargets[kMainDepthTarget];

				auto& destinationColor =
					rendererData_->renderTargets[kMainColorTarget];
				destinationColor.texture =
					reinterpret_cast<REX::W32::ID3D11Texture2D*>(
						color.texture.Get());
				destinationColor.copyTexture =
					reinterpret_cast<REX::W32::ID3D11Texture2D*>(
						color.copyTexture.Get());
				destinationColor.rtView =
					reinterpret_cast<REX::W32::ID3D11RenderTargetView*>(
						color.renderTargetView.Get());
				destinationColor.srView =
					reinterpret_cast<REX::W32::ID3D11ShaderResourceView*>(
						color.shaderResourceView.Get());
				destinationColor.copySRView =
					reinterpret_cast<REX::W32::ID3D11ShaderResourceView*>(
						color.copyShaderResourceView.Get());
				destinationColor.uaView =
					reinterpret_cast<REX::W32::ID3D11UnorderedAccessView*>(
						color.unorderedAccessView.Get());

				auto& destinationDepth =
					rendererData_->depthStencilTargets[kMainDepthTarget];
				destinationDepth.texture =
					reinterpret_cast<REX::W32::ID3D11Texture2D*>(
						depth.texture.Get());
				for (std::size_t index = 0; index < 4; ++index) {
					destinationDepth.dsView[index] =
						reinterpret_cast<REX::W32::ID3D11DepthStencilView*>(
							depth.depthViews[index].Get());
					destinationDepth.dsViewReadOnlyDepth[index] =
						reinterpret_cast<REX::W32::ID3D11DepthStencilView*>(
							depth.readOnlyDepthViews[index].Get());
					destinationDepth.dsViewReadOnlyStencil[index] =
						reinterpret_cast<REX::W32::ID3D11DepthStencilView*>(
							depth.readOnlyStencilViews[index].Get());
					destinationDepth.dsViewReadOnlyDepthStencil[index] =
						reinterpret_cast<REX::W32::ID3D11DepthStencilView*>(
							depth.readOnlyDepthStencilViews[index].Get());
				}
				destinationDepth.srViewDepth =
					reinterpret_cast<REX::W32::ID3D11ShaderResourceView*>(
						depth.depthShaderResourceView.Get());
				destinationDepth.srViewStencil =
					reinterpret_cast<REX::W32::ID3D11ShaderResourceView*>(
						depth.stencilShaderResourceView.Get());
				acquired_ = true;
			}

			~RendererSlotGuard()
			{
				if (acquired_ && rendererData_) {
					rendererData_->renderTargets[kMainColorTarget] =
						savedColor_;
					rendererData_->depthStencilTargets[kMainDepthTarget] =
						savedDepth_;
				}
			}

			[[nodiscard]] bool IsAcquired() const noexcept
			{
				return acquired_;
			}

		private:
			RE::BSGraphics::RendererData* rendererData_ = nullptr;
			RE::BSGraphics::RenderTarget savedColor_{};
			RE::BSGraphics::DepthStencilTarget savedDepth_{};
			bool acquired_ = false;
		};

		[[nodiscard]] bool CreateMatchingTexture(
			ID3D11Device* device,
			ID3D11Texture2D* source,
			ComPtr<ID3D11Texture2D>& destination,
			D3D11_TEXTURE2D_DESC& description) noexcept
		{
			if (!device || !source) {
				return false;
			}
			source->GetDesc(&description);
			D3D11_TEXTURE2D_DESC created = description;
			created.Usage = D3D11_USAGE_DEFAULT;
			created.CPUAccessFlags = 0;
			created.MiscFlags &=
				~(D3D11_RESOURCE_MISC_SHARED |
					D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX |
					D3D11_RESOURCE_MISC_GDI_COMPATIBLE);
			return SUCCEEDED(device->CreateTexture2D(
				&created,
				nullptr,
				destination.ReleaseAndGetAddressOf()));
		}

		template <class View, class Desc, class Getter, class Creator>
		[[nodiscard]] bool CloneView(
			View* source,
			ComPtr<View>& destination,
			Getter&& getter,
			Creator&& creator) noexcept
		{
			if (!source) {
				destination.Reset();
				return true;
			}
			Desc description{};
			getter(source, description);
			return SUCCEEDED(
				creator(description, destination.ReleaseAndGetAddressOf()));
		}

		[[nodiscard]] bool BuildPrivateColor(
			ID3D11Device* device,
			const RE::BSGraphics::RenderTarget& source,
			PrivateRenderTarget& destination) noexcept
		{
			destination.Reset();
			if (!CaptureColorTargetIdentity(
					source,
					destination.sourceIdentity)) {
				return false;
			}
			auto* sourceTexture =
				reinterpret_cast<ID3D11Texture2D*>(source.texture);
			D3D11_TEXTURE2D_DESC sourceDescription{};
			if (!CreateMatchingTexture(
					device,
					sourceTexture,
					destination.texture,
					sourceDescription)) {
				return false;
			}

			auto createRTV = [&](const D3D11_RENDER_TARGET_VIEW_DESC& desc,
								 ID3D11RenderTargetView** output) {
				return device->CreateRenderTargetView(
					destination.texture.Get(),
					&desc,
					output);
			};
			auto createSRV = [&](const D3D11_SHADER_RESOURCE_VIEW_DESC& desc,
								 ID3D11ShaderResourceView** output) {
				return device->CreateShaderResourceView(
					destination.texture.Get(),
					&desc,
					output);
			};
			auto createUAV = [&](const D3D11_UNORDERED_ACCESS_VIEW_DESC& desc,
								 ID3D11UnorderedAccessView** output) {
				return device->CreateUnorderedAccessView(
					destination.texture.Get(),
					&desc,
					output);
			};
			if (!CloneView<
					ID3D11RenderTargetView,
					D3D11_RENDER_TARGET_VIEW_DESC>(
					reinterpret_cast<ID3D11RenderTargetView*>(source.rtView),
					destination.renderTargetView,
					[](auto* view, auto& desc) { view->GetDesc(&desc); },
					createRTV) ||
				!CloneView<
					ID3D11ShaderResourceView,
					D3D11_SHADER_RESOURCE_VIEW_DESC>(
					reinterpret_cast<ID3D11ShaderResourceView*>(
						source.srView),
					destination.shaderResourceView,
					[](auto* view, auto& desc) { view->GetDesc(&desc); },
					createSRV) ||
				!CloneView<
					ID3D11UnorderedAccessView,
					D3D11_UNORDERED_ACCESS_VIEW_DESC>(
					reinterpret_cast<ID3D11UnorderedAccessView*>(
						source.uaView),
					destination.unorderedAccessView,
					[](auto* view, auto& desc) { view->GetDesc(&desc); },
					createUAV)) {
				return false;
			}

			auto* sourceCopy =
				reinterpret_cast<ID3D11Texture2D*>(source.copyTexture);
			if (sourceCopy) {
				if (!CopyResourceCompatible(
						destination.sourceIdentity.texture.description,
						destination.sourceIdentity.copyTexture.description)) {
					// The private copy must be initialized from private RT4
					// before Render_PreUI. Reject an incompatible table layout
					// instead of exposing undefined texture contents.
					return false;
				}
				D3D11_TEXTURE2D_DESC ignored{};
				if (!CreateMatchingTexture(
						device,
						sourceCopy,
						destination.copyTexture,
						ignored)) {
					return false;
				}
				auto createCopySRV =
					[&](const D3D11_SHADER_RESOURCE_VIEW_DESC& desc,
						ID3D11ShaderResourceView** output) {
						return device->CreateShaderResourceView(
							destination.copyTexture.Get(),
							&desc,
							output);
					};
				if (!CloneView<
						ID3D11ShaderResourceView,
						D3D11_SHADER_RESOURCE_VIEW_DESC>(
						reinterpret_cast<ID3D11ShaderResourceView*>(
							source.copySRView),
						destination.copyShaderResourceView,
						[](auto* view, auto& desc) {
							view->GetDesc(&desc);
						},
						createCopySRV)) {
					return false;
				}
			} else if (source.copySRView) {
				return false;
			}

			destination.valid =
				destination.texture &&
				destination.renderTargetView &&
				destination.shaderResourceView;
			return destination.valid;
		}

		[[nodiscard]] bool BuildBoundaryCaptureColor(
			ID3D11Device* device,
			const RE::BSGraphics::RenderTarget& source,
			PrivateRenderTarget& destination) noexcept
		{
			destination.Reset();
			if (!device ||
				!CaptureColorTargetIdentity(
					source,
					destination.sourceIdentity)) {
				return false;
			}

			auto* sourceTexture =
				reinterpret_cast<ID3D11Texture2D*>(source.texture);
			auto* sourceView =
				reinterpret_cast<ID3D11ShaderResourceView*>(source.srView);
			if (!sourceTexture || !sourceView) {
				return false;
			}

			D3D11_TEXTURE2D_DESC ignored{};
			if (!CreateMatchingTexture(
					device,
					sourceTexture,
					destination.texture,
					ignored)) {
				return false;
			}

			D3D11_SHADER_RESOURCE_VIEW_DESC viewDescription{};
			sourceView->GetDesc(&viewDescription);
			if (FAILED(device->CreateShaderResourceView(
					destination.texture.Get(),
					&viewDescription,
					destination.shaderResourceView
						.ReleaseAndGetAddressOf()))) {
				destination.Reset();
				return false;
			}

			destination.valid =
				destination.texture && destination.shaderResourceView;
			return destination.valid;
		}

		[[nodiscard]] bool BuildPrivateDepth(
			ID3D11Device* device,
			const RE::BSGraphics::DepthStencilTarget& source,
			PrivateDepthTarget& destination) noexcept
		{
			destination.Reset();
			if (!CaptureDepthTargetIdentity(
					source,
					destination.sourceIdentity)) {
				return false;
			}
			auto* sourceTexture =
				reinterpret_cast<ID3D11Texture2D*>(source.texture);
			D3D11_TEXTURE2D_DESC sourceDescription{};
			if (!CreateMatchingTexture(
					device,
					sourceTexture,
					destination.texture,
					sourceDescription)) {
				return false;
			}

			auto cloneDSV = [&](ID3D11DepthStencilView* sourceView,
								ComPtr<ID3D11DepthStencilView>& result) {
				auto creator =
					[&](const D3D11_DEPTH_STENCIL_VIEW_DESC& desc,
						ID3D11DepthStencilView** output) {
						return device->CreateDepthStencilView(
							destination.texture.Get(),
							&desc,
							output);
					};
				return CloneView<
					ID3D11DepthStencilView,
					D3D11_DEPTH_STENCIL_VIEW_DESC>(
					sourceView,
					result,
					[](auto* view, auto& desc) { view->GetDesc(&desc); },
					creator);
			};
			for (std::size_t index = 0; index < 4; ++index) {
				if (!cloneDSV(
						reinterpret_cast<ID3D11DepthStencilView*>(
							source.dsView[index]),
						destination.depthViews[index]) ||
					!cloneDSV(
						reinterpret_cast<ID3D11DepthStencilView*>(
							source.dsViewReadOnlyDepth[index]),
						destination.readOnlyDepthViews[index]) ||
					!cloneDSV(
						reinterpret_cast<ID3D11DepthStencilView*>(
							source.dsViewReadOnlyStencil[index]),
						destination.readOnlyStencilViews[index]) ||
					!cloneDSV(
						reinterpret_cast<ID3D11DepthStencilView*>(
							source.dsViewReadOnlyDepthStencil[index]),
						destination.readOnlyDepthStencilViews[index])) {
					return false;
				}
			}

			auto cloneSRV = [&](ID3D11ShaderResourceView* sourceView,
								ComPtr<ID3D11ShaderResourceView>& result) {
				auto creator =
					[&](const D3D11_SHADER_RESOURCE_VIEW_DESC& desc,
						ID3D11ShaderResourceView** output) {
						return device->CreateShaderResourceView(
							destination.texture.Get(),
							&desc,
							output);
					};
				return CloneView<
					ID3D11ShaderResourceView,
					D3D11_SHADER_RESOURCE_VIEW_DESC>(
					sourceView,
					result,
					[](auto* view, auto& desc) { view->GetDesc(&desc); },
					creator);
			};
			if (!cloneSRV(
					reinterpret_cast<ID3D11ShaderResourceView*>(
						source.srViewDepth),
					destination.depthShaderResourceView) ||
				!cloneSRV(
					reinterpret_cast<ID3D11ShaderResourceView*>(
						source.srViewStencil),
					destination.stencilShaderResourceView)) {
				return false;
			}

			destination.valid =
				destination.texture && destination.depthViews[0] &&
				destination.depthShaderResourceView;
			return destination.valid;
		}

		[[nodiscard]] bool BuildScratchBackups(
			ID3D11Device* device,
			RE::BSGraphics::RendererData* rendererData,
			std::vector<TextureBackup>& backups,
			std::array<ColorTargetIdentity, kScratchColorTargets.size()>&
				colorIdentities,
			std::array<DepthTargetIdentity, kScratchDepthTargets.size()>&
				depthIdentities)
		{
			backups.clear();
			colorIdentities = {};
			depthIdentities = {};
			auto add = [&](ID3D11Texture2D* texture) {
				if (!texture) {
					return true;
				}
				if (std::ranges::any_of(
						backups,
						[texture](const TextureBackup& existing) {
							return existing.source.Get() == texture;
						})) {
					return true;
				}
				D3D11_TEXTURE2D_DESC ignored{};
				TextureBackup backup;
				backup.source = texture;
				if (!CreateMatchingTexture(
						device,
						texture,
						backup.backup,
						ignored)) {
					return false;
				}
				backups.push_back(std::move(backup));
				return true;
			};

			for (std::size_t identityIndex = 0;
				identityIndex < kScratchColorTargets.size();
				++identityIndex) {
				const auto index = kScratchColorTargets[identityIndex];
				const auto& target = rendererData->renderTargets[index];
				if (!CaptureColorTargetIdentity(
						target,
						colorIdentities[identityIndex])) {
					return false;
				}
				if (!add(reinterpret_cast<ID3D11Texture2D*>(target.texture)) ||
					!add(reinterpret_cast<ID3D11Texture2D*>(
						target.copyTexture))) {
					return false;
				}
			}
			for (std::size_t identityIndex = 0;
				identityIndex < kScratchDepthTargets.size();
				++identityIndex) {
				const auto index = kScratchDepthTargets[identityIndex];
				const auto& target =
					rendererData->depthStencilTargets[index];
				if (!CaptureDepthTargetIdentity(
						target,
						depthIdentities[identityIndex])) {
					return false;
				}
				if (!add(reinterpret_cast<ID3D11Texture2D*>(
						target.texture))) {
					return false;
				}
			}
			return true;
		}

		[[nodiscard]] bool ScratchResourcesMatch(
			const State& state,
			RE::BSGraphics::RendererData* rendererData) noexcept
		{
			if (!state.scratchIdentitiesValid || !rendererData) {
				return false;
			}
			for (std::size_t identityIndex = 0;
				identityIndex < kScratchColorTargets.size();
				++identityIndex) {
				ColorTargetIdentity current;
				if (!CaptureColorTargetIdentity(
						rendererData
							->renderTargets[kScratchColorTargets[identityIndex]],
						current) ||
					!SameColorTargetIdentity(
						current,
						state.scratchColorIdentities[identityIndex])) {
					return false;
				}
			}
			for (std::size_t identityIndex = 0;
				identityIndex < kScratchDepthTargets.size();
				++identityIndex) {
				DepthTargetIdentity current;
				if (!CaptureDepthTargetIdentity(
						rendererData
							->depthStencilTargets[kScratchDepthTargets[identityIndex]],
						current) ||
					!SameDepthTargetIdentity(
						current,
						state.scratchDepthIdentities[identityIndex])) {
					return false;
				}
			}
			return true;
		}

		[[nodiscard]] bool EnsurePrivateResources(
			State& state,
			RE::BSGraphics::RendererData* rendererData,
			ID3D11Device* device)
		{
			if (!rendererData || !device) {
				return false;
			}

			auto* colorTexture = reinterpret_cast<ID3D11Texture2D*>(
				rendererData->renderTargets[kMainColorTarget].texture);
			auto* depthTexture = reinterpret_cast<ID3D11Texture2D*>(
				rendererData->depthStencilTargets[kMainDepthTarget].texture);
			if (!colorTexture || !depthTexture) {
				return false;
			}

			ColorTargetIdentity colorIdentity;
			DepthTargetIdentity depthIdentity;
			if (!CaptureColorTargetIdentity(
					rendererData->renderTargets[kMainColorTarget],
					colorIdentity) ||
				!CaptureDepthTargetIdentity(
					rendererData
						->depthStencilTargets[kMainDepthTarget],
					depthIdentity)) {
				return false;
			}
			const bool rebuild =
				state.deviceIdentity != device ||
				!state.privateColor.valid ||
				!state.privateDepth.valid ||
				!SameColorTargetIdentity(
					state.privateColor.sourceIdentity,
					colorIdentity) ||
				!SameDepthTargetIdentity(
					state.privateDepth.sourceIdentity,
					depthIdentity) ||
				!ScratchResourcesMatch(state, rendererData);
			if (!rebuild) {
				return true;
			}

			state.outputReady.store(false, std::memory_order_release);
			state.privateColor.Reset();
			state.privateDepth.Reset();
			state.scratchBackups.clear();
			state.scratchColorIdentities = {};
			state.scratchDepthIdentities = {};
			state.scratchIdentitiesValid = false;
			state.deviceIdentity = nullptr;

			if (!BuildPrivateColor(
					device,
					rendererData->renderTargets[kMainColorTarget],
					state.privateColor) ||
				!BuildPrivateDepth(
					device,
					rendererData->depthStencilTargets[kMainDepthTarget],
					state.privateDepth) ||
				!BuildScratchBackups(
					device,
					rendererData,
					state.scratchBackups,
					state.scratchColorIdentities,
					state.scratchDepthIdentities)) {
				state.privateColor.Reset();
				state.privateDepth.Reset();
				state.scratchBackups.clear();
				state.scratchColorIdentities = {};
				state.scratchDepthIdentities = {};
				return false;
			}
			state.scratchIdentitiesValid = true;
			state.deviceIdentity = device;
			return true;
		}

		// The functional path needs only an immutable snapshot of ordinary RT4.
		// It deliberately does not build depth or the large scratch set used by
		// the dormant second-Render_PreUI experiment.
		[[nodiscard]] bool EnsureBoundaryCaptureResources(
			State& state,
			RE::BSGraphics::RendererData* rendererData,
			ID3D11Device* device)
		{
			if (!rendererData || !device) {
				return false;
			}

			const auto& color =
				rendererData->renderTargets[kMainColorTarget];
			if (!color.texture) {
				return false;
			}

			ColorTargetIdentity colorIdentity;
			if (!CaptureColorTargetIdentity(color, colorIdentity)) {
				return false;
			}

			const bool rebuild =
				state.deviceIdentity != device ||
				!state.privateColor.valid ||
				!SameColorTargetIdentity(
					state.privateColor.sourceIdentity,
					colorIdentity);
			if (!rebuild) {
				return true;
			}

			state.outputReady.store(false, std::memory_order_release);
			state.phase.store(
				WorldOnlyScopeRenderer::Phase::kIdle,
				std::memory_order_release);
			state.privateColor.Reset();
			// DS2 is intentionally not copied during this initial functional
			// slice. ScopeFade geometry supplies the correct authored depth.
			state.privateDepth.Reset();
			state.deviceIdentity = nullptr;

			if (!BuildBoundaryCaptureColor(
					device,
					color,
					state.privateColor)) {
				state.privateColor.Reset();
				state.privateDepth.Reset();
				return false;
			}

			state.deviceIdentity = device;
			return true;
		}

		[[nodiscard]] bool IsQualifiedMainColorBoundary(
			ID3D11DeviceContext* context,
			ID3D11Texture2D* mainColor) noexcept
		{
			if (!context || !mainColor) {
				return false;
			}

			D3D11_TEXTURE2D_DESC mainDescription{};
			mainColor->GetDesc(&mainDescription);
			if (mainDescription.Width < 640 ||
				mainDescription.Height < 360 ||
				mainDescription.ArraySize != 1 ||
				mainDescription.MipLevels != 1 ||
				mainDescription.SampleDesc.Count != 1 ||
				(mainDescription.BindFlags &
					D3D11_BIND_RENDER_TARGET) == 0 ||
				(mainDescription.BindFlags &
					D3D11_BIND_SHADER_RESOURCE) == 0) {
				return false;
			}

			std::array<ID3D11RenderTargetView*,
				D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT>
				renderTargets{};
			ID3D11DepthStencilView* depthView = nullptr;
			context->OMGetRenderTargets(
				static_cast<UINT>(renderTargets.size()),
				renderTargets.data(),
				&depthView);

			ComPtr<IUnknown> mainColorIdentity;
			if (FAILED(mainColor->QueryInterface(
					IID_PPV_ARGS(mainColorIdentity.GetAddressOf())))) {
				for (auto* renderTarget : renderTargets) {
					if (renderTarget) {
						renderTarget->Release();
					}
				}
				if (depthView) {
					depthView->Release();
				}
				return false;
			}

			std::uint32_t mainColorMatches = 0;
			for (auto*& renderTarget : renderTargets) {
				if (!renderTarget) {
					continue;
				}
				ComPtr<ID3D11Resource> resource;
				renderTarget->GetResource(resource.GetAddressOf());
				ComPtr<IUnknown> resourceIdentity;
				if (resource &&
					SUCCEEDED(resource.As(&resourceIdentity)) &&
					resourceIdentity.Get() ==
						mainColorIdentity.Get()) {
					++mainColorMatches;
				}
				renderTarget->Release();
				renderTarget = nullptr;
			}
			if (depthView) {
				depthView->Release();
			}
			// Renderer target index 4 identifies the resource in the engine
			// table; it is not an OM slot number. Require exactly one live RTV
			// to reference that resource so an absent or aliased binding fails
			// closed without copying from an ambiguous pipeline boundary.
			return mainColorMatches == 1;
		}

		class ScratchContentGuard final
		{
		public:
			ScratchContentGuard(
				ID3D11DeviceContext* context,
				std::span<TextureBackup> backups) :
				context_(context), backups_(backups)
			{
				if (!context_) {
					return;
				}
				for (auto& backup : backups_) {
					if (!backup.source || !backup.backup) {
						return;
					}
					context_->CopyResource(
						backup.backup.Get(),
						backup.source.Get());
				}
				acquired_ = true;
			}

			~ScratchContentGuard()
			{
				if (!acquired_ || !context_) {
					return;
				}
				for (auto& backup : backups_) {
					context_->CopyResource(
						backup.source.Get(),
						backup.backup.Get());
				}
			}

			[[nodiscard]] bool IsAcquired() const noexcept
			{
				return acquired_;
			}

		private:
			ID3D11DeviceContext* context_ = nullptr;
			std::span<TextureBackup> backups_;
			bool acquired_ = false;
		};

		void ClearAuxiliaryTargets(
			ID3D11DeviceContext* context,
			RE::BSGraphics::RendererData* rendererData,
			const PrivateRenderTarget& privateColor,
			const PrivateDepthTarget& privateDepth) noexcept
		{
			if (!context || !rendererData ||
				!privateColor.renderTargetView ||
				!privateDepth.depthViews[0]) {
				return;
			}

			// The private RT4/DS2 pair owns the auxiliary frame. RT4 starts
			// transparent because only its RGB output is currently consumed;
			// reversed-Z DS2 starts at the far plane.
			constexpr float privateClear[4]{
				0.0F, 0.0F, 0.0F, 0.0F
			};
			context->ClearRenderTargetView(
				privateColor.renderTargetView.Get(),
				privateClear);
			if (privateColor.copyTexture) {
				// RT4's copy SRV may be sampled before the engine refreshes it.
				// Seed it from the known clear so the auxiliary frame never
				// sees allocator contents from an earlier resource lifetime.
				context->CopyResource(
					privateColor.copyTexture.Get(),
					privateColor.texture.Get());
			}
			context->ClearDepthStencilView(
				privateDepth.depthViews[0].Get(),
				D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL,
				0.0F,
				0);

			// TTS clears RT0 through RT3 to opaque black before its second
			// Render_PreUI. These are real engine slots, but every non-null
			// texture was copied by ScratchContentGuard and is restored after
			// the auxiliary pipeline state unwinds.
			constexpr float opaqueBlack[4]{
				0.0F, 0.0F, 0.0F, 1.0F
			};
			for (const auto index : kInitialColorClearTargets) {
				auto* view = reinterpret_cast<ID3D11RenderTargetView*>(
					rendererData->renderTargets[index].rtView);
				if (view) {
					context->ClearRenderTargetView(view, opaqueBlack);
				}
			}

			// RT5 through RT19 are the GBuffer family used by this world pass.
			// An absent view is a legitimate renderer configuration and is
			// skipped. RT8 receives the neutral tangent-space normal while all
			// other present buffers receive zero.
			constexpr float emptyGBuffer[4]{
				0.0F, 0.0F, 0.0F, 0.0F
			};
			constexpr float defaultNormal[4]{
				0.5F, 0.5F, 1.0F, 1.0F
			};
			for (const auto index : kGBufferClearTargets) {
				auto* view = reinterpret_cast<ID3D11RenderTargetView*>(
					rendererData->renderTargets[index].rtView);
				if (view) {
					context->ClearRenderTargetView(
						view,
						index == 8 ? defaultNormal : emptyGBuffer);
				}
			}

			// DS8 is the shadow-map depth surface. Fallout 4 uses reversed Z,
			// so 0.0 is the far plane. Keeping stale first-pass depth here
			// would project world shadows from the displayed frame into the
			// isolated source.
			auto* shadowDepth = reinterpret_cast<ID3D11DepthStencilView*>(
				rendererData
					->depthStencilTargets[kScratchDepthTargets[0]]
					.dsView[0]);
			if (shadowDepth) {
				context->ClearDepthStencilView(
					shadowDepth,
					D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL,
					0.0F,
					0);
			}
		}

		[[nodiscard]] std::uint64_t ExecuteAuxiliaryPass(
			std::uint64_t drawWorld);

		constexpr std::uint64_t kAuxiliaryCallbackGateOpen =
			std::uint64_t{ 1 } << 63;
		constexpr std::uint64_t kAuxiliaryCallbackCountMask =
			~kAuxiliaryCallbackGateOpen;

		class AuxiliaryCallbackReference final
		{
		public:
			explicit AuxiliaryCallbackReference(State& state) noexcept :
				state_(state)
			{
				auto value = state_.auxiliaryCallbackGate.load(
					std::memory_order_acquire);
				while ((value & kAuxiliaryCallbackGateOpen) != 0) {
					if ((value & kAuxiliaryCallbackCountMask) ==
						kAuxiliaryCallbackCountMask) {
						return;
					}
					if (state_.auxiliaryCallbackGate.compare_exchange_weak(
							value,
							value + 1,
							std::memory_order_acq_rel,
							std::memory_order_acquire)) {
						acquired_ = true;
						return;
					}
				}
			}

			~AuxiliaryCallbackReference()
			{
				if (acquired_) {
					state_.auxiliaryCallbackGate.fetch_sub(
						1,
						std::memory_order_acq_rel);
				}
			}

			[[nodiscard]] bool IsAcquired() const noexcept
			{
				return acquired_;
			}

			AuxiliaryCallbackReference(
				const AuxiliaryCallbackReference&) = delete;
			AuxiliaryCallbackReference& operator=(
				const AuxiliaryCallbackReference&) = delete;

		private:
			State& state_;
			bool acquired_ = false;
		};

		[[nodiscard]] bool OpenAuxiliaryCallbackGate(State& state) noexcept
		{
			std::uint64_t expected = 0;
			return state.auxiliaryCallbackGate.compare_exchange_strong(
				expected,
				kAuxiliaryCallbackGateOpen,
				std::memory_order_acq_rel,
				std::memory_order_acquire);
		}

		[[nodiscard]] bool CloseAndDrainAuxiliaryCallbackGate(
			State& state) noexcept
		{
			const auto previous = state.auxiliaryCallbackGate.fetch_and(
				kAuxiliaryCallbackCountMask,
				std::memory_order_acq_rel);
			if ((previous & kAuxiliaryCallbackGateOpen) == 0) {
				return false;
			}

			// Stage 5b and 5c-a established that the observed OG callbacks are
			// quiescent when Render_PreUI returns. Keep a bounded allowance for
			// scheduler jitter, but never restore shared render state while an
			// admitted auxiliary callback is still executing.
			const auto deadline =
				std::chrono::steady_clock::now() +
				std::chrono::milliseconds(250);
			while ((state.auxiliaryCallbackGate.load(
						std::memory_order_acquire) &
					kAuxiliaryCallbackCountMask) != 0) {
				if (std::chrono::steady_clock::now() >= deadline) {
					return false;
				}
				SwitchToThread();
			}
			return true;
		}

		void UpdateMaximum(
			std::atomic_uint32_t& maximum,
			std::uint32_t candidate) noexcept
		{
			auto observed = maximum.load(std::memory_order_relaxed);
			while (candidate > observed &&
				   !maximum.compare_exchange_weak(
					   observed,
					   candidate,
					   std::memory_order_relaxed,
					   std::memory_order_relaxed)) {
			}
		}

		void RecordFirstThread(
			std::atomic_uint32_t& destination,
			std::uint32_t thread) noexcept
		{
			std::uint32_t empty = 0;
			destination.compare_exchange_strong(
				empty,
				thread,
				std::memory_order_relaxed,
				std::memory_order_relaxed);
		}

		void RecordCallbackThread(
			State& state,
			std::uint32_t thread) noexcept
		{
			for (auto& slot : state.observationCallbackThreads) {
				auto observed = slot.load(std::memory_order_relaxed);
				if (observed == thread) {
					return;
				}
				if (observed == 0 &&
					slot.compare_exchange_strong(
						observed,
						thread,
						std::memory_order_relaxed,
						std::memory_order_relaxed)) {
					return;
				}
			}
			state.observationCallbackThreadOverflow.fetch_add(
				1,
				std::memory_order_relaxed);
		}

		constexpr std::uint64_t kObservationPhaseMask = 0x3;
		constexpr std::uint64_t kObservationInsidePreUI = 0x1;
		constexpr std::uint64_t kObservationAfterReturn = 0x2;

		[[nodiscard]] constexpr std::uint64_t MakeObservationToken(
			std::uint64_t generation,
			std::uint64_t phase) noexcept
		{
			return (generation << 2) | phase;
		}

		template <class T>
		[[nodiscard]] bool ReadProcessValue(
			const T* source,
			T& destination) noexcept
		{
			if (!source) {
				return false;
			}
			SIZE_T read = 0;
			return ReadProcessMemory(
					   GetCurrentProcess(),
					   source,
					   &destination,
					   sizeof(destination),
					   &read) != FALSE &&
			       read == sizeof(destination);
		}

		[[nodiscard]] bool IsReadableCommittedRange(
			const void* pointer,
			std::size_t bytes) noexcept
		{
			if (!pointer || bytes == 0) {
				return false;
			}
			MEMORY_BASIC_INFORMATION memory{};
			if (VirtualQuery(pointer, &memory, sizeof(memory)) !=
					sizeof(memory) ||
				memory.State != MEM_COMMIT ||
				(memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
				return false;
			}
			const auto begin =
				reinterpret_cast<std::uintptr_t>(pointer);
			const auto regionBegin =
				reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
			const auto regionEnd = regionBegin + memory.RegionSize;
			return begin >= regionBegin && begin <= regionEnd &&
			       bytes <= regionEnd - begin;
		}

		[[nodiscard]] bool IsPlausibleAccumulator(
			RE::BSShaderAccumulator* accumulator) noexcept
		{
			if (!accumulator ||
				reinterpret_cast<std::uintptr_t>(accumulator) %
						alignof(void*) !=
					0 ||
				!IsReadableCommittedRange(accumulator, sizeof(void*))) {
				return false;
			}

			void* vtable = nullptr;
			if (!ReadProcessValue(
					reinterpret_cast<void* const*>(accumulator),
					vtable) ||
				!IsReadableCommittedRange(vtable, sizeof(void*))) {
				return false;
			}

			void* firstVirtualFunction = nullptr;
			if (!ReadProcessValue(
					reinterpret_cast<void* const*>(vtable),
					firstVirtualFunction)) {
				return false;
			}
			MEMORY_BASIC_INFORMATION functionMemory{};
			return firstVirtualFunction &&
			       VirtualQuery(
					   firstVirtualFunction,
					   &functionMemory,
					   sizeof(functionMemory)) == sizeof(functionMemory) &&
			       functionMemory.State == MEM_COMMIT &&
			       IsExecutableProtection(functionMemory.Protect);
		}

		[[nodiscard]] bool HaveSameCOMIdentity(
			IUnknown* left,
			IUnknown* right) noexcept
		{
			if (!left || !right) {
				return false;
			}
			if (left == right) {
				return true;
			}

			ComPtr<IUnknown> leftIdentity;
			ComPtr<IUnknown> rightIdentity;
			return SUCCEEDED(left->QueryInterface(
					   IID_IUnknown,
					   reinterpret_cast<void**>(
						   leftIdentity.ReleaseAndGetAddressOf()))) &&
			       SUCCEEDED(right->QueryInterface(
					   IID_IUnknown,
					   reinterpret_cast<void**>(
						   rightIdentity.ReleaseAndGetAddressOf()))) &&
			       leftIdentity.Get() == rightIdentity.Get();
		}

		struct ObservationEntryState
		{
			bool available = false;
			bool immediateContext = false;
			bool deviceMatches = false;
			bool multithreadInterface = false;
			bool multithreadProtected = false;
			bool outputUAVBound = false;
			bool computeUAVBound = false;
			bool streamOutputBound = false;
			bool predicateBound = false;
			UINT highestRenderTarget = 0;
			D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_9_1;
			void* rendererDevice = nullptr;
			void* contextDevice = nullptr;
			void* context = nullptr;
			void* rt4 = nullptr;
			void* ds2 = nullptr;
			D3D11_TEXTURE2D_DESC rt4Description{};
			D3D11_TEXTURE2D_DESC ds2Description{};
		};

		[[nodiscard]] ObservationEntryState ObserveD3DEntryState(
			RE::BSGraphics::RendererData* rendererData) noexcept
		{
			ObservationEntryState result;
			if (!rendererData || !rendererData->device ||
				!rendererData->context) {
				return result;
			}

			auto* device =
				reinterpret_cast<ID3D11Device*>(rendererData->device);
			auto* context = reinterpret_cast<ID3D11DeviceContext*>(
				rendererData->context);
			result.rendererDevice = device;
			result.context = context;
			result.immediateContext =
				context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE;
			result.featureLevel = device->GetFeatureLevel();

			ComPtr<ID3D11Device> contextDevice;
			context->GetDevice(contextDevice.GetAddressOf());
			result.contextDevice = contextDevice.Get();
			result.deviceMatches =
				HaveSameCOMIdentity(device, contextDevice.Get());

			ComPtr<ID3D11Multithread> multithread;
			result.multithreadInterface =
				SUCCEEDED(context->QueryInterface(
					IID_PPV_ARGS(multithread.GetAddressOf())));
			result.multithreadProtected =
				multithread && multithread->GetMultithreadProtected();

			std::array<ComPtr<ID3D11RenderTargetView>,
				D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT>
				renderTargets;
			std::array<ID3D11RenderTargetView*,
				D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT>
				rawRenderTargets{};
			ComPtr<ID3D11DepthStencilView> depthTarget;
			ID3D11DepthStencilView* rawDepthTarget = nullptr;
			context->OMGetRenderTargets(
				static_cast<UINT>(rawRenderTargets.size()),
				rawRenderTargets.data(),
				&rawDepthTarget);
			AttachArray<ID3D11RenderTargetView>(
				renderTargets,
				rawRenderTargets);
			depthTarget.Attach(rawDepthTarget);
			for (UINT index = 0;
				index < static_cast<UINT>(rawRenderTargets.size());
				++index) {
				if (rawRenderTargets[index]) {
					result.highestRenderTarget = index + 1;
				}
			}

			if (result.highestRenderTarget <
				D3D11_PS_CS_UAV_REGISTER_COUNT) {
				const auto start = result.highestRenderTarget;
				const auto count =
					D3D11_PS_CS_UAV_REGISTER_COUNT - start;
				std::array<ID3D11UnorderedAccessView*,
					D3D11_PS_CS_UAV_REGISTER_COUNT>
					rawOutputUAVs{};
				context->OMGetRenderTargetsAndUnorderedAccessViews(
					0,
					nullptr,
					nullptr,
					start,
					count,
					rawOutputUAVs.data());
				for (UINT index = 0; index < count; ++index) {
					if (rawOutputUAVs[index]) {
						result.outputUAVBound = true;
						rawOutputUAVs[index]->Release();
					}
				}
			}

			std::array<ID3D11UnorderedAccessView*,
				D3D11_PS_CS_UAV_REGISTER_COUNT>
				rawComputeUAVs{};
			context->CSGetUnorderedAccessViews(
				0,
				static_cast<UINT>(rawComputeUAVs.size()),
				rawComputeUAVs.data());
			for (auto* uav : rawComputeUAVs) {
				if (uav) {
					result.computeUAVBound = true;
					uav->Release();
				}
			}

			std::array<ID3D11Buffer*, D3D11_SO_BUFFER_SLOT_COUNT>
				rawStreamOutput{};
			context->SOGetTargets(
				static_cast<UINT>(rawStreamOutput.size()),
				rawStreamOutput.data());
			for (auto* buffer : rawStreamOutput) {
				if (buffer) {
					result.streamOutputBound = true;
					buffer->Release();
				}
			}

			ComPtr<ID3D11Predicate> predicate;
			BOOL predicateValue = FALSE;
			context->GetPredication(
				predicate.GetAddressOf(),
				&predicateValue);
			result.predicateBound = predicate != nullptr;

			auto* rt4 = reinterpret_cast<ID3D11Texture2D*>(
				rendererData->renderTargets[kMainColorTarget].texture);
			auto* ds2 = reinterpret_cast<ID3D11Texture2D*>(
				rendererData
					->depthStencilTargets[kMainDepthTarget]
					.texture);
			result.rt4 = rt4;
			result.ds2 = ds2;
			if (rt4) {
				rt4->GetDesc(&result.rt4Description);
			}
			if (ds2) {
				ds2->GetDesc(&result.ds2Description);
			}
			result.available = true;
			return result;
		}

		[[nodiscard]] std::uint32_t TotalLowerInFlight(
			const State& state) noexcept
		{
			return state.observationLowerInFlight.load(
				std::memory_order_acquire);
		}

		class ObservationCallbackGuard final
		{
		public:
			ObservationCallbackGuard(
				State& state,
				std::atomic_uint32_t& inFlight,
				std::atomic_uint32_t& firstThread) noexcept :
				state_(state),
				inFlight_(inFlight),
				entryToken_(0)
			{
				const auto thread = GetCurrentThreadId();
				RecordFirstThread(firstThread, thread);
				RecordCallbackThread(state_, thread);
				state_.observationLowerInFlight.fetch_add(
					1,
					std::memory_order_acq_rel);
				inFlight_.fetch_add(1, std::memory_order_acq_rel);
				entryToken_ = state_.observationToken.load(
					std::memory_order_acquire);
				UpdateMaximum(
					state_.observationMaxLowerInFlight,
					TotalLowerInFlight(state_));
				const auto ownerThread =
					state_.observationOwnerThread.load(
						std::memory_order_relaxed);
				if (ownerThread != 0 && thread == ownerThread) {
					state_.observationOwnerThreadCallbacks.fetch_add(
						1,
						std::memory_order_relaxed);
				} else {
					state_.observationWorkerThreadCallbacks.fetch_add(
						1,
						std::memory_order_relaxed);
				}
				if ((entryToken_ & kObservationPhaseMask) ==
					kObservationAfterReturn) {
					state_.observationPostReturnCallbacks.fetch_add(
						1,
						std::memory_order_relaxed);
				}
			}

			~ObservationCallbackGuard()
			{
				const auto exitToken =
					state_.observationToken.load(
						std::memory_order_acquire);
				if ((entryToken_ & kObservationPhaseMask) ==
						kObservationInsidePreUI &&
					exitToken != entryToken_) {
					state_
						.observationCompletedAfterOwnerReturn
						.fetch_add(1, std::memory_order_relaxed);
				}
				inFlight_.fetch_sub(1, std::memory_order_acq_rel);
				state_.observationLowerInFlight.fetch_sub(
					1,
					std::memory_order_acq_rel);
			}

			[[nodiscard]] std::uint64_t Generation() const noexcept
			{
				return entryToken_ >> 2;
			}

		private:
			State& state_;
			std::atomic_uint32_t& inFlight_;
			std::uint64_t entryToken_ = 0;
		};

		void LogObservationSummary(
			State& state,
			std::uint64_t generation) noexcept
		{
			try {
				logger::info(
					"Stage 5c-a observation summary generation {}: ownerThread={} ownerThreadChanges={} ownerCallbacks={} workerCallbacks={} callbackThreads=[{},{},{},{},{},{},{},{}] threadOverflow={} primaryOverlaps={} lowerInFlightAtReturn={} postReturnCallbacks={} completedAfterOwnerReturn={} maxLowerInFlight={} RenderBatches={} firstPersonMatches={} DoZPrePass={} DoUmbraQuery={} invalidAccumulatorSnapshots={} implausibleZCounts={} RT4Changes={} DS2Changes={} lastZ=({}, {})",
					generation,
					state.observationOwnerThread.load(
						std::memory_order_relaxed),
					state.observationOwnerThreadChanges.load(
						std::memory_order_relaxed),
					state.observationOwnerThreadCallbacks.load(
						std::memory_order_relaxed),
					state.observationWorkerThreadCallbacks.load(
						std::memory_order_relaxed),
					state.observationCallbackThreads[0].load(
						std::memory_order_relaxed),
					state.observationCallbackThreads[1].load(
						std::memory_order_relaxed),
					state.observationCallbackThreads[2].load(
						std::memory_order_relaxed),
					state.observationCallbackThreads[3].load(
						std::memory_order_relaxed),
					state.observationCallbackThreads[4].load(
						std::memory_order_relaxed),
					state.observationCallbackThreads[5].load(
						std::memory_order_relaxed),
					state.observationCallbackThreads[6].load(
						std::memory_order_relaxed),
					state.observationCallbackThreads[7].load(
						std::memory_order_relaxed),
					state.observationCallbackThreadOverflow.load(
						std::memory_order_relaxed),
					state.observationOverlappingPrimary.load(
						std::memory_order_relaxed),
					state.observationLowerInFlightAtReturn.load(
						std::memory_order_relaxed),
					state.observationPostReturnCallbacks.load(
						std::memory_order_relaxed),
					state.observationCompletedAfterOwnerReturn.load(
						std::memory_order_relaxed),
					state.observationMaxLowerInFlight.load(
						std::memory_order_relaxed),
					state.observationRenderBatchesCalls.load(
						std::memory_order_relaxed),
					state.observationFirstPersonBatchMatches.load(
						std::memory_order_relaxed),
					state.observationZPrePassCalls.load(
						std::memory_order_relaxed),
					state.observationUmbraCalls.load(
						std::memory_order_relaxed),
					state.observationInvalidAccumulatorSnapshots.load(
						std::memory_order_relaxed),
					state.observationImplausibleZCounts.load(
						std::memory_order_relaxed),
					state.observationRT4IdentityChanges.load(
						std::memory_order_relaxed),
					state.observationDS2IdentityChanges.load(
						std::memory_order_relaxed),
					state.observationLastOpaqueZCount.load(
						std::memory_order_relaxed),
					state.observationLastAlphaZCount.load(
						std::memory_order_relaxed));
			} catch (...) {
				// Observation logging must never escape an engine hook.
			}
		}

		void RecordPassThroughReturn(
			const char* hookName,
			std::atomic_uint64_t& returnedCalls) noexcept
		{
			const auto returned =
				returnedCalls.fetch_add(1, std::memory_order_relaxed) + 1;
			if (returned != 1) {
				return;
			}
			try {
				logger::info(
					"Stage 5b pass-through {} returned from its original function on thread {}",
					hookName,
					GetCurrentThreadId());
			} catch (...) {
				// A diagnostic heartbeat must never escape an engine hook.
			}
		}

		void ForwardObservedRenderPreUI(
			State& state,
			std::uint64_t drawWorld) noexcept
		{
			const auto ownerThread = GetCurrentThreadId();
			const auto previousOwner =
				state.observationOwnerThread.load(
					std::memory_order_relaxed);
			if (previousOwner == 0) {
				std::uint32_t empty = 0;
				state.observationOwnerThread.compare_exchange_strong(
					empty,
					ownerThread,
					std::memory_order_relaxed,
					std::memory_order_relaxed);
			} else if (previousOwner != ownerThread) {
				state.observationOwnerThreadChanges.fetch_add(
					1,
					std::memory_order_relaxed);
			}

			const auto primaryInFlight =
				state.observationRenderPreUIInFlight.fetch_add(
					1,
					std::memory_order_acq_rel) +
				1;
			const auto generation =
				state.observationGeneration.fetch_add(
					1,
					std::memory_order_acq_rel) +
				1;
			const auto insideToken = MakeObservationToken(
				generation,
				kObservationInsidePreUI);

			bool ownsToken = false;
			auto previousToken =
				state.observationToken.load(std::memory_order_acquire);
			if (primaryInFlight == 1 &&
				(previousToken == 0 ||
					(previousToken & kObservationPhaseMask) ==
						kObservationAfterReturn)) {
				ownsToken =
					state.observationToken.compare_exchange_strong(
						previousToken,
						insideToken,
						std::memory_order_acq_rel,
						std::memory_order_acquire);
			}
			if (!ownsToken) {
				state.observationOverlappingPrimary.fetch_add(
					1,
					std::memory_order_relaxed);
			}

			RE::BSShaderAccumulator* firstPersonAccumulator = nullptr;
			const bool accumulatorRead =
				ReadProcessValue(
					state.firstPersonAccumulator,
					firstPersonAccumulator);
			const bool accumulatorPlausible =
				accumulatorRead &&
				IsPlausibleAccumulator(firstPersonAccumulator);
			if (!accumulatorPlausible) {
				state.observationFirstPersonAccumulator.store(
					nullptr,
					std::memory_order_release);
				state.observationInvalidAccumulatorSnapshots.fetch_add(
					1,
					std::memory_order_relaxed);
			} else {
				state.observationFirstPersonAccumulator.store(
					firstPersonAccumulator,
					std::memory_order_release);
			}

			std::uint32_t opaqueZCount = 0;
			std::uint32_t alphaZCount = 0;
			const bool countsRead =
				ReadProcessValue(
					state.firstPersonZCount,
					opaqueZCount) &&
				ReadProcessValue(
					state.firstPersonAlphaZCount,
					alphaZCount);
			constexpr std::uint32_t kPlausibleMaximumZCount = 1'000'000;
			if (!countsRead ||
				opaqueZCount > kPlausibleMaximumZCount ||
				alphaZCount > kPlausibleMaximumZCount) {
				state.observationImplausibleZCounts.fetch_add(
					1,
					std::memory_order_relaxed);
			}
			state.observationLastOpaqueZCount.store(
				opaqueZCount,
				std::memory_order_relaxed);
			state.observationLastAlphaZCount.store(
				alphaZCount,
				std::memory_order_relaxed);

			auto* rendererData = RE::BSGraphics::GetRendererData();
			void* rt4 = rendererData ?
				reinterpret_cast<void*>(
					rendererData
						->renderTargets[kMainColorTarget]
						.texture) :
				nullptr;
			void* ds2 = rendererData ?
				reinterpret_cast<void*>(
					rendererData
						->depthStencilTargets[kMainDepthTarget]
						.texture) :
				nullptr;
			auto previousRT4 =
				state.observationRT4Identity.exchange(
					rt4,
					std::memory_order_acq_rel);
			auto previousDS2 =
				state.observationDS2Identity.exchange(
					ds2,
					std::memory_order_acq_rel);
			if (previousRT4 && previousRT4 != rt4) {
				state.observationRT4IdentityChanges.fetch_add(
					1,
					std::memory_order_relaxed);
			}
			if (previousDS2 && previousDS2 != ds2) {
				state.observationDS2IdentityChanges.fetch_add(
					1,
					std::memory_order_relaxed);
			}

			if (generation == 1 || generation % 300 == 0) {
				const auto d3d = ObserveD3DEntryState(rendererData);
				try {
					logger::info(
						"Stage 5c-a Render_PreUI entry generation {} thread={} drawWorld=0x{:X} fpAccumulator=0x{:X} fpValid={} Z=({}, {}) rendererData=0x{:X} context=0x{:X} contextImmediate={} rendererDevice=0x{:X} contextDevice=0x{:X} deviceMatch={} featureLevel=0x{:X} multithreadInterface={} multithreadProtected={} highestRTV={} OMUAV={} CSUAV={} SO={} predicate={} RT4=0x{:X} {}x{} format={} samples={} DS2=0x{:X} {}x{} format={} samples={}",
						generation,
						ownerThread,
						drawWorld,
						reinterpret_cast<std::uintptr_t>(
							firstPersonAccumulator),
						accumulatorPlausible,
						opaqueZCount,
						alphaZCount,
						reinterpret_cast<std::uintptr_t>(
							rendererData),
						reinterpret_cast<std::uintptr_t>(
							d3d.context),
						d3d.immediateContext,
						reinterpret_cast<std::uintptr_t>(
							d3d.rendererDevice),
						reinterpret_cast<std::uintptr_t>(
							d3d.contextDevice),
						d3d.deviceMatches,
						static_cast<unsigned int>(
							d3d.featureLevel),
						d3d.multithreadInterface,
						d3d.multithreadProtected,
						d3d.highestRenderTarget,
						d3d.outputUAVBound,
						d3d.computeUAVBound,
						d3d.streamOutputBound,
						d3d.predicateBound,
						reinterpret_cast<std::uintptr_t>(d3d.rt4),
						d3d.rt4Description.Width,
						d3d.rt4Description.Height,
						static_cast<unsigned int>(
							d3d.rt4Description.Format),
						d3d.rt4Description.SampleDesc.Count,
						reinterpret_cast<std::uintptr_t>(d3d.ds2),
						d3d.ds2Description.Width,
						d3d.ds2Description.Height,
						static_cast<unsigned int>(
							d3d.ds2Description.Format),
						d3d.ds2Description.SampleDesc.Count);
				} catch (...) {
					// Observation logging must never escape this hook.
				}
			}

			state.renderPreUIOriginal(drawWorld);

			if (ownsToken) {
				state.observationToken.store(
					MakeObservationToken(
						generation,
						kObservationAfterReturn),
					std::memory_order_release);
			}
			const auto lowerInFlight = TotalLowerInFlight(state);
			if (lowerInFlight != 0) {
				state.observationLowerInFlightAtReturn.fetch_add(
					lowerInFlight,
					std::memory_order_relaxed);
			}
			state.observationFirstPersonAccumulator.store(
				nullptr,
				std::memory_order_release);
			state.observationRenderPreUIInFlight.fetch_sub(
				1,
				std::memory_order_acq_rel);

			if (generation == 1 || generation % 300 == 0) {
				LogObservationSummary(state, generation);
			}
		}

		void __fastcall HookRenderPreUI(std::uint64_t drawWorld)
		{
			auto& state = GetState();
			if (!state.renderPreUIOriginal) {
				return;
			}
			if (state.passThroughOnly.load(std::memory_order_acquire)) {
				if (state.observationOnly.load(
						std::memory_order_acquire)) {
					ForwardObservedRenderPreUI(state, drawWorld);
				} else {
					state.renderPreUIOriginal(drawWorld);
				}
				RecordPassThroughReturn(
					"DrawWorld::Render_PreUI",
					state.renderPreUIForwarded);
				return;
			}

			InvocationGuard invocationGuard(state);
			if (!invocationGuard.IsAcquired()) {
				// Concurrent ordinary Render_PreUI invocations would make the
				// shared renderer table and phase token ambiguous. Permanently
				// disable future auxiliary work and forward this invocation
				// rather than entering a second transaction.
				state.outputReady.store(false, std::memory_order_release);
				state.requested.store(false, std::memory_order_release);
				state.permanentlyDisabled.store(
					true,
					std::memory_order_release);
				state.renderPreUIOriginal(drawWorld);
				return;
			}

			// A private source belongs to one ordinary frame. Invalidate the
			// preceding generation before the engine begins the next frame.
			state.outputReady.store(false, std::memory_order_release);
			state.phase.store(
				WorldOnlyScopeRenderer::Phase::kIdle,
				std::memory_order_release);
			state.firstPersonCaptureClaimed.store(
				false,
				std::memory_order_release);
			state.captureFrameGeneration.fetch_add(
				1,
				std::memory_order_acq_rel);
			g_renderPhase = WorldOnlyScopeRenderer::Phase::kIdle;
			g_renderPhaseGeneration = 0;

			RE::BSShaderAccumulator* firstPersonAccumulator = nullptr;
			if (state.requested.load(std::memory_order_acquire) &&
				state.firstPersonAccumulator &&
				ReadProcessValue(
					state.firstPersonAccumulator,
					firstPersonAccumulator) &&
				IsPlausibleAccumulator(firstPersonAccumulator)) {
				state.primaryFirstPersonAccumulator.store(
					firstPersonAccumulator,
					std::memory_order_release);
			} else {
				state.primaryFirstPersonAccumulator.store(
					nullptr,
					std::memory_order_release);
			}

			// The functional path is exactly the ordinary pass. The matching
			// RenderBatches hook only opens an admission window so the existing
			// D3D hook can snapshot RT4 immediately before the first
			// first-person draw. No nested Render_PreUI call is made.
			state.renderPreUIOriginal(drawWorld);
			state.primaryFirstPersonAccumulator.store(
				nullptr,
				std::memory_order_release);
		}

		class FirstPersonCaptureArm final
		{
		public:
			explicit FirstPersonCaptureArm(
				std::uint64_t frameGeneration) noexcept :
				previousDepth_(g_firstPersonCaptureArmDepth),
				previousFrame_(g_firstPersonCaptureFrame)
			{
				g_firstPersonCaptureArmDepth = previousDepth_ + 1;
				g_firstPersonCaptureFrame = frameGeneration;
			}

			~FirstPersonCaptureArm()
			{
				g_firstPersonCaptureArmDepth = previousDepth_;
				g_firstPersonCaptureFrame = previousFrame_;
			}

			FirstPersonCaptureArm(const FirstPersonCaptureArm&) = delete;
			FirstPersonCaptureArm& operator=(
				const FirstPersonCaptureArm&) = delete;

		private:
			std::uint32_t previousDepth_ = 0;
			std::uint64_t previousFrame_ = 0;
		};

		void __fastcall HookRenderBatches(
			RE::BSShaderAccumulator* accumulator,
			int shader,
			bool alphaPass,
			int group)
		{
			auto& state = GetState();
			if (state.passThroughOnly.load(std::memory_order_acquire)) {
				if (state.renderBatchesOriginal) {
					std::optional<ObservationCallbackGuard>
						observationGuard;
					if (state.observationOnly.load(
							std::memory_order_acquire)) {
						observationGuard.emplace(
							state,
							state.observationRenderBatchesInFlight,
							state.observationRenderBatchesThread);
						const auto call =
							state.observationRenderBatchesCalls.fetch_add(
								1,
								std::memory_order_relaxed) +
							1;
						auto* firstPerson =
							state.observationFirstPersonAccumulator.load(
								std::memory_order_acquire);
						if (firstPerson &&
							accumulator == firstPerson) {
							state
								.observationFirstPersonBatchMatches
								.fetch_add(
									1,
									std::memory_order_relaxed);
						}
						if (call == 1) {
							try {
								logger::info(
									"Stage 5c-a first RenderBatches observation: generation={} thread={} accumulator=0x{:X} fpSnapshot=0x{:X} match={} shader={} alphaPass={} group={}",
									observationGuard->Generation(),
									GetCurrentThreadId(),
									reinterpret_cast<std::uintptr_t>(
										accumulator),
									reinterpret_cast<std::uintptr_t>(
										firstPerson),
									firstPerson &&
										accumulator == firstPerson,
									shader,
									alphaPass,
									group);
							} catch (...) {
							}
						}
					}
					state.renderBatchesOriginal(
						accumulator,
						shader,
						alphaPass,
						group);
					RecordPassThroughReturn(
						"BSShaderAccumulator::RenderBatches",
						state.renderBatchesForwarded);
				}
				return;
			}
			if (state.renderBatchesOriginal) {
				const bool captureBoundary =
					state.requested.load(std::memory_order_acquire) &&
					!state.permanentlyDisabled.load(
						std::memory_order_acquire) &&
					!state.outputReady.load(std::memory_order_acquire) &&
					accumulator &&
					accumulator ==
						state.primaryFirstPersonAccumulator.load(
							std::memory_order_acquire);
				if (captureBoundary) {
					FirstPersonCaptureArm arm(
						state.captureFrameGeneration.load(
							std::memory_order_acquire));
					state.renderBatchesOriginal(
						accumulator,
						shader,
						alphaPass,
						group);
					return;
				}
				state.renderBatchesOriginal(
					accumulator,
					shader,
					alphaPass,
					group);
			}
		}

		void __fastcall HookDoZPrePass(
			std::uint64_t self,
			RE::NiCamera* firstPersonCamera,
			RE::NiCamera* worldCamera,
			float firstPersonNear,
			float firstPersonFar,
			float nearPlane,
			float farPlane)
		{
			auto& state = GetState();
			if (!state.doZPrePassOriginal) {
				return;
			}
			if (state.passThroughOnly.load(std::memory_order_acquire)) {
				std::optional<ObservationCallbackGuard>
					observationGuard;
				if (state.observationOnly.load(
						std::memory_order_acquire)) {
					observationGuard.emplace(
						state,
						state.observationZPrePassInFlight,
						state.observationZPrePassThread);
					const auto call =
						state.observationZPrePassCalls.fetch_add(
							1,
							std::memory_order_relaxed) +
						1;
					std::uint32_t opaqueCount = 0;
					std::uint32_t alphaCount = 0;
					(void)ReadProcessValue(
						state.firstPersonZCount,
						opaqueCount);
					(void)ReadProcessValue(
						state.firstPersonAlphaZCount,
						alphaCount);
					state.observationLastOpaqueZCount.store(
						opaqueCount,
						std::memory_order_relaxed);
					state.observationLastAlphaZCount.store(
						alphaCount,
						std::memory_order_relaxed);
					if (call == 1) {
						try {
							logger::info(
								"Stage 5c-a first DoZPrePass observation: generation={} thread={} self=0x{:X} firstPersonCamera=0x{:X} worldCamera=0x{:X} firstPersonPlanes=({}, {}) worldPlanes=({}, {}) Z=({}, {})",
								observationGuard->Generation(),
								GetCurrentThreadId(),
								self,
								reinterpret_cast<std::uintptr_t>(
									firstPersonCamera),
								reinterpret_cast<std::uintptr_t>(
									worldCamera),
								firstPersonNear,
								firstPersonFar,
								nearPlane,
								farPlane,
								opaqueCount,
								alphaCount);
						} catch (...) {
						}
					}
				}
				state.doZPrePassOriginal(
					self,
					firstPersonCamera,
					worldCamera,
					firstPersonNear,
					firstPersonFar,
					nearPlane,
					farPlane);
				RecordPassThroughReturn(
					"Renderer::DoZPrePass",
					state.doZPrePassForwarded);
				return;
			}
			if (state.phase.load(std::memory_order_acquire) !=
					WorldOnlyScopeRenderer::Phase::kAuxiliary) {
				state.doZPrePassOriginal(
					self,
					firstPersonCamera,
					worldCamera,
					firstPersonNear,
					firstPersonFar,
					nearPlane,
					farPlane);
				return;
			}

			AuxiliaryCallbackReference callback(state);
			state.doZPrePassOriginal(
				self,
				firstPersonCamera,
				worldCamera,
				firstPersonNear,
				firstPersonFar,
				nearPlane,
				farPlane);
		}

		void __fastcall HookDoUmbraQuery(std::uint64_t drawWorld)
		{
			auto& state = GetState();
			if (!state.doUmbraQueryOriginal) {
				return;
			}
			if (state.passThroughOnly.load(std::memory_order_acquire)) {
				std::optional<ObservationCallbackGuard>
					observationGuard;
				if (state.observationOnly.load(
						std::memory_order_acquire)) {
					observationGuard.emplace(
						state,
						state.observationUmbraInFlight,
						state.observationUmbraThread);
					const auto call =
						state.observationUmbraCalls.fetch_add(
							1,
							std::memory_order_relaxed) +
						1;
					if (call == 1) {
						try {
							logger::info(
								"Stage 5c-a first DoUmbraQuery observation: generation={} thread={} drawWorld=0x{:X}",
								observationGuard->Generation(),
								GetCurrentThreadId(),
								drawWorld);
						} catch (...) {
						}
					}
				}
				state.doUmbraQueryOriginal(drawWorld);
				RecordPassThroughReturn(
					"DrawWorld::DoUmbraQuery",
					state.doUmbraQueryForwarded);
				return;
			}

			// The auxiliary call must reuse the immediately preceding primary
			// visibility result. Running Umbra twice mutates shared culling
			// state and the inherited implementation can dereference invalid
			// data from the synthetic invocation.
			if (state.phase.load(std::memory_order_acquire) ==
				WorldOnlyScopeRenderer::Phase::kAuxiliary) {
				AuxiliaryCallbackReference callback(state);
				return;
			}
			state.doUmbraQueryOriginal(drawWorld);
		}

		std::uint64_t ExecuteAuxiliaryPass(std::uint64_t drawWorld)
		{
			auto& state = GetState();
			std::scoped_lock lock(state.resourceMutex);
			state.outputReady.store(false, std::memory_order_release);

			auto* rendererData = RE::BSGraphics::GetRendererData();
			auto* context = rendererData ?
			                    reinterpret_cast<ID3D11DeviceContext*>(
									rendererData->context) :
			                    nullptr;
			auto* device = rendererData ?
			                   reinterpret_cast<ID3D11Device*>(rendererData->device) :
			                   nullptr;
			if (!rendererData || !context || !device ||
				context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE ||
				!EnsurePrivateResources(state, rendererData, device)) {
				return 0;
			}

			RE::BSShaderAccumulator* firstPersonAccumulator = nullptr;
			if (!state.firstPersonAccumulator ||
				!ReadProcessValue(
					state.firstPersonAccumulator,
					firstPersonAccumulator) ||
				!IsPlausibleAccumulator(firstPersonAccumulator) ||
				!state.firstPersonZCount ||
				!state.firstPersonAlphaZCount) {
				return 0;
			}
			state.auxiliaryFirstPersonAccumulator.store(
				firstPersonAccumulator,
				std::memory_order_release);

			{
				ScratchContentGuard scratchGuard(
					context,
					state.scratchBackups);
				if (!scratchGuard.IsAcquired()) {
					return 0;
				}
				// Construct the pipeline guard after the scratch backup.
				// Destruction is reverse-order, so the original bindings are
				// restored before ScratchContentGuard copies saved textures
				// back into resources that the auxiliary pass may have left
				// bound.
				PipelineStateGuard pipelineGuard(context);
				if (!pipelineGuard.IsRestorable()) {
					return 0;
				}
				RendererSlotGuard slotGuard(
					rendererData,
					state.privateColor,
					state.privateDepth);
				if (!slotGuard.IsAcquired()) {
					return 0;
				}
				ClearAuxiliaryTargets(
					context,
					rendererData,
					state.privateColor,
					state.privateDepth);

				// Z-prepass workers read these process globals. Zero them for
				// the complete auxiliary epoch so first-person draw data cannot
				// enter private depth, then restore only after every admitted
				// lower callback has drained.
				FirstPersonZCountGuard firstPersonZGuard(
					state.firstPersonZCount,
					state.firstPersonAlphaZCount);
				AuxiliaryFlagGuard passGuard(state);
				if (!passGuard.IsAcquired()) {
					return 0;
				}

				if (!OpenAuxiliaryCallbackGate(state)) {
					return 0;
				}
				state.renderPreUIOriginal(drawWorld);
				if (!CloseAndDrainAuxiliaryCallbackGate(state)) {
					state.auxiliaryCallbackGate.store(
						0,
						std::memory_order_release);
					state.auxiliaryFirstPersonAccumulator.store(
						nullptr,
						std::memory_order_release);
					state.requested.store(
						false,
						std::memory_order_release);
					state.permanentlyDisabled.store(
						true,
						std::memory_order_release);
					try {
						logger::error(
							"World-only auxiliary renderer disabled because worker callbacks did not quiesce within 250 ms");
					} catch (...) {
					}
					return 0;
				}
			}
			state.auxiliaryFirstPersonAccumulator.store(
				nullptr,
				std::memory_order_release);

			if (state.permanentlyDisabled.load(std::memory_order_acquire) ||
				!state.requested.load(std::memory_order_acquire)) {
				return 0;
			}

			// Publish only after the auxiliary call, pipeline restoration, slot
			// restoration and scratch-content rollback all complete. The hook
			// immediately enters the one primary invocation allowed to consume
			// this exact generation.
			const auto publishedGeneration =
				state.generation.fetch_add(
					1,
					std::memory_order_acq_rel) +
				1;
			state.outputReady.store(true, std::memory_order_release);
			return publishedGeneration;
		}

	}

	WorldOnlyScopeRenderer& WorldOnlyScopeRenderer::GetSingleton() noexcept
	{
		static WorldOnlyScopeRenderer singleton;
		return singleton;
	}

	bool WorldOnlyScopeRenderer::ProbeRuntimeTargets()
	{
		constexpr REL::Version supportedRuntime{ 1, 10, 163, 0 };
		if (!REX::FModule::IsRuntimeOG()) {
			logger::error(
				"World-only runtime preflight refused a non-OG runtime");
			return false;
		}

		const auto executableModule = REX::FModule::GetExecutingModule();
		const auto runtimeVersion = executableModule.GetFileVersion();
		if (runtimeVersion != supportedRuntime) {
			logger::error(
				"World-only runtime preflight requires Fallout 4 1.10.163.0; loaded executable is {}",
				runtimeVersion.string());
			return false;
		}

		const auto moduleBase = executableModule.GetBaseAddress();
		const auto moduleHandle =
			reinterpret_cast<HMODULE>(moduleBase);
		if (!moduleBase || moduleHandle != GetModuleHandleW(nullptr)) {
			logger::error(
				"World-only runtime preflight could not establish the Fallout4.exe image base");
			return false;
		}

		struct Target
		{
			const char* name;
			std::uint64_t id;
			std::uintptr_t expectedRVA;
			std::size_t alignment;
			bool executable;
		};

		constexpr std::array<Target, 8> targets{
			Target{
				"DrawWorld::Render_PreUI",
				kRenderPreUIID,
				kRenderPreUIRVA,
				1,
				true },
			Target{
				"BSShaderAccumulator::RenderBatches",
				kRenderBatchesID,
				kRenderBatchesRVA,
				1,
				true },
			Target{
				"Renderer::DoZPrePass",
				kDoZPrePassID,
				kDoZPrePassRVA,
				1,
				true },
			Target{
				"DrawWorld::DoUmbraQuery",
				kDoUmbraQueryID,
				kDoUmbraQueryRVA,
				1,
				true },
			Target{
				"BSGraphics::RenderZPrePass",
				kRenderZPrePassID,
				kRenderZPrePassRVA,
				1,
				true },
			Target{
				"ptr_Draw1stPersonAccum",
				kFirstPersonAccumulatorID,
				kFirstPersonAccumulatorRVA,
				8,
				false },
			Target{
				"FPZPrePassDrawDataCount",
				kFirstPersonZCountID,
				kFirstPersonZCountRVA,
				4,
				false },
			Target{
				"FPAlphaTestZPrePassDrawDataCount",
				kFirstPersonAlphaZCountID,
				kFirstPersonAlphaZCountRVA,
				4,
				false }
		};

		try {
			for (const auto& target : targets) {
				const REL::ID id{ target.id };
				const auto resolvedAddress = id.address();
				const auto expectedAddress =
					moduleBase + target.expectedRVA;
				if (resolvedAddress != expectedAddress) {
					logger::error(
						"World-only runtime preflight rejected {}: ID {} resolved 0x{:X}, expected Fallout4.exe+0x{:X} (0x{:X})",
						target.name,
						target.id,
						resolvedAddress,
						target.expectedRVA,
						expectedAddress);
					return false;
				}
				if (target.alignment > 1 &&
					resolvedAddress % target.alignment != 0) {
					logger::error(
						"World-only runtime preflight rejected {}: address 0x{:X} is not {}-byte aligned",
						target.name,
						resolvedAddress,
						target.alignment);
					return false;
				}

				MEMORY_BASIC_INFORMATION memory{};
				if (VirtualQuery(
						reinterpret_cast<const void*>(resolvedAddress),
						&memory,
						sizeof(memory)) != sizeof(memory) ||
					memory.State != MEM_COMMIT ||
					memory.Type != MEM_IMAGE ||
					memory.AllocationBase != moduleHandle) {
					logger::error(
						"World-only runtime preflight rejected {}: target is not a committed Fallout4.exe image page",
						target.name);
					return false;
				}

				const auto section =
					FindLoadedSection(moduleHandle, resolvedAddress);
				const bool sectionPermission =
					section.found &&
					(target.executable ?
							(section.characteristics &
								IMAGE_SCN_MEM_EXECUTE) != 0 :
							(section.characteristics &
								IMAGE_SCN_MEM_WRITE) != 0);
				const bool pagePermission =
					target.executable ?
						IsExecutableProtection(memory.Protect) :
						IsWritableProtection(memory.Protect);
				if (!sectionPermission || !pagePermission) {
					logger::error(
						"World-only runtime preflight rejected {}: section={} characteristics=0x{:08X} protection=0x{:08X}",
						target.name,
						section.found ? section.name : "<none>",
						section.characteristics,
						memory.Protect);
					return false;
				}

				const auto regionEnd =
					reinterpret_cast<std::uintptr_t>(
						memory.BaseAddress) +
					memory.RegionSize;
				if (target.executable &&
					(regionEnd < resolvedAddress ||
						regionEnd - resolvedAddress <
							kLoggedPrologueBytes)) {
					logger::error(
						"World-only runtime preflight rejected {}: its first {} bytes cross the validated image region",
						target.name,
						kLoggedPrologueBytes);
					return false;
				}

				if (target.executable) {
					const auto prologue = FormatPrologue(
						reinterpret_cast<const std::uint8_t*>(
							resolvedAddress));
					logger::info(
						"World-only preflight function {}: ID={} RVA=0x{:X} address=0x{:X} section={} sectionFlags=0x{:08X} regionBase=0x{:X} regionSize=0x{:X} state=0x{:X} type=0x{:X} protect=0x{:X} prologue[16]=[{}]",
						target.name,
						target.id,
						target.expectedRVA,
						resolvedAddress,
						section.name,
						section.characteristics,
						reinterpret_cast<std::uintptr_t>(
							memory.BaseAddress),
						memory.RegionSize,
						memory.State,
						memory.Type,
						memory.Protect,
						prologue);
				} else {
					logger::info(
						"World-only preflight data {}: ID={} RVA=0x{:X} address=0x{:X} alignment={} section={} sectionFlags=0x{:08X} regionBase=0x{:X} regionSize=0x{:X} state=0x{:X} type=0x{:X} protect=0x{:X}",
						target.name,
						target.id,
						target.expectedRVA,
						resolvedAddress,
						target.alignment,
						section.name,
						section.characteristics,
						reinterpret_cast<std::uintptr_t>(
							memory.BaseAddress),
						memory.RegionSize,
						memory.State,
						memory.Type,
						memory.Protect);
				}
			}
		} catch (const std::exception& error) {
			logger::error(
				"World-only runtime preflight failed closed while resolving Address Library targets: {}",
				error.what());
			return false;
		} catch (...) {
			logger::error(
				"World-only runtime preflight failed closed while resolving Address Library targets");
			return false;
		}

		logger::info(
			"World-only OG 1.10.163 runtime target preflight passed (read-only; hooks remain disabled)");
		return true;
	}

	bool WorldOnlyScopeRenderer::InstallPassThroughHooks()
	{
		return InstallForwardingHooks(false, false);
	}

	bool WorldOnlyScopeRenderer::InstallObservationHooks()
	{
		return InstallForwardingHooks(true, false);
	}

	bool WorldOnlyScopeRenderer::InstallForwardingHooks(
		bool observationOnly,
		bool functional)
	{
		auto& state = GetState();
		if (state.installed.load(std::memory_order_acquire)) {
			if (functional) {
				return !state.passThroughOnly.load(
						   std::memory_order_acquire) &&
				       !state.permanentlyDisabled.load(
						   std::memory_order_acquire);
			}
			return state.passThroughOnly.load(std::memory_order_acquire) &&
			       state.observationOnly.load(
					   std::memory_order_acquire) == observationOnly;
		}

		state.requested.store(false, std::memory_order_release);
		state.outputReady.store(false, std::memory_order_release);
		state.permanentlyDisabled.store(true, std::memory_order_release);
		state.passThroughOnly.store(false, std::memory_order_release);
		state.observationOnly.store(
			observationOnly && !functional,
			std::memory_order_release);

		if (!ProbeRuntimeTargets()) {
			logger::error(
				"Stage 5b hook transaction failed closed because the OG target preflight failed");
			return false;
		}

		try {
			state.renderPreUITarget =
				reinterpret_cast<void*>(REL::ID(kRenderPreUIID).address());
			state.renderBatchesTarget =
				reinterpret_cast<void*>(REL::ID(kRenderBatchesID).address());
			state.doZPrePassTarget =
				reinterpret_cast<void*>(REL::ID(kDoZPrePassID).address());
			state.doUmbraQueryTarget =
				reinterpret_cast<void*>(REL::ID(kDoUmbraQueryID).address());
			if (observationOnly || functional) {
				state.firstPersonAccumulator =
					reinterpret_cast<RE::BSShaderAccumulator**>(
						REL::ID(kFirstPersonAccumulatorID).address());
				state.firstPersonZCount =
					reinterpret_cast<std::uint32_t*>(
						REL::ID(kFirstPersonZCountID).address());
				state.firstPersonAlphaZCount =
					reinterpret_cast<std::uint32_t*>(
						REL::ID(
							kFirstPersonAlphaZCountID)
							.address());
			}
		} catch (const std::exception& error) {
			logger::error(
				"Stage 5b hook transaction failed closed while resolving targets: {}",
				error.what());
			return false;
		} catch (...) {
			logger::error(
				"Stage 5b hook transaction failed closed while resolving targets");
			return false;
		}

		const bool prologuesMatch =
			MatchesKnownPrologue(
				"DrawWorld::Render_PreUI",
				state.renderPreUITarget,
				kRenderPreUIPrologue) &&
			MatchesKnownPrologue(
				"BSShaderAccumulator::RenderBatches",
				state.renderBatchesTarget,
				kRenderBatchesPrologue) &&
			(functional ||
				(MatchesKnownPrologue(
					 "Renderer::DoZPrePass",
					 state.doZPrePassTarget,
					 kDoZPrePassPrologue) &&
				 MatchesKnownPrologue(
					 "DrawWorld::DoUmbraQuery",
					 state.doUmbraQueryTarget,
					 kDoUmbraQueryPrologue)));
		if (!prologuesMatch) {
			return false;
		}

		const auto initializeStatus = MH_Initialize();
		if (initializeStatus != MH_OK &&
			initializeStatus != MH_ERROR_ALREADY_INITIALIZED) {
			logger::error(
				"Stage 5b hook transaction failed closed: MH_Initialize returned {}",
				MH_StatusToString(initializeStatus));
			return false;
		}

		struct HookBinding
		{
			const char* name;
			void* target;
			void* detour;
			void** original;
		};
		std::array<HookBinding, 4> hooks{
			HookBinding{
				"DrawWorld::Render_PreUI",
				state.renderPreUITarget,
				reinterpret_cast<void*>(&HookRenderPreUI),
				reinterpret_cast<void**>(&state.renderPreUIOriginal) },
			HookBinding{
				"BSShaderAccumulator::RenderBatches",
				state.renderBatchesTarget,
				reinterpret_cast<void*>(&HookRenderBatches),
				reinterpret_cast<void**>(&state.renderBatchesOriginal) },
			HookBinding{
				"Renderer::DoZPrePass",
				state.doZPrePassTarget,
				reinterpret_cast<void*>(&HookDoZPrePass),
				reinterpret_cast<void**>(&state.doZPrePassOriginal) },
			HookBinding{
				"DrawWorld::DoUmbraQuery",
				state.doUmbraQueryTarget,
				reinterpret_cast<void*>(&HookDoUmbraQuery),
				reinterpret_cast<void**>(&state.doUmbraQueryOriginal) }
		};
		const std::size_t hookCount = functional ? 2 : hooks.size();

		std::size_t createdCount = 0;
		auto rollback = [&]() noexcept {
			// A failed MH_ApplyQueued may have enabled only part of the set.
			// Keep forwarding mode published until every possibly live detour
			// is confirmed disabled and removed. If cleanup itself fails, keep
			// all surviving trampoline pointers callable and refuse re-entry.
			state.passThroughOnly.store(true, std::memory_order_release);
			bool cleanupSucceeded = true;

			for (std::size_t index = 0; index < createdCount; ++index) {
				const auto queueStatus =
					MH_QueueDisableHook(hooks[index].target);
				if (queueStatus != MH_OK &&
					queueStatus != MH_ERROR_DISABLED) {
					logger::error(
						"Stage 5b rollback could not queue-disable {}: {}",
						hooks[index].name,
						MH_StatusToString(queueStatus));
					cleanupSucceeded = false;
				}
			}

			const auto applyDisableStatus = MH_ApplyQueued();
			if (applyDisableStatus != MH_OK) {
				logger::error(
					"Stage 5b rollback could not apply the disable queue: {}",
					MH_StatusToString(applyDisableStatus));
				cleanupSucceeded = false;
			}

			for (std::size_t index = 0; index < createdCount; ++index) {
				const auto disableStatus =
					MH_DisableHook(hooks[index].target);
				const bool disabled =
					disableStatus == MH_OK ||
					disableStatus == MH_ERROR_DISABLED;
				if (!disabled) {
					logger::error(
						"Stage 5b rollback could not disable {}: {}",
						hooks[index].name,
						MH_StatusToString(disableStatus));
					cleanupSucceeded = false;
					continue;
				}

				const auto removeStatus =
					MH_RemoveHook(hooks[index].target);
				if (removeStatus == MH_OK ||
					removeStatus == MH_ERROR_NOT_CREATED) {
					*hooks[index].original = nullptr;
				} else {
					logger::error(
						"Stage 5b rollback could not remove {}: {}",
						hooks[index].name,
						MH_StatusToString(removeStatus));
					cleanupSucceeded = false;
				}
			}

			if (cleanupSucceeded) {
				state.passThroughOnly.store(false, std::memory_order_release);
				state.observationOnly.store(
					false,
					std::memory_order_release);
				state.installed.store(false, std::memory_order_release);
			} else {
				state.installed.store(true, std::memory_order_release);
				logger::error(
					"Stage 5b rollback was incomplete; forwarding mode remains latched and reinstallation is forbidden");
			}
			return cleanupSucceeded;
		};

		for (std::size_t index = 0; index < hookCount; ++index) {
			const auto& hook = hooks[index];
			const auto createStatus =
				MH_CreateHook(hook.target, hook.detour, hook.original);
			if (createStatus != MH_OK) {
				logger::error(
					"Stage 5b hook transaction failed closed while creating {}: {}",
					hook.name,
					MH_StatusToString(createStatus));
				rollback();
				return false;
			}
			++createdCount;
		}

		for (std::size_t index = 0; index < hookCount; ++index) {
			const auto& hook = hooks[index];
			const auto queueStatus = MH_QueueEnableHook(hook.target);
			if (queueStatus != MH_OK) {
				logger::error(
					"Stage 5b hook transaction failed closed while queuing {}: {}",
					hook.name,
					MH_StatusToString(queueStatus));
				rollback();
				return false;
			}
		}

		// Publish forwarding mode before the queued transaction becomes live.
		// No detour can observe the auxiliary path during the enable boundary.
		state.passThroughOnly.store(true, std::memory_order_release);
		const auto applyStatus = MH_ApplyQueued();
		if (applyStatus != MH_OK) {
			logger::error(
				"Stage 5b hook transaction failed closed while applying the complete queue: {}",
				MH_StatusToString(applyStatus));
			rollback();
			return false;
		}

		state.installed.store(true, std::memory_order_release);
		if (functional) {
			state.observationOnly.store(false, std::memory_order_release);
			state.permanentlyDisabled.store(
				false,
				std::memory_order_release);
			// Publish the functional mode last. Every live detour forwarded
			// exactly once until all runtime data and failure latches were
			// initialized.
			state.passThroughOnly.store(false, std::memory_order_release);
			logger::info(
				"Stage 5c installed the functional OG pre-first-person capture using only Render_PreUI and RenderBatches hooks");
		} else if (observationOnly) {
			logger::info(
				"Stage 5c-a installed the complete four-hook transaction in read-only observation mode; every detour still forwards its original exactly once");
		} else {
			logger::info(
				"Stage 5b installed the complete four-hook transaction in forwarding-only mode");
		}
		return true;
	}

	bool WorldOnlyScopeRenderer::InstallHooks()
	{
		return InstallForwardingHooks(false, true);
	}

	void WorldOnlyScopeRenderer::RequestFrame(bool requested) noexcept
	{
		auto& state = GetState();
		state.requested.store(requested, std::memory_order_release);
		if (!requested) {
			state.outputReady.store(false, std::memory_order_release);
			state.firstPersonCaptureClaimed.store(
				false,
				std::memory_order_release);
			state.phase.store(Phase::kIdle, std::memory_order_release);
		}
	}

	bool WorldOnlyScopeRenderer::CaptureBeforeFirstPersonDraw(
		ID3D11DeviceContext* context) noexcept
	{
		auto& state = GetState();
		if (!context ||
			!state.installed.load(std::memory_order_acquire) ||
			state.passThroughOnly.load(std::memory_order_acquire) ||
			state.permanentlyDisabled.load(std::memory_order_acquire) ||
			!state.requested.load(std::memory_order_acquire) ||
			g_firstPersonCaptureArmDepth == 0 ||
			g_firstPersonCaptureFrame == 0 ||
			g_firstPersonCaptureFrame !=
				state.captureFrameGeneration.load(
					std::memory_order_acquire) ||
			state.outputReady.load(std::memory_order_acquire)) {
			return false;
		}

		bool expected = false;
		if (!state.firstPersonCaptureClaimed.compare_exchange_strong(
				expected,
				true,
				std::memory_order_acq_rel,
				std::memory_order_acquire)) {
			return false;
		}

		bool published = false;
		try {
			do {
				if (!state.requested.load(std::memory_order_acquire) ||
					g_firstPersonCaptureArmDepth == 0 ||
					g_firstPersonCaptureFrame !=
						state.captureFrameGeneration.load(
							std::memory_order_acquire)) {
					break;
				}

				auto* rendererData = RE::BSGraphics::GetRendererData();
				auto* mainColor = rendererData ?
					reinterpret_cast<ID3D11Texture2D*>(
						rendererData
							->renderTargets[kMainColorTarget]
							.texture) :
					nullptr;
				if (!rendererData || !mainColor ||
					!IsQualifiedMainColorBoundary(
						context,
						mainColor)) {
					break;
				}

				ComPtr<ID3D11Device> contextDevice;
				ComPtr<ID3D11Device> resourceDevice;
				context->GetDevice(contextDevice.GetAddressOf());
				mainColor->GetDevice(resourceDevice.GetAddressOf());
				ComPtr<IUnknown> contextDeviceIdentity;
				ComPtr<IUnknown> resourceDeviceIdentity;
				if (!contextDevice || !resourceDevice ||
					FAILED(contextDevice.As(&contextDeviceIdentity)) ||
					FAILED(resourceDevice.As(&resourceDeviceIdentity)) ||
					contextDeviceIdentity.Get() !=
						resourceDeviceIdentity.Get()) {
					break;
				}

				std::scoped_lock lock(state.resourceMutex);
				if (!state.requested.load(std::memory_order_acquire) ||
					g_firstPersonCaptureArmDepth == 0 ||
					g_firstPersonCaptureFrame !=
						state.captureFrameGeneration.load(
							std::memory_order_acquire) ||
					!EnsureBoundaryCaptureResources(
						state,
						rendererData,
						contextDevice.Get()) ||
					!state.privateColor.texture ||
					!state.privateColor.shaderResourceView) {
					break;
				}

				context->CopyResource(
					state.privateColor.texture.Get(),
					mainColor);

				if (!state.requested.load(std::memory_order_acquire)) {
					break;
				}
				const auto generation =
					state.generation.fetch_add(
						1,
						std::memory_order_acq_rel) +
					1;
				state.outputReady.store(true, std::memory_order_release);
				state.phase.store(
					Phase::kPrimaryEligible,
					std::memory_order_release);
				g_renderPhase = Phase::kPrimaryEligible;
				g_renderPhaseGeneration = generation;
				published = true;
			} while (false);
		} catch (...) {
			// Never let allocation, mutex or COM failures escape the game's
			// DrawIndexed boundary. This frame simply retains ordinary STS.
		}

		if (!published) {
			state.firstPersonCaptureClaimed.store(
				false,
				std::memory_order_release);
		}
		return published;
	}

	bool WorldOnlyScopeRenderer::IsInstalled() const noexcept
	{
		return GetState().installed.load(std::memory_order_acquire);
	}

	bool WorldOnlyScopeRenderer::IsOutputReady() const noexcept
	{
		const auto& state = GetState();
		return state.phase.load(std::memory_order_acquire) ==
		           Phase::kPrimaryEligible &&
		       state.outputReady.load(std::memory_order_acquire) &&
		       state.generation.load(std::memory_order_acquire) != 0;
	}

	std::uint64_t WorldOnlyScopeRenderer::GetOutputGeneration() const noexcept
	{
		return IsOutputReady() ?
		           GetState().generation.load(std::memory_order_acquire) :
		           0;
	}

	WorldOnlyScopeRenderer::PhaseToken
		WorldOnlyScopeRenderer::GetPhaseToken() const noexcept
	{
		const auto phase =
			GetState().phase.load(std::memory_order_acquire);
		return PhaseToken{
			.phase = phase,
			.generation =
				phase == Phase::kPrimaryEligible ?
					GetState().generation.load(
						std::memory_order_acquire) :
					0
		};
	}

	ID3D11ShaderResourceView*
		WorldOnlyScopeRenderer::AcquireColorSRV(PhaseToken token) noexcept
	{
		auto& state = GetState();
		if (token.phase != Phase::kPrimaryEligible ||
			g_renderPhase != Phase::kPrimaryEligible ||
			token.generation != g_renderPhaseGeneration ||
			state.phase.load(std::memory_order_acquire) !=
				Phase::kPrimaryEligible ||
			token.generation == 0) {
			return nullptr;
		}
		try {
			std::scoped_lock lock(state.resourceMutex);
			if (!state.outputReady.load(std::memory_order_acquire) ||
				state.phase.load(std::memory_order_acquire) !=
					Phase::kPrimaryEligible ||
				state.generation.load(std::memory_order_acquire) !=
					token.generation ||
				!state.privateColor.shaderResourceView) {
				return nullptr;
			}
			auto* result = state.privateColor.shaderResourceView.Get();
			result->AddRef();
			return result;
		} catch (...) {
			// A lock failure must never escape a DrawIndexed hook boundary.
			return nullptr;
		}
	}

	ID3D11ShaderResourceView*
		WorldOnlyScopeRenderer::AcquireDepthSRV(PhaseToken token) noexcept
	{
		auto& state = GetState();
		if (token.phase != Phase::kPrimaryEligible ||
			g_renderPhase != Phase::kPrimaryEligible ||
			token.generation != g_renderPhaseGeneration ||
			state.phase.load(std::memory_order_acquire) !=
				Phase::kPrimaryEligible ||
			token.generation == 0) {
			return nullptr;
		}
		try {
			std::scoped_lock lock(state.resourceMutex);
			if (!state.outputReady.load(std::memory_order_acquire) ||
				state.phase.load(std::memory_order_acquire) !=
					Phase::kPrimaryEligible ||
				state.generation.load(std::memory_order_acquire) !=
					token.generation ||
				!state.privateDepth.depthShaderResourceView) {
				return nullptr;
			}
			auto* result = state.privateDepth.depthShaderResourceView.Get();
			result->AddRef();
			return result;
		} catch (...) {
			// A lock failure must never escape a DrawIndexed hook boundary.
			return nullptr;
		}
	}
}
