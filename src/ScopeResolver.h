#pragma once

#include "ImGuiImpl.h"
#include "ScopeProfile.h"

#include <cstdint>
#include <map>
#include <string>
#include <tuple>

namespace MagnaScope
{
	// Which optic, magnification and reticle a given scope is currently on.
	//
	// This is session state, not authored data. It lives in the co-save rather
	// than beside the profile because it has to fork when a save forks: loading
	// a save from before an optic swap must restore the sight that save was
	// taken with, and two characters carrying the same weapon must not share one
	// selection.
	struct SightSessionState
	{
		// Continuous position within the sorted variant list. Stepped mode
		// eases toward an integer target; continuous mode moves it directly.
		float variantPosition = 0.0F;
		float variantTarget = 0.0F;
		// The selected variant's stable id, and the only variant field the
		// co-save stores. Position is a list index, and an index silently means
		// a different optic the moment the author inserts a variant; the id
		// does not. Kept in step with the target as the wheel moves.
		std::uint32_t variantId = 0;
		// Set when a co-save restores an id that has not yet been mapped onto a
		// list position. Resolution needs the profile, which is not available
		// during deserialisation.
		bool variantNeedsResolve = false;
		// -1 selects the primary optic.
		int secondaryIndex = -1;
		// 0 = primary, 1 = fully on the secondary sight.
		float sightBlend = 0.0F;
		// The sight most recently selected, kept while blending BACK to the
		// optic so the return glide has something to lerp from. Session-only.
		int lastSightIndex = -1;
		// -1 selects the authored 3D reticle mesh.
		int reticleIndex = -1;
		// Set once the player has touched anything, so the profile's authored
		// defaults stop being reapplied on the next selection.
		bool userTouched = false;
	};

	// (plugin filename, local FormID, omod key). Deliberately not a raw FormID:
	// this identity is already load-order independent, so a co-save written
	// under one load order still resolves under another with no FormID fixups.
	using SightSessionKey = std::tuple<std::string, std::uint32_t, std::string>;

	[[nodiscard]] SightSessionKey MakeSessionKey(
		const ScopeData::ScopeProfile& profile);

	// The whole session map, for the co-save to walk.
	[[nodiscard]] std::map<SightSessionKey, SightSessionState>& SessionStates();

	// State for one profile, created from the profile's authored defaults on
	// first sight.
	[[nodiscard]] SightSessionState& SessionStateFor(
		const ScopeData::ScopeProfile& profile);

	// Index of the variant whose id matches, or -1.
	[[nodiscard]] int FindVariantIndexById(
		const ScopeData::VariantSet& variants,
		std::uint32_t id);

	// The variant the resolver is currently sitting on, rounded to nearest.
	// -1 when variants are disabled or empty.
	[[nodiscard]] int ActiveVariantIndex(
		const ScopeData::ScopeProfile& profile,
		const SightSessionState& state);

	// Advance eases and produce the live overlay for this tick.
	//
	// Runs on the game thread only, and must run AFTER profile selection within
	// the tick: it stamps the caller's selection revision onto the snapshot, and
	// a snapshot carrying a stale revision is silently dropped by the consumer,
	// which for the resolver would mean one frame of base-profile values at the
	// wrong magnification.
	//
	// Returns false when nothing should be published this tick (no profile, or
	// the selection changed and the previous frame's values should be held).
	[[nodiscard]] bool ResolveOverlay(
		const ScopeData::ScopeProfile& profile,
		SightSessionState& state,
		float deltaSeconds,
		std::uint64_t selectionRevision,
		ImGuiImpl::EditorPreviewSnapshot& outOverlay);

	// Scroll input: step or blend between variants. Returns true if it changed
	// anything, so the caller can suppress the free-zoom path.
	bool AdjustVariantSelection(
		const ScopeData::ScopeProfile& profile,
		SightSessionState& state,
		int direction);

	// Cycle the secondary sight: -1 (primary), then each configured sight.
	bool CycleSecondarySight(
		const ScopeData::ScopeProfile& profile,
		SightSessionState& state,
		int direction);

	// Cycle the reticle: -1 (authored mesh), then each discovered texture.
	bool CycleReticle(
		const ScopeData::ScopeProfile& profile,
		SightSessionState& state,
		int reticleCount,
		int direction);
}
