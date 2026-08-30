#pragma once

#include <d3d11.h>

// Stage 1 of the heat-mask subsystem: tag actor/character draws so a later
// stage can render their true silhouettes into a heat mask.
//
// Identity: the drawn geometry names its own actor. The engine stamps
// scene-graph nodes with their owning TESObjectREFR* (NiAVObject::userData),
// so classification walks the geometry's parent chain to the first stamped
// node and pointer-compares that ref against a set of live actor refs built on
// the game thread. Geometry-pointer and geometry-name matching both failed in
// game (renderer submits different instances; names collide across instances
// and under-cover skinned parts).
//
// Coverage requires TWO routes, because Fallout 4 issues draws two ways:
//
//  1. Immediate path (gore caps, FX, some skinned passes):
//     BSBatchRenderer::Draw is detoured and brackets a thread_local flag
//     across the draw it issues (SetupGeometry/RestoreGeometry vtable hooks
//     remain as a fallback bracket).
//
//  2. Command-buffer path (the main skinned-body route -- verified in game:
//     an aimed companion's mesh never crossed the batch chokepoint):
//     draws are RECORDED by BSShader::BuildCommandBuffer and REPLAYED later
//     by ProcessCommandBuffer with no per-pass CPU call. The only per-draw
//     signal that survives into the replay is a recorded GPU binding, so the
//     BuildCommandBuffer hook classifies the pass geometry at record time and
//     appends one extra SRV record (kHeatTagSlot): the actor tag SRV for
//     actor-owned geometry, a null SRV otherwise (every pass gets a record so
//     the slot is freshly re-bound per replayed draw -- no staleness). At
//     DrawIndexed time, while a ProcessCommandBuffer hook's thread_local says
//     replay is active, the CPU reads the slot back and pointer-compares.
//     Layouts (param +0x00 geometry / +0x14 srvCount / +0x38 srvSrc; 16-byte
//     record; wrapper SRV at +0x08) match ShaderEngineCS, proven in game.
//
// Only pointer VALUES cross threads for comparison -- the actor refs are never
// dereferenced on the render thread -- so a stale ref in the set is harmless.

namespace MagnaScope::ActorHeatTag
{
	// PS SRV slot the command-buffer tag record binds. ShaderEngineCL uses 26
	// for its own draw-tag; stay clear of it.
	inline constexpr UINT kHeatTagSlot = 27;

	// Install all hooks (vtable brackets on every runtime; BSBatchRenderer::
	// Draw + BuildCommandBuffer + ProcessCommandBuffer detours on OG). Call
	// once, after the game's vtables exist.
	void Install();

	// D3D device used to lazily create the 1x1 actor-tag SRV. Call from the
	// renderer init once the device exists; command-buffer tagging stays
	// dormant until then.
	void SetTagDevice(ID3D11Device* device);

	// Game thread: rebuild the live-actor ref set from currently-loaded
	// high-process actors. Call throttled while a vision scope is active.
	void RebuildActorGeometrySet();

	// Cheap global gate: when false, the per-draw tag classification is skipped
	// entirely (near-zero cost when no vision scope is up).
	void SetActive(bool enabled);

	// Render thread, inside a draw: is the draw currently being issued one of a
	// tagged actor's geometries? Replay draws are judged by the recorded tag
	// SRV bound at kHeatTagSlot; immediate draws by the thread_local bracket.
	// Increments the diagnostic counter when true.
	[[nodiscard]] bool CurrentDrawIsActor(ID3D11DeviceContext* context);

	// Verification counters since the last read (which resets them). setSize is
	// the current live-actor ref count. cbActor/cbNeutral count command-buffer
	// records injected per classification; replayTagged counts draws identified
	// through the replay-SRV route (vs taggedDraws = all identified draws).
	struct DiagSnapshot
	{
		std::uint64_t taggedDraws;
		std::uint64_t replayTagged;
		std::uint64_t batchDraws;
		std::uint64_t cbActor;
		std::uint64_t cbNeutral;
		std::size_t setSize;
	};
	[[nodiscard]] DiagSnapshot ReadDiag();
}
