#pragma once

#include <cstdint>
#include <d3d11.h>

namespace MagnaScope
{
	// Captures the ordinary primary RT4 color immediately before the first
	// draw submitted by the first-person accumulator. The snapshot therefore
	// contains the current-frame world but not the weapon or hands. The
	// resulting color SRV is intended to become the source sampled by the
	// authored ScopeFade draw later in that same first-person batch.
	//
	// This component is deliberately independent of the existing ScopeFade
	// replacement. Installing its hooks does not request auxiliary rendering;
	// a caller must publish RequestFrame(true) while an eligible scope is
	// active. This separation lets rollout prove the render source before it
	// changes the already verified aperture draw.
	class WorldOnlyScopeRenderer final
	{
	public:
		enum class Phase : std::uint8_t
		{
			kIdle,
			// Retained only so older call sites fail closed while the removed
			// second-Render_PreUI prototype remains dormant in this TU.
			kAuxiliary,
			// The current-frame pre-first-person snapshot is available to a
			// later DrawIndexed consumer.
			kPrimaryEligible
		};

		struct PhaseToken
		{
			Phase phase = Phase::kIdle;
			std::uint64_t generation = 0;
		};

		static WorldOnlyScopeRenderer& GetSingleton() noexcept;

		// Performs a read-only OG 1.10.163 preflight over every engine target
		// required by the proposed auxiliary renderer. It resolves the Address
		// Library IDs, verifies their exact known RVAs, checks loaded-image page
		// ownership/protection/alignment, and logs only the first 16 bytes of
		// each function. It never writes, calls, or hooks an engine target.
		[[nodiscard]] bool ProbeRuntimeTargets();

		// Installs the complete four-hook OG transaction after exact prologue
		// verification, but forces every detour to forward directly to its
		// original. This is the Stage 5b ownership and ABI verification mode.
		// It creates no renderer resources and cannot request an auxiliary pass.
		[[nodiscard]] bool InstallPassThroughHooks();

		// Installs the same verified four-hook transaction as Stage 5b, but
		// adds read-only Stage 5c-a telemetry around each forwarded call. No
		// engine value, D3D binding, renderer resource, or displayed pixel is
		// changed. The observations establish callback ordering, worker
		// quiescence, first-person accumulator identity, and D3D entry-state
		// constraints before an auxiliary render is allowed to exist.
		[[nodiscard]] bool InstallObservationHooks();

		// Installs only the two OG hooks required by the functional capture:
		// Render_PreUI establishes the frame/accumulator identity and
		// RenderBatches arms the exact first-person boundary. No second world
		// render, culling mutation, Z-count mutation, or Umbra hook is active.
		// Every unsupported or ambiguous condition fails closed to ordinary STS
		// rendering.
		[[nodiscard]] bool InstallHooks();

		// Requests or cancels one ordinary-frame boundary capture. This is an
		// atomic handoff; callers may publish from the game thread.
		void RequestFrame(bool requested) noexcept;

		// Captures RT4 while the verified first-person RenderBatches invocation
		// is active. The RenderBatches entry calls this with
		// requireLiveBinding=false because that hook itself is the authoritative
		// pre-first-person boundary; RT4 need not still be an OM output before
		// the batch binds its first material. DrawIndexed fallbacks retain the
		// stricter live-binding check. No D3D binding is changed. Returns true
		// only when this call completed the capture.
		[[nodiscard]] bool CaptureBeforeFirstPersonDraw(
			ID3D11DeviceContext* context,
			bool requireLiveBinding = true) noexcept;

		[[nodiscard]] bool IsInstalled() const noexcept;

		// These return the current-frame pre-first-person snapshot. The output
		// is invalidated at the beginning of the next ordinary Render_PreUI
		// invocation or when RequestFrame(false) is published.
		[[nodiscard]] bool IsOutputReady() const noexcept;
		[[nodiscard]] std::uint64_t GetOutputGeneration() const noexcept;

		// kPrimaryEligible means the exact D3D boundary capture succeeded for
		// the current ordinary frame. kAuxiliary is never entered by the
		// functional implementation.
		[[nodiscard]] PhaseToken GetPhaseToken() const noexcept;

		// Returns an AddRef'd SRV only when token exactly matches the published
		// current-frame output generation. The caller owns one COM reference
		// and must Release it promptly. AddRef under the resource mutex prevents
		// resize or rebuild from invalidating the acquired resource.
		[[nodiscard]] ID3D11ShaderResourceView* AcquireColorSRV(
			PhaseToken token) noexcept;

		// Same ownership contract as AcquireColorSRV. The functional capture is
		// deliberately color-only, so this returns null. It remains in the API
		// only to keep dormant observation/prototype call sites fail-closed.
		[[nodiscard]] ID3D11ShaderResourceView* AcquireDepthSRV(
			PhaseToken token) noexcept;

	private:
		[[nodiscard]] bool InstallForwardingHooks(
			bool observationOnly,
			bool functional = false);

		WorldOnlyScopeRenderer() = default;
		~WorldOnlyScopeRenderer() = default;
		WorldOnlyScopeRenderer(const WorldOnlyScopeRenderer&) = delete;
		WorldOnlyScopeRenderer& operator=(const WorldOnlyScopeRenderer&) = delete;
	};
}
