#include "ScopeResolver.h"

#include <algorithm>
#include <cmath>

namespace MagnaScope
{
	namespace
	{
		[[nodiscard]] float Lerp(float from, float to, float t) noexcept
		{
			return from + (to - from) * t;
		}

		void LerpZoom(
			ScopeData::ZoomDataOverwrite& out,
			const ScopeData::ZoomDataOverwrite& from,
			const ScopeData::ZoomDataOverwrite& to,
			float t)
		{
			out.x = Lerp(from.x, to.x, t);
			out.y = Lerp(from.y, to.y, t);
			out.z = Lerp(from.z, to.z, t);
			out.fovMul = Lerp(from.fovMul, to.fovMul, t);
			// A blend is only meaningful if both ends want the override; if
			// either does, keeping it enabled is what makes the transition
			// continuous rather than snapping to authored zoom mid-way.
			out.enableZoomDateOverwrite =
				from.enableZoomDateOverwrite || to.enableZoomDateOverwrite;
		}

		// Eases a value toward a target with a fixed time constant. Framerate
		// independent: an exponential approach whose rate is expressed per
		// second, so a 30 fps and a 144 fps client take the same wall-clock time
		// to settle. HUDMenu already taught this project not to assume 60.
		void Approach(float& value, float target, float seconds, float deltaSeconds)
		{
			if (seconds <= 0.0001F || deltaSeconds <= 0.0F) {
				value = target;
				return;
			}
			const float rate = 1.0F - std::exp(-deltaSeconds / seconds);
			value += (target - value) * std::clamp(rate, 0.0F, 1.0F);
			if (std::abs(target - value) < 0.0005F) {
				value = target;
			}
		}

		// Smoothstep, so a sight swap has no velocity discontinuity at either
		// end. A linear blend reads as a mechanical slide.
		[[nodiscard]] float Smooth(float t) noexcept
		{
			const float clamped = std::clamp(t, 0.0F, 1.0F);
			return clamped * clamped * (3.0F - 2.0F * clamped);
		}

		// Interpolates every numeric optical field. The caller restores the
		// structural fields (surface names, bool flags) from the base profile
		// afterwards -- they are not lerpable, and changing apertureSurface
		// mid-blend would re-trigger aperture discovery every frame.
		//
		// Starting from a copy of the low end means a field this function
		// forgets is left at the low variant's value rather than at zero, which
		// is the failure worth having.
		[[nodiscard]] ScopeData::ShaderData LerpShaderData(
			const ScopeData::ShaderData& from,
			const ScopeData::ShaderData& to,
			float t)
		{
			ScopeData::ShaderData out = from;

			out.nvIntensity = Lerp(from.nvIntensity, to.nvIntensity, t);
			out.baseWeaponPos = Lerp(from.baseWeaponPos, to.baseWeaponPos, t);
			out.movePercentage = Lerp(from.movePercentage, to.movePercentage, t);
			out.camDepth = Lerp(from.camDepth, to.camDepth, t);
			out.minZoom = Lerp(from.minZoom, to.minZoom, t);
			out.maxZoom = Lerp(from.maxZoom, to.maxZoom, t);
			out.fovAdjust = Lerp(from.fovAdjust, to.fovAdjust, t);

			for (int index = 0; index < 2; ++index) {
				out.PositionOffset[index] = Lerp(
					from.PositionOffset[index], to.PositionOffset[index], t);
				out.OriPositionOffset[index] = Lerp(
					from.OriPositionOffset[index],
					to.OriPositionOffset[index],
					t);
				out.Size[index] = Lerp(from.Size[index], to.Size[index], t);
				out.OriSize[index] = Lerp(from.OriSize[index], to.OriSize[index], t);
				out.reticle_Offset[index] = Lerp(
					from.reticle_Offset[index], to.reticle_Offset[index], t);
				out.lensOffset[index] = Lerp(
					from.lensOffset[index], to.lensOffset[index], t);
			}
			for (int index = 0; index < 4; ++index) {
				out.rectSize[index] =
					Lerp(from.rectSize[index], to.rectSize[index], t);
			}

			out.ReticleSize = Lerp(from.ReticleSize, to.ReticleSize, t);
			out.fishEyeStrength = Lerp(from.fishEyeStrength, to.fishEyeStrength, t);
			out.fishEyePower = Lerp(from.fishEyePower, to.fishEyePower, t);
			out.edgeRefractionStrength = Lerp(
				from.edgeRefractionStrength, to.edgeRefractionStrength, t);
			out.edgeRefractionWidth = Lerp(
				from.edgeRefractionWidth, to.edgeRefractionWidth, t);
			out.edgeChromaticAberration = Lerp(
				from.edgeChromaticAberration, to.edgeChromaticAberration, t);
			out.sceneParallaxStrength = Lerp(
				from.sceneParallaxStrength, to.sceneParallaxStrength, t);
			out.opticalLagStrength = Lerp(
				from.opticalLagStrength, to.opticalLagStrength, t);
			out.imageDenoise = Lerp(from.imageDenoise, to.imageDenoise, t);
			out.imageSharpen = Lerp(from.imageSharpen, to.imageSharpen, t);
			out.reticleMagnification = Lerp(
				from.reticleMagnification, to.reticleMagnification, t);
			out.reticleShadowStrength = Lerp(
				from.reticleShadowStrength, to.reticleShadowStrength, t);
			out.reticleParallaxStrength = Lerp(
				from.reticleParallaxStrength, to.reticleParallaxStrength, t);
			out.lensScale = Lerp(from.lensScale, to.lensScale, t);

			out.breathing.rate = Lerp(from.breathing.rate, to.breathing.rate, t);
			out.breathing.sway = Lerp(from.breathing.sway, to.breathing.sway, t);
			out.breathing.drift = Lerp(from.breathing.drift, to.breathing.drift, t);
			out.breathing.figure =
				Lerp(from.breathing.figure, to.breathing.figure, t);
			out.breathing.hold = Lerp(from.breathing.hold, to.breathing.hold, t);
			out.breathing.pupilFollow =
				Lerp(from.breathing.pupilFollow, to.breathing.pupilFollow, t);

			out.parallax.radius = Lerp(from.parallax.radius, to.parallax.radius, t);
			out.parallax.relativeFogRadius = Lerp(
				from.parallax.relativeFogRadius, to.parallax.relativeFogRadius, t);
			out.parallax.scopeSwayAmount = Lerp(
				from.parallax.scopeSwayAmount, to.parallax.scopeSwayAmount, t);
			out.parallax.maxTravel =
				Lerp(from.parallax.maxTravel, to.parallax.maxTravel, t);
			out.parallax.sceneDepth =
				Lerp(from.parallax.sceneDepth, to.parallax.sceneDepth, t);
			out.parallax.shadowDepth =
				Lerp(from.parallax.shadowDepth, to.parallax.shadowDepth, t);
			out.parallax.imageStillness = Lerp(
				from.parallax.imageStillness, to.parallax.imageStillness, t);
			out.parallax.axialBreathing = Lerp(
				from.parallax.axialBreathing, to.parallax.axialBreathing, t);
			out.parallax.recenterSpeed = Lerp(
				from.parallax.recenterSpeed, to.parallax.recenterSpeed, t);
			out.parallax.strafeLag =
				Lerp(from.parallax.strafeLag, to.parallax.strafeLag, t);
			out.parallax.tubeDepth =
				Lerp(from.parallax.tubeDepth, to.parallax.tubeDepth, t);

			return out;
		}
	}

	SightSessionKey MakeSessionKey(const ScopeData::ScopeProfile& profile)
	{
		return SightSessionKey{
			profile.sourcePlugin,
			profile.sourceFormID,
			profile.omodKey.empty() ? std::string("Default") : profile.omodKey
		};
	}

	std::map<SightSessionKey, SightSessionState>& SessionStates()
	{
		static std::map<SightSessionKey, SightSessionState> states;
		return states;
	}

	int FindVariantIndexById(
		const ScopeData::VariantSet& variants,
		std::uint32_t id)
	{
		for (std::size_t index = 0; index < variants.variants.size(); ++index) {
			if (variants.variants[index].id == id) {
				return static_cast<int>(index);
			}
		}
		return -1;
	}

	SightSessionState& SessionStateFor(const ScopeData::ScopeProfile& profile)
	{
		auto& states = SessionStates();
		const auto key = MakeSessionKey(profile);
		const auto existing = states.find(key);
		if (existing != states.end()) {
			auto& state = existing->second;
			// A co-save restores an id, not a position; the profile needed to
			// map one onto the other is only available here. If the id no longer
			// exists -- the author deleted that variant since the save -- fall
			// back to the authored default rather than to whatever slid into
			// that index, which is the whole reason ids are never reused.
			if (state.variantNeedsResolve) {
				state.variantNeedsResolve = false;
				int index = FindVariantIndexById(profile.variants, state.variantId);
				if (index < 0) {
					index = FindVariantIndexById(
						profile.variants, profile.variants.defaultVariantId);
				}
				if (index < 0) {
					index = 0;
				}
				state.variantPosition = static_cast<float>(index);
				state.variantTarget = state.variantPosition;
				if (static_cast<std::size_t>(index) <
					profile.variants.variants.size()) {
					state.variantId = profile.variants.variants[
						static_cast<std::size_t>(index)]
						.id;
				}
			}
			return state;
		}

		// First sight of this scope on this save: start where the profile's
		// author said to. The co-save overrides this the moment the player
		// touches anything.
		SightSessionState state;
		if (profile.variants.enabled && !profile.variants.variants.empty()) {
			const int defaultIndex = FindVariantIndexById(
				profile.variants,
				profile.variants.defaultVariantId);
			const float position =
				static_cast<float>(defaultIndex >= 0 ? defaultIndex : 0);
			state.variantPosition = position;
			state.variantTarget = position;
			const auto index = static_cast<std::size_t>(
				defaultIndex >= 0 ? defaultIndex : 0);
			if (index < profile.variants.variants.size()) {
				state.variantId = profile.variants.variants[index].id;
			}
		}
		return states.emplace(key, state).first->second;
	}

	int ActiveVariantIndex(
		const ScopeData::ScopeProfile& profile,
		const SightSessionState& state)
	{
		const auto count = profile.variants.variants.size();
		if (!profile.variants.enabled || count == 0U) {
			return -1;
		}
		const int rounded = static_cast<int>(std::lround(state.variantPosition));
		return std::clamp(rounded, 0, static_cast<int>(count) - 1);
	}

	bool ResolveOverlay(
		const ScopeData::ScopeProfile& profile,
		SightSessionState& state,
		float deltaSeconds,
		std::uint64_t selectionRevision,
		ImGuiImpl::EditorPreviewSnapshot& outOverlay)
	{
		const auto& variants = profile.variants;
		const auto variantCount = variants.variants.size();

		// --- advance the eases ---------------------------------------------
		if (variants.enabled && variantCount > 0U) {
			const float maximum = static_cast<float>(variantCount) - 1.0F;
			state.variantTarget = std::clamp(state.variantTarget, 0.0F, maximum);
			if (variants.continuous) {
				// The scroll already moved the position directly.
				state.variantPosition = state.variantTarget;
			} else {
				Approach(
					state.variantPosition,
					state.variantTarget,
					std::max(0.0F, variants.stepSeconds),
					deltaSeconds);
			}
			state.variantPosition =
				std::clamp(state.variantPosition, 0.0F, maximum);
		} else {
			state.variantPosition = 0.0F;
			state.variantTarget = 0.0F;
		}

		const int sightCount = static_cast<int>(profile.secondarySights.size());
		if (state.secondaryIndex >= sightCount) {
			state.secondaryIndex = -1;
		}
		const float sightTarget = state.secondaryIndex >= 0 ? 1.0F : 0.0F;
		// The return glide reuses the transition time of the sight being left
		// (lastSightIndex), so a swap reads symmetric in both directions.
		const int easeSight = state.secondaryIndex >= 0 ?
		                          state.secondaryIndex :
		                          state.lastSightIndex;
		const float sightSeconds =
			easeSight >= 0 && easeSight < sightCount ?
				profile.secondarySights[static_cast<std::size_t>(easeSight)]
					.transitionSeconds :
				0.18F;
		Approach(state.sightBlend, sightTarget, sightSeconds, deltaSeconds);

		// --- build the overlay ---------------------------------------------
		// Start from the base profile so a disabled variant set publishes
		// exactly what the profile already says, byte for byte.
		auto shaderData = profile.shaderData;
		auto zoom = profile.zoomDataOverwrite;

		if (variants.enabled && variantCount > 0U) {
			const float position = state.variantPosition;
			const auto lower = static_cast<std::size_t>(std::floor(position));
			const auto upper = std::min(lower + 1U, variantCount - 1U);
			const float t = position - static_cast<float>(lower);

			const auto& from = variants.variants[lower];
			const auto& to = variants.variants[upper];

			// Interpolating the whole ShaderData and then restoring the
			// structural fields from the base is deliberate: a hand-picked
			// lerpable subset would need maintaining in lockstep with ShaderData
			// and would silently drop anything added later. Restoring afterwards
			// fails safe instead.
			shaderData = LerpShaderData(from.shaderData, to.shaderData, t);
			shaderData.apertureSurface = profile.shaderData.apertureSurface;
			shaderData.reticleSurface = profile.shaderData.reticleSurface;
			shaderData.IsCircle = profile.shaderData.IsCircle;
			shaderData.bEnableZMove = profile.shaderData.bEnableZMove;
			shaderData.bCanEnableNV = profile.shaderData.bCanEnableNV;
			shaderData.bBoltDisable = profile.shaderData.bBoltDisable;

			LerpZoom(zoom, from.zoomDataOverwrite, to.zoomDataOverwrite, t);

			// With variants on, the resolver owns magnification outright. The
			// per-frame clamp in the shader path is driven from minZoom/maxZoom,
			// and during a blend both those bounds and the magnification are
			// moving, with nothing keeping the latter inside the former. Pinning
			// the range to the resolved value removes the race entirely; free
			// scroll headroom is a non-variant feature.
			const float magnification =
				Lerp(from.magnification, to.magnification, t);
			shaderData.minZoom = magnification;
			shaderData.maxZoom = magnification;
		}

		outOverlay = ImGuiImpl::EditorPreviewSnapshot::FromProfile(
			shaderData, zoom, selectionRevision);

		// --- secondary sight -------------------------------------------------
		// The transition is a pure ZoomData lerp, applied every tick while the
		// player STAYS sighted. The engine samples ZoomData only when the
		// animation graph is poked, so the consumer writes these lerped values
		// into the installed runtime zoom form and re-fires the [Sights] graph
		// events every tick the blend is in flight (DriveSightZoomTransition
		// in main.cpp) -- the engine tracks the moving values and the eye
		// glides between sights, camera offset and FOV together. Never drop
		// and re-raise the sighted state to force a re-sample: doing that
		// opened a not-sighted window that other wheel mods acted on and could
		// wedge the engine's aim state machine.
		//
		// lastSightIndex keeps the transition symmetric: blending BACK to the
		// optic lerps from the sight just left rather than snapping.
		{
			const int blendSight = state.secondaryIndex >= 0 ?
			                           state.secondaryIndex :
			                           state.lastSightIndex;
			if (blendSight >= 0 && blendSight < sightCount &&
				state.sightBlend > 0.0001F) {
				const auto& sight = profile.secondarySights[
					static_cast<std::size_t>(blendSight)];
				const float blend = Smooth(state.sightBlend);
				// Smoothstepped so the glide has no velocity discontinuity at
				// either end; the graph re-sample makes these lerped values
				// live every tick, not just at the next aim-in.
				LerpZoom(outOverlay.zoomOverride, zoom, sight.zoomData, blend);
				// sightShiftX/Y/Z deliberately stay zero: shifting the
				// first-person weapon subtree mid-frame tore the scope render
				// apart in game (skinned arms, capture fingerprints, and the
				// optical projections all disagree about the pose). The graph
				// re-sample made that whole mechanism unnecessary anyway.
				// Deliberately no aperture suppression either: the swap does
				// exactly one thing -- set the other sight's zoom offsets.
			}
		}

		outOverlay.customReticleIndex = state.reticleIndex;
		outOverlay.customReticleScale = profile.customReticleScale;
		outOverlay.Clamp();
		return true;
	}

	bool AdjustVariantSelection(
		const ScopeData::ScopeProfile& profile,
		SightSessionState& state,
		int direction)
	{
		const auto& variants = profile.variants;
		const auto count = variants.variants.size();
		if (!variants.enabled || count < 2U || direction == 0) {
			return false;
		}

		const float maximum = static_cast<float>(count) - 1.0F;
		const float before = state.variantTarget;
		if (variants.continuous) {
			// A tenth of a stop per notch, so a full sweep of a two-stop optic
			// takes ten notches rather than snapping on the first.
			state.variantTarget = std::clamp(
				state.variantTarget + static_cast<float>(direction) * 0.1F,
				0.0F,
				maximum);
		} else {
			const int current = static_cast<int>(std::lround(state.variantTarget));
			state.variantTarget = static_cast<float>(
				std::clamp(current + direction, 0, static_cast<int>(count) - 1));
		}

		if (state.variantTarget == before) {
			return false;
		}

		// Keep the persisted id in step with where the wheel now points. In
		// continuous mode the target is fractional, so the nearest variant is
		// what a reload should land on.
		const auto nearest = static_cast<std::size_t>(
			std::clamp(
				static_cast<int>(std::lround(state.variantTarget)),
				0,
				static_cast<int>(count) - 1));
		state.variantId = variants.variants[nearest].id;
		state.userTouched = true;
		return true;
	}

	bool CycleSecondarySight(
		const ScopeData::ScopeProfile& profile,
		SightSessionState& state,
		int direction)
	{
		const int count = static_cast<int>(profile.secondarySights.size());
		if (count == 0 || direction == 0) {
			return false;
		}

		// CLAMPED, deliberately not a ring: scrolling several notches while
		// holding the key must park on the last sight (or back on the optic),
		// not oscillate through optic-sight-optic. With one sight configured a
		// wrapped ring made every notch a toggle, and a scroll burst thrashed
		// the aperture fade and the zoom writes -- exactly the reported
		// "glitches out" behaviour. -1 is the primary optic.
		const int previous = state.secondaryIndex;
		state.secondaryIndex =
			std::clamp(state.secondaryIndex + direction, -1, count - 1);
		if (state.secondaryIndex == previous) {
			return false;
		}
		if (state.secondaryIndex >= 0) {
			state.lastSightIndex = state.secondaryIndex;
		}
		state.userTouched = true;
		return true;
	}

	bool CycleReticle(
		const ScopeData::ScopeProfile& profile,
		SightSessionState& state,
		int reticleCount,
		int direction)
	{
		(void)profile;
		if (reticleCount <= 0 || direction == 0) {
			return false;
		}

		// -1 (the authored 3D reticle) is a position in the ring, so a scope
		// with one custom texture toggles between authored and custom.
		const int positions = reticleCount + 1;
		int position = state.reticleIndex + 1;
		position = (position + direction) % positions;
		if (position < 0) {
			position += positions;
		}
		state.reticleIndex = position - 1;
		state.userTouched = true;
		return true;
	}
}
