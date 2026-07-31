#pragma once

namespace MagnaScope
{
	// Plugin-wide options persisted in MagnaScope.ini next to the DLL.
	struct Settings
	{
		// Diagnostic rollout gate. Stages deliberately separate game-data,
		// projection, override, resource, and draw work so an in-game crash can
		// be attributed to one verified boundary instead of guessed from the
		// last log line.
		//
		// 0: profile detection only
		// 1: verified HUD world-to-screen projection
		// 2: projection plus zoom/camera override lifecycle
		// 3: framework or TAA source capture, without a draw
		// 4: private render hooks, with either the geometry probe or the
		//    complete scope composite selected by the sub-gate below
		// 5: isolated OG auxiliary-renderer address/page/prologue preflight;
		//    the AuxiliaryPassThroughHooks sub-gate may install the complete
		//    four-hook transaction in forwarding-only mode; the mutually
		//    exclusive AuxiliaryObservationHooks sub-gate may install the same
		//    forwarding hooks with observation telemetry enabled
		std::uint32_t verificationStage = 0;
		// Stage 5b sub-gate. All four required world-only renderer hooks are
		// created and enabled as one transaction, but every detour immediately
		// calls the original function. No auxiliary frame can be requested.
		bool verificationAuxiliaryPassThroughHooks = false;
		// Stage 5c-a sub-gate. The same four detours remain forwarding-only,
		// but record bounded invocation and worker-lifetime telemetry needed to
		// prove that a future private pass can be introduced safely. This gate
		// never authorizes an auxiliary render, private resource creation,
		// profile processing, overrides, user interface, or Papyrus work.
		bool verificationAuxiliaryObservationHooks = false;
		// Stage 5c functional gate. This installs the verified OG hook set,
		// produces a private world-only source, and feeds that source into the
		// exact ScopeFade draw. It is mutually exclusive with the preflight
		// and forwarding-only diagnostics above.
		bool verificationAuxiliaryWorldPass = false;
		// Stage 2 is split so a bad camera offset cannot be confused with the
		// FOV lifecycle. Stage 4 always permits camera overrides.
		bool verificationCameraOverride = false;
		// Stage 3 is split by render anchor. The default Stage 3 path uses
		// F4SE Menu Framework's verified before-render callback. This sub-gate
		// adds only the guarded OG TAA vtable callback.
		bool verificationTaaCapture = false;
		// Stage 4a is a framework-owned HUD probe. It draws a translucent disc
		// at the published STS aperture without initializing MagnaScope shaders
		// or changing any D3D render target.
		bool verificationVisualProbe = false;
		// Stage 4d identifies the authored ScopeFade draw through its live
		// vertex and index buffers. A geometry shader fills the standardized
		// annulus center and a pixel shader colors the complete aperture.
		// Fallout still owns the scene transform, depth state, rasterizer
		// state, render target, and draw order under ScopeAiming.
		bool verificationGeometryProbe = false;
		// Stage 4e.2 keeps the verified ScopeFade geometry/depth contract, but
		// replaces the cyan diagnostic with a profile-driven sample of a private
		// copy of the current scene-color target. GeometryProbe must also be
		// enabled so a production INI cannot accidentally activate this
		// isolated rollout path.
		bool verificationGeometryMagnification = false;
		bool autoSTS = true;
		float defaultMaskDiameter = 700.0F;
		// Starting overlay magnification for automatically detected scopes.
		// Stacks on top of the sighted zoom the weapon already has.
		float defaultMagnification = 2.0F;
		// Mouse wheel zoom headroom: maxZoom = magnification * zoomSpread.
		float zoomSpread = 1.5F;

		[[nodiscard]] bool IsLegacyRolloutStage() const noexcept
		{
			return verificationStage <= 4;
		}
		[[nodiscard]] bool AllowsAuxiliaryWorldPass() const noexcept
		{
			// Retired after the Stage 5c crash proved that detouring Fallout's
			// private world/culling path is not an acceptable production
			// boundary. Automatic STS magnification now follows FTS's stable
			// late coherent-color replay contract instead. Keep the parsed
			// field only so an old diagnostic INI fails closed below.
			return false;
		}
		[[nodiscard]] bool AllowsProjection() const noexcept
		{
			return (IsLegacyRolloutStage() && verificationStage >= 1) ||
			       AllowsAuxiliaryWorldPass();
		}
		[[nodiscard]] bool AllowsOverrides() const noexcept
		{
			return (IsLegacyRolloutStage() && verificationStage >= 2) ||
			       AllowsAuxiliaryWorldPass();
		}
		[[nodiscard]] bool AllowsCameraOverrides() const noexcept
		{
			return verificationStage == 4 ||
			       AllowsAuxiliaryWorldPass() ||
			       (verificationStage >= 2 &&
			        verificationStage <= 3 &&
			        verificationCameraOverride);
		}
		[[nodiscard]] bool AllowsRenderer() const noexcept
		{
			return (IsLegacyRolloutStage() && verificationStage >= 3) ||
			       AllowsAuxiliaryWorldPass();
		}
		[[nodiscard]] bool AllowsGeometryProbe() const noexcept
		{
			return verificationStage == 4 &&
			       verificationGeometryProbe &&
			       !verificationGeometryMagnification;
		}
		[[nodiscard]] bool AllowsGeometryMagnification() const noexcept
		{
			return (verificationStage == 4 &&
			        verificationGeometryProbe &&
			        verificationGeometryMagnification) ||
			       AllowsAuxiliaryWorldPass();
		}
		[[nodiscard]] bool AllowsScopeFadeGeometry() const noexcept
		{
			return AllowsGeometryProbe() || AllowsGeometryMagnification();
		}
		[[nodiscard]] bool AllowsPrivateRenderHooks() const noexcept
		{
			return verificationStage == 4 ||
			       AllowsAuxiliaryWorldPass();
		}
		[[nodiscard]] bool AllowsComposite() const noexcept
		{
			// Geometry magnification now follows the original FTS late-frame
			// contract. The authored ScopeFade draw is recorded during the
			// weapon pass, but its magnified replay must run after TAA or at
			// Present against a fully rendered color source. The cyan geometry
			// probe remains draw-time-only and therefore does not composite.
			return verificationStage == 4 &&
			       (!AllowsScopeFadeGeometry() ||
			        AllowsGeometryMagnification());
		}
		[[nodiscard]] bool AllowsVisualProbe() const noexcept
		{
			return verificationStage == 3 && verificationVisualProbe;
		}
		[[nodiscard]] bool AllowsTAACapture() const noexcept
		{
			return verificationStage == 4 ||
			       (verificationStage == 3 && verificationTaaCapture);
		}
		[[nodiscard]] bool AllowsAuxiliaryRuntimePreflight() const noexcept
		{
			return verificationStage == 5 &&
			       !verificationAuxiliaryPassThroughHooks &&
			       !verificationAuxiliaryObservationHooks &&
			       !verificationAuxiliaryWorldPass;
		}
		[[nodiscard]] bool AllowsAuxiliaryPassThroughHooks() const noexcept
		{
			return verificationStage == 5 &&
			       verificationAuxiliaryPassThroughHooks &&
			       !verificationAuxiliaryObservationHooks &&
			       !verificationAuxiliaryWorldPass;
		}
		[[nodiscard]] bool AllowsAuxiliaryObservationHooks() const noexcept
		{
			return verificationStage == 5 &&
			       !verificationAuxiliaryPassThroughHooks &&
			       verificationAuxiliaryObservationHooks &&
			       !verificationAuxiliaryWorldPass;
		}

		void Load();
		void Save() const;
	};

	Settings& GetSettings();
}
