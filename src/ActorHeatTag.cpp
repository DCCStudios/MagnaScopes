#include "PCH.h"
#include "ActorHeatTag.h"

#include <MinHook.h>

#include <array>
#include <shared_mutex>
#include <unordered_set>
#include <vector>

namespace MagnaScope::ActorHeatTag
{
	namespace
	{
		// Minimal view of the engine render-pass; only geometry @ 0x18 is read.
		// Layout verified against ShaderEngineCS's BSRenderPassLayout.
		struct RenderPass
		{
			void*           next;            // 00
			void*           shader;          // 08
			void*           shaderProperty;  // 10
			RE::BSGeometry* geometry;        // 18
		};
		static_assert(offsetof(RenderPass, geometry) == 0x18);

		// Wrapper object a recorded command-buffer SRV entry points at; the
		// engine's replay reads the SRV pointer from +0x08 (kind = 0 path).
		// Layout mirrors ShaderEngineCS's FakeStructuredResource, proven in
		// game on this exact runtime.
		struct alignas(16) TagWrapper
		{
			void*                                   pad00 = nullptr;
			std::atomic<ID3D11ShaderResourceView*>  srv{ nullptr };
			char                                    pad10[0x20] = {};
			std::uint32_t                           fenceCount = 0;
			char                                    pad34[12] = {};
		};
		static_assert(sizeof(TagWrapper) == 64);
		static_assert(offsetof(TagWrapper, srv) == 0x08);
		static_assert(offsetof(TagWrapper, fenceCount) == 0x30);

		// Recorded entry layout ProcessCommandBuffer iterates as a 16-byte
		// table inside each command buffer (ShaderEngineCS-verified).
		struct CBShaderResource
		{
			void*         resourceWrapper;  // +0x00
			std::uint8_t  slot;             // +0x08
			std::uint8_t  stage;            // +0x09 (0=VS, 1=PS)
			std::uint8_t  kind;             // +0x0A (0 = wrapper+0x08 path)
			std::uint8_t  pad0B = 0;
			std::uint32_t pad0C = 0;
		};
		static_assert(sizeof(CBShaderResource) == 16);

		// SetupGeometry/RestoreGeometry are member functions; the vtable thunk
		// passes `this` (the BSShader*) as the first argument.
		using GeomFn = void (*)(RE::BSShader*, RenderPass*);
		GeomFn g_origLightSetup = nullptr;
		GeomFn g_origLightRestore = nullptr;
		GeomFn g_origEffectSetup = nullptr;
		GeomFn g_origEffectRestore = nullptr;

		// BSBatchRenderer::Draw(pass, ...) -- the per-geometry chokepoint of the
		// IMMEDIATE draw path (gore caps, FX, some skinned passes).
		using BatchDrawFn = void (*)(RenderPass*, std::uintptr_t, std::uintptr_t, void*);
		BatchDrawFn g_origBatchDraw = nullptr;

		// BSShader::BuildCommandBuffer -- records a pass into an engine command
		// buffer (param carries the geometry and an SRV table we extend).
		using BuildCommandBufferFn = char* (*)(void*, void*, void*);
		BuildCommandBufferFn g_origBuildCommandBuffer = nullptr;

		// Renderer::ProcessCommandBuffer -- replays recorded buffers on the
		// render thread; bracketed so DrawIndexed-time code knows the recorded
		// tag SRV (not the thread_local) is the valid classification signal.
		using ProcessCommandBufferFn = void (*)(void*, void*);
		ProcessCommandBufferFn g_origProcessCommandBuffer = nullptr;

		std::shared_mutex g_setLock;
		// Live actor references (as raw pointer VALUES, never dereferenced on the
		// render thread). Identity history: matching collected BSGeometry*
		// pointers tagged nothing (the renderer submits different instances than
		// actor->Get3D() exposes), and matching geometry NAMES both over-tagged
		// (a dead creature sharing a live one's mesh name lit up) and
		// under-covered (Dogmeat's drawn parts never matched). The engine itself
		// stamps scene-graph nodes with their owning TESObjectREFR* in
		// NiAVObject::userData, so the drawn geometry's own ancestry names its
		// actor exactly -- classification walks up to that and compares here.
		std::unordered_set<std::uintptr_t> g_actorRefs;

		std::atomic_bool g_active{ false };
		std::atomic_uint64_t g_taggedDraws{ 0U };
		std::atomic_uint64_t g_replayTagged{ 0U };
		std::atomic_uint64_t g_batchDraws{ 0U };
		std::atomic_uint64_t g_cbActor{ 0U };
		std::atomic_uint64_t g_cbNeutral{ 0U };

		// Command-buffer tag plumbing. The actor wrapper's SRV is a 1x1 R8
		// dummy created once from the game's device; its POINTER IDENTITY is the
		// signal (nothing samples it). The neutral wrapper stays null so every
		// non-actor pass still re-binds the slot at replay -- without that, a
		// stale actor bind would leak onto following draws.
		TagWrapper g_actorWrapper;
		TagWrapper g_neutralWrapper;
		std::atomic<ID3D11Device*> g_tagDevice{ nullptr };
		std::atomic<ID3D11ShaderResourceView*> g_actorTagSRV{ nullptr };

		// Per-render-thread tag state. SetupGeometry can nest (a pass drawing a
		// sub-geometry), so the previous value is stacked and restored.
		thread_local bool tls_current = false;
		thread_local std::vector<bool> tls_stack;
		// Depth of ProcessCommandBuffer on this thread; > 0 means the current
		// DrawIndexed comes from a recorded replay.
		thread_local int tls_replayDepth = 0;

		// Ask a scene-graph node who owns it: walk the parent chain to the first
		// node carrying a userData ref stamp. Dereferencing the chain is safe
		// while the engine is actively drawing/recording the subtree (it holds
		// it alive). The walk is bounded in case of a cyclic/corrupt chain.
		bool IsActorNode(RE::NiAVObject* node)
		{
			std::uintptr_t owner = 0;
			for (int depth = 0; node && depth < 16; node = node->parent, ++depth) {
				if (node->userData != 0) {
					owner = node->userData;
					break;
				}
			}
			if (owner == 0) {
				return false;
			}
			std::shared_lock lock(g_setLock);
			return g_actorRefs.contains(owner);
		}

		bool Classify(RenderPass* pass)
		{
			return pass ? IsActorNode(pass->geometry) : false;
		}

		// Record-time classifier for BuildCommandBuffer. Uses the owner ref's
		// own form type instead of the live-actor set: during a save load an
		// actor's buffers can be recorded BEFORE he reaches the high-process
		// list the set is built from, which would bake him in as neutral until
		// his buffers happen to rebuild. Dereferencing the owner is safe HERE
		// (the engine is actively recording the subtree, so the ref is alive);
		// the draw-time paths keep the compare-only set. Dead actors record as
		// actor -- a warm corpse until the buffer rebuilds is acceptable
		// thermal behavior. The player is excluded (scoped view is first
		// person; this also keeps viewmodel arms out of the mask).
		bool IsActorNodeAtRecord(RE::NiAVObject* node)
		{
			std::uintptr_t owner = 0;
			for (int depth = 0; node && depth < 16; node = node->parent, ++depth) {
				if (node->userData != 0) {
					owner = node->userData;
					break;
				}
			}
			if (owner == 0) {
				return false;
			}
			auto* ref = reinterpret_cast<RE::TESObjectREFR*>(owner);
			if (ref == static_cast<RE::TESObjectREFR*>(
						   RE::PlayerCharacter::GetSingleton())) {
				return false;
			}
			return ref->GetFormType() == RE::ENUM_FORM_ID::kACHR;
		}

		void Push(RenderPass* pass)
		{
			tls_stack.push_back(tls_current);
			tls_current = g_active.load(std::memory_order_relaxed) ?
			                  Classify(pass) :
			                  false;
		}

		void Pop()
		{
			if (!tls_stack.empty()) {
				tls_current = tls_stack.back();
				tls_stack.pop_back();
			} else {
				tls_current = false;
			}
		}

		void HookLightSetup(RE::BSShader* a_shader, RenderPass* a_pass)
		{
			Push(a_pass);
			g_origLightSetup(a_shader, a_pass);
		}
		void HookLightRestore(RE::BSShader* a_shader, RenderPass* a_pass)
		{
			g_origLightRestore(a_shader, a_pass);
			Pop();
		}
		void HookEffectSetup(RE::BSShader* a_shader, RenderPass* a_pass)
		{
			Push(a_pass);
			g_origEffectSetup(a_shader, a_pass);
		}
		void HookEffectRestore(RE::BSShader* a_shader, RenderPass* a_pass)
		{
			g_origEffectRestore(a_shader, a_pass);
			Pop();
		}

		// Immediate-path bracket: carries tls_current across the draw this call
		// issues, into the D3D11 DrawIndexed detour. Gated on g_active so it is
		// a single atomic load + one call when vision is off.
		void HookBatchDraw(
			RenderPass* a_pass,
			std::uintptr_t a_arg2,
			std::uintptr_t a_arg3,
			void* a_dynamicDrawData)
		{
			if (!g_active.load(std::memory_order_relaxed)) {
				g_origBatchDraw(a_pass, a_arg2, a_arg3, a_dynamicDrawData);
				return;
			}
			g_batchDraws.fetch_add(1U, std::memory_order_relaxed);
			Push(a_pass);
			g_origBatchDraw(a_pass, a_arg2, a_arg3, a_dynamicDrawData);
			Pop();
		}

		// Command-buffer record hook. Skinned actor bodies are the main cargo of
		// this path -- verified in game: an aimed companion's mesh never crossed
		// the batch chokepoint while gore caps and FX did. There is no per-pass
		// CPU call at replay, so the classification must ride inside the buffer
		// as a recorded binding: classify the pass geometry NOW (userData owner
		// walk) and append one SRV record at kHeatTagSlot -- the actor tag SRV
		// for actor-owned geometry, null otherwise. Injected for EVERY pass so
		// the slot is freshly re-bound per replayed draw.
		//
		// Field offsets in the param (IDA-verified by ShaderEngineCS, running in
		// game alongside this plugin): +0x00 BSGeometry*, +0x14 srvCount,
		// +0x38 srvSrc. The engine memcpy's 16*srvCount from srvSrc into the
		// buffer during the original call, so a stack-extended copy is safe and
		// the mutation is restored immediately after.
		char* HookedBuildCommandBuffer(void* a_this, void* a_param, void* a_memCtx)
		{
			ID3D11ShaderResourceView* actorSRV =
				g_actorTagSRV.load(std::memory_order_acquire);
			if (!a_param || !actorSRV) {
				return g_origBuildCommandBuffer(a_this, a_param, a_memCtx);
			}

			auto* bytes = reinterpret_cast<char*>(a_param);
			auto* geom = *reinterpret_cast<RE::NiAVObject**>(bytes);
			auto& srvCount = *reinterpret_cast<std::uint32_t*>(bytes + 0x14);
			auto& srvSrc =
				*reinterpret_cast<const CBShaderResource**>(bytes + 0x38);

			const std::uint32_t origCount = srvCount;
			const CBShaderResource* origSrc = srvSrc;
			constexpr std::size_t kMaxSRV = 32;
			if ((origCount && !origSrc) || origCount >= kMaxSRV) {
				return g_origBuildCommandBuffer(a_this, a_param, a_memCtx);
			}

			const bool isActor = IsActorNodeAtRecord(geom);

			std::array<CBShaderResource, kMaxSRV + 1> extended{};
			if (origCount) {
				std::memcpy(
					extended.data(),
					origSrc,
					origCount * sizeof(CBShaderResource));
			}
			CBShaderResource& rec = extended[origCount];
			rec.resourceWrapper = isActor ?
			                          static_cast<void*>(&g_actorWrapper) :
			                          static_cast<void*>(&g_neutralWrapper);
			rec.slot = static_cast<std::uint8_t>(kHeatTagSlot);
			rec.stage = 1;  // PS
			rec.kind = 0;   // engine reads the SRV from wrapper+0x08

			srvCount = origCount + 1U;
			srvSrc = extended.data();
			char* result = g_origBuildCommandBuffer(a_this, a_param, a_memCtx);
			srvCount = origCount;
			srvSrc = origSrc;

			(isActor ? g_cbActor : g_cbNeutral)
				.fetch_add(1U, std::memory_order_relaxed);
			return result;
		}

		void HookedProcessCommandBuffer(void* a_renderer, void* a_cbData)
		{
			++tls_replayDepth;
			g_origProcessCommandBuffer(a_renderer, a_cbData);
			--tls_replayDepth;
		}

		bool InstallDetour(
			const char* a_name,
			std::uintptr_t a_address,
			void* a_hook,
			void** a_original)
		{
			void* target = reinterpret_cast<void*>(a_address);
			if (MH_CreateHook(target, a_hook, a_original) != MH_OK ||
				MH_EnableHook(target) != MH_OK) {
				logger::error("[heatmask] failed to hook {} at {:p}", a_name, target);
				return false;
			}
			logger::info("[heatmask] {} hooked at {:p}", a_name, target);
			return true;
		}
	}

	void Install()
	{
		// Once only: kGameDataReady can fire more than once, and re-running
		// write_vfunc would capture our own thunk as the "original", recursing.
		static std::atomic_bool installed{ false };
		bool expected = false;
		if (!installed.compare_exchange_strong(expected, true)) {
			return;
		}

		REL::Relocation<std::uintptr_t> lightVT{ RE::VTABLE::BSLightingShader[0] };
		g_origLightSetup = reinterpret_cast<GeomFn>(
			lightVT.write_vfunc(7, reinterpret_cast<void*>(&HookLightSetup)));
		g_origLightRestore = reinterpret_cast<GeomFn>(
			lightVT.write_vfunc(8, reinterpret_cast<void*>(&HookLightRestore)));

		REL::Relocation<std::uintptr_t> effectVT{ RE::VTABLE::BSEffectShader[0] };
		g_origEffectSetup = reinterpret_cast<GeomFn>(
			effectVT.write_vfunc(7, reinterpret_cast<void*>(&HookEffectSetup)));
		g_origEffectRestore = reinterpret_cast<GeomFn>(
			effectVT.write_vfunc(8, reinterpret_cast<void*>(&HookEffectRestore)));

		logger::info(
			"[heatmask] actor-tagging vtable hooks installed (BSLightingShader "
			"+ BSEffectShader SetupGeometry/RestoreGeometry)");

		// The three function detours below carry OG-only Address Library ids
		// (second slots unmapped); the vtable hooks above stay active on all
		// runtimes as a best-effort fallback.
		if (!REX::FModule::IsRuntimeOG()) {
			logger::info(
				"[heatmask] draw-path detours skipped (non-OG runtime)");
			return;
		}

		const auto minHookStatus = MH_Initialize();
		if (minHookStatus != MH_OK && minHookStatus != MH_ERROR_ALREADY_INITIALIZED) {
			logger::error(
				"[heatmask] MinHook init failed ({}); draw-path detours not "
				"installed -- the heat mask will stay empty",
				static_cast<int>(minHookStatus));
			return;
		}

		// Immediate path (gore caps, FX, some skinned passes).
		const REL::Relocation<std::uintptr_t> batchDraw{ REL::ID(1152191) };
		InstallDetour(
			"BSBatchRenderer::Draw",
			batchDraw.address(),
			reinterpret_cast<void*>(&HookBatchDraw),
			reinterpret_cast<void**>(&g_origBatchDraw));

		// Command-buffer record + replay pair (the main skinned-body route).
		// Both or neither: recording tags without replay detection would do
		// nothing; replay detection without recorded tags would misread stale
		// slot contents.
		const REL::Relocation<std::uintptr_t> buildCB{ REL::ID(833764) };
		const REL::Relocation<std::uintptr_t> processCB{ REL::ID(673619) };
		if (InstallDetour(
				"BSShader::BuildCommandBuffer",
				buildCB.address(),
				reinterpret_cast<void*>(&HookedBuildCommandBuffer),
				reinterpret_cast<void**>(&g_origBuildCommandBuffer))) {
			InstallDetour(
				"Renderer::ProcessCommandBuffer",
				processCB.address(),
				reinterpret_cast<void*>(&HookedProcessCommandBuffer),
				reinterpret_cast<void**>(&g_origProcessCommandBuffer));
		}

		// Mint the tag SRV NOW, from the engine's own device. Command buffers
		// are recorded once and cached: in the first in-game run the SRV only
		// became ready at renderer init, AFTER the save load had already built
		// the whole scene's buffers -- so nothing pre-built ever carried a tag
		// record and taggedDraws stayed 0 on the aimed companion. kGameDataReady
		// fires at the main menu, before any save loads.
		if (auto* rendererData = RE::BSGraphics::GetRendererData();
			rendererData && rendererData->device) {
			SetTagDevice(reinterpret_cast<ID3D11Device*>(rendererData->device));
		} else {
			logger::warn(
				"[heatmask] renderer device unavailable at install; tag SRV "
				"deferred to renderer init (pre-built buffers stay untagged)");
		}
	}

	void SetTagDevice(ID3D11Device* device)
	{
		if (!device || g_actorTagSRV.load(std::memory_order_acquire)) {
			return;
		}
		// 1x1 R8 dummy whose SRV pointer is the actor-tag identity. Never
		// released: recorded command buffers may reference the wrapper (and
		// through it this SRV) for the life of the process.
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = 1;
		desc.Height = 1;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_IMMUTABLE;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		const std::uint8_t whitePixel = 0xFFU;
		D3D11_SUBRESOURCE_DATA init{};
		init.pSysMem = &whitePixel;
		init.SysMemPitch = 1;

		ID3D11Texture2D* texture = nullptr;
		if (FAILED(device->CreateTexture2D(&desc, &init, &texture)) || !texture) {
			logger::error("[heatmask] actor-tag texture creation failed");
			return;
		}
		ID3D11ShaderResourceView* srv = nullptr;
		const HRESULT hr = device->CreateShaderResourceView(texture, nullptr, &srv);
		texture->Release();
		if (FAILED(hr) || !srv) {
			logger::error("[heatmask] actor-tag SRV creation failed");
			return;
		}
		g_actorWrapper.srv.store(srv, std::memory_order_release);
		g_actorTagSRV.store(srv, std::memory_order_release);
		g_tagDevice.store(device, std::memory_order_release);
		logger::info(
			"[heatmask] actor-tag SRV ready at {} (slot t{})",
			static_cast<void*>(srv),
			kHeatTagSlot);
	}

	void RebuildActorGeometrySet()
	{
		auto* processLists = RE::ProcessLists::GetSingleton();
		if (!processLists) {
			return;
		}
		auto* player = RE::PlayerCharacter::GetSingleton();

		std::unordered_set<std::uintptr_t> fresh;
		for (const RE::ActorHandle& handle : processLists->highActorHandles) {
			const RE::NiPointer<RE::Actor> actorPtr = handle.get();
			RE::Actor* actor = actorPtr.get();
			if (!actor || actor == player || actor->IsPlayerRef()) {
				continue;
			}
			if (actor->IsDead(false)) {
				continue;
			}
			// The engine stamps the ref pointer into NiAVObject::userData across
			// the actor's 3D, so the owner lookups compare against the
			// TESObjectREFR base pointer value.
			fresh.insert(reinterpret_cast<std::uintptr_t>(
				static_cast<RE::TESObjectREFR*>(actor)));
		}

		std::unique_lock lock(g_setLock);
		g_actorRefs.swap(fresh);
	}

	void SetActive(bool enabled)
	{
		g_active.store(enabled, std::memory_order_relaxed);
	}

	bool CurrentDrawIsActor(ID3D11DeviceContext* context)
	{
		if (!g_active.load(std::memory_order_relaxed)) {
			return false;
		}
		// Immediate path: the batch/vtable bracket around this draw.
		if (tls_current) {
			g_taggedDraws.fetch_add(1U, std::memory_order_relaxed);
			return true;
		}
		// Replay path: the recorded tag binding at kHeatTagSlot. Only trusted
		// while ProcessCommandBuffer is on this thread's stack -- outside
		// replay the slot holds stale leftovers.
		if (tls_replayDepth > 0 && context) {
			ID3D11ShaderResourceView* actorSRV =
				g_actorTagSRV.load(std::memory_order_acquire);
			if (!actorSRV) {
				return false;
			}
			ID3D11ShaderResourceView* bound = nullptr;
			context->PSGetShaderResources(kHeatTagSlot, 1, &bound);
			const bool isActor = bound == actorSRV;
			if (bound) {
				bound->Release();
			}
			if (isActor) {
				g_taggedDraws.fetch_add(1U, std::memory_order_relaxed);
				g_replayTagged.fetch_add(1U, std::memory_order_relaxed);
				return true;
			}
		}
		return false;
	}

	DiagSnapshot ReadDiag()
	{
		std::size_t size = 0U;
		{
			std::shared_lock lock(g_setLock);
			size = g_actorRefs.size();
		}
		return DiagSnapshot{
			g_taggedDraws.exchange(0U, std::memory_order_relaxed),
			g_replayTagged.exchange(0U, std::memory_order_relaxed),
			g_batchDraws.exchange(0U, std::memory_order_relaxed),
			g_cbActor.exchange(0U, std::memory_order_relaxed),
			g_cbNeutral.exchange(0U, std::memory_order_relaxed),
			size
		};
	}
}
