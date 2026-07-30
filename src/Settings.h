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
		std::uint32_t verificationStage = 0;
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
		bool autoSTS = true;
		float defaultMaskDiameter = 700.0F;
		// Starting overlay magnification for automatically detected scopes.
		// Stacks on top of the sighted zoom the weapon already has.
		float defaultMagnification = 2.0F;
		// Mouse wheel zoom headroom: maxZoom = magnification * zoomSpread.
		float zoomSpread = 1.5F;

		[[nodiscard]] bool AllowsProjection() const noexcept { return verificationStage >= 1; }
		[[nodiscard]] bool AllowsOverrides() const noexcept { return verificationStage >= 2; }
		[[nodiscard]] bool AllowsCameraOverrides() const noexcept
		{
			return verificationStage >= 4 ||
			       (verificationStage >= 2 && verificationCameraOverride);
		}
		[[nodiscard]] bool AllowsRenderer() const noexcept { return verificationStage >= 3; }
		[[nodiscard]] bool AllowsGeometryProbe() const noexcept
		{
			return verificationStage == 4 && verificationGeometryProbe;
		}
		[[nodiscard]] bool AllowsPrivateRenderHooks() const noexcept
		{
			return verificationStage >= 4;
		}
		[[nodiscard]] bool AllowsComposite() const noexcept
		{
			return verificationStage >= 4 && !verificationGeometryProbe;
		}
		[[nodiscard]] bool AllowsVisualProbe() const noexcept
		{
			return verificationStage == 3 && verificationVisualProbe;
		}
		[[nodiscard]] bool AllowsTAACapture() const noexcept
		{
			return verificationStage >= 4 ||
			       (verificationStage == 3 && verificationTaaCapture);
		}

		void Load();
		void Save() const;
	};

	Settings& GetSettings();
}
