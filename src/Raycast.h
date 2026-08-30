#pragma once

namespace RE
{
	class Actor;
	class NiAVObject;
	class TESObjectCELL;
	enum class COL_LAYER : std::int32_t;
}

namespace MagnaScope::Raycast
{
	// Result of one ray query. Distances are in game units along the requested
	// segment; `hit == false` leaves `distance` at the full segment length so
	// callers can treat "no hit" as "clear for the whole distance."
	struct RayHit
	{
		bool            hit{ false };
		float           distance{ 0.0f };  // from start to the accepted hit
		RE::NiPoint3    point{};           // world-space hit point
		RE::NiPoint3    normal{};          // hit normal (zero when unavailable)
		RE::COL_LAYER   layer{};           // collision layer of the accepted hit
		RE::NiAVObject* obj{ nullptr };    // scene object (closest-hit only)
	};

	// Human-readable name for a collision layer (engine layer table).
	const char* LayerName(RE::COL_LAYER a_layer);

	// Solid, visible, world geometry -- the allow-list every occlusion ray
	// filters through. Unknown layers FAIL as solid: a source that flickers off
	// for a frame is benign, a source glowing through a wall is the bug.
	bool IsSolidLayer(RE::COL_LAYER a_layer);

	// Cast a ray from a_from to a_to against the player's parent cell. Skips the
	// player's own body / any actor skeleton and non-solid helper volumes by
	// re-casting past them (up to 3 attempts), so real geometry behind them
	// still measures at its true distance. Returns true when solid geometry was
	// hit. Game thread only (uses cell->Pick / Havok).
	bool Cast(const RE::NiPoint3& a_from, const RE::NiPoint3& a_to, RayHit& a_out);
}
