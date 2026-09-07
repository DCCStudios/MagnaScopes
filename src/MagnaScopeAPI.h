#pragma once

#include <cstdint>

// MagnaScope's plugin interface.
//
// This header is the whole contract. Copy it into your plugin; do not link
// against MagnaScope.
//
// Obtain the interface by listening for MagnaScope's kPostLoad dispatch:
//
//     if (msg->type == MagnaScopeAPI::kInterfaceMessage &&
//         msg->sender && std::string_view(msg->sender) == "MagnaScope") {
//         auto* api = static_cast<const MagnaScopeAPI::InterfaceV1*>(msg->data);
//         if (api && api->version >= 1) { ... }
//     }
//
// What this is for: retargeting which geometry MagnaScope treats as the
// aperture and the reticle, and triggering its secondary-sight swap. The
// motivating case is a double-pip optic -- two sights on one NIF, because
// putting two zoom-data-bearing attachments on one weapon misbehaves in the
// engine, so it has to be a single mesh.
//
// Threading and lifetime rules, which the implementation enforces rather than
// merely documents:
//
//   * Setters queue. They may be called from any thread; the work runs on the
//     game thread on the next update.
//   * A BSTriShape* you pass is resolved to its node name inside the call and
//     never retained. MagnaScope therefore cannot outlive your pointer, and a
//     3D rebuild between your call and the next frame resolves harmlessly to
//     "shape not found" instead of a use-after-free.
//   * Getters copy into your buffer. No engine pointer is ever handed out.
namespace MagnaScopeAPI
{
	// Sent at kPostLoad with sender "MagnaScope" and data pointing at an
	// InterfaceV1.
	inline constexpr std::uint32_t kInterfaceMessage = 'MGSI';

	struct NodeInfoV1
	{
		char name[128];
		// Index into the same array, or -1 for the root.
		std::int32_t parentIndex;
		float localTranslation[3];
		float localRotation[9];
		float localScale;
		// Non-zero when the node is culled (not drawn).
		std::uint32_t culled;
		// Non-zero when this node is a BSTriShape rather than a plain node.
		std::uint32_t isShape;
	};

	struct ScopeInfoV1
	{
		char weaponPlugin[128];
		std::uint32_t weaponFormID;
		char omodKey[256];
		char apertureSurface[128];
		char reticleSurface[128];
		float magnification;
		// -1 when the primary optic is up.
		std::int32_t secondarySightIndex;
		std::int32_t secondarySightCount;
		// -1 selects the authored 3D reticle.
		std::int32_t reticleIndex;
		std::int32_t reticleCount;
		std::int32_t variantIndex;
		std::int32_t variantCount;
	};

	struct InterfaceV1
	{
		std::uint32_t version;  // 1, or 2 when the members marked v2 exist

		// --- retarget ---------------------------------------------------
		// Override which shape MagnaScope treats as the aperture. Pass nullptr
		// or an empty name to clear the override and return to the profile's
		// own pin. Returns false if there is no scope selected.
		bool (*SetApertureOverride)(void* bsTriShape);
		bool (*SetApertureOverrideByName)(const char* nodeName);
		bool (*SetReticleOverride)(void* bsTriShape);
		bool (*SetReticleOverrideByName)(const char* nodeName);
		bool (*ClearOverrides)();

		// --- sights -----------------------------------------------------
		// -1 selects the primary optic; 0 and above select a secondary sight
		// configured on the profile. The transition is the profile's own lerp.
		bool (*TriggerSightSwap)(std::int32_t sightIndex);

		// --- read -------------------------------------------------------
		// Fills out with the equipped scope's state. Returns false when no
		// MagnaScope-managed scope is equipped.
		bool (*GetEquippedScope)(ScopeInfoV1* out);
		// Copies up to maxNodes entries of the first-person weapon's node tree
		// and returns how many were written. Pass nullptr to query the count.
		// This is the discovery mechanism: walk it, find your second aperture,
		// pass its name back through SetApertureOverrideByName.
		std::uint32_t (*SnapshotNodeTree)(
			NodeInfoV1* out,
			std::uint32_t maxNodes);

		// --- v2: upscaler / frame-generation cooperation ------------------
		// Only present when version >= 2. Appended after the v1 members so a
		// v1 consumer sees an unchanged layout.
		//
		// True while MagnaScope will draw a lens this frame: aimed through a
		// MagnaScope-managed scope with the effect enabled. Any thread.
		//
		// Intended consumer: an upscaler that presents through D3D12 with the
		// D3D11 back buffer as a UI layer. While this is true it should take
		// the same path it takes for the vanilla ScopeMenu (copy the finished
		// output back into the D3D11 chain and skip its present override), so
		// the lens magnifies the reconstructed frame rather than the
		// render-resolution one.
		bool (*IsOpticalEffectActive)();
		// The upscaler reports whether it took that copy-back path. Cheap;
		// call every frame, or on each change. While active MagnaScope uses
		// the D3D11 back buffer as its scene source instead of its own
		// pre-upsample capture.
		void (*NotifyPresentCopyBack)(bool active);
	};
}

#ifdef MAGNASCOPE_INTERNAL
#	include <string>
namespace MagnaScopeAPI
{
	// Implementation side only; not part of the published contract.
	InterfaceV1* GetInterface();
	// Drained once per game-thread tick.
	void DrainCommands();
	// Rebuilds the snapshots the getters serve. Game thread only.
	void RefreshSnapshots(int reticleCount);
	// Currently active overrides, consulted by aperture and reticle selection.
	// Empty means the profile's own pin is in force.
	[[nodiscard]] std::string GetApertureOverride();
	[[nodiscard]] std::string GetReticleOverride();
	// v2 backends, implemented next to the render state they read/write.
	[[nodiscard]] bool QueryOpticalEffectActive();
	void SetPresentCopyBack(bool active);
	[[nodiscard]] bool PresentCopyBackActive();
}
#endif
