#include "ImGuiImpl.h"
#include "ScopeProfile.h"
#include "Settings.h"
#include "hooking.h"
#include <DirectXMath.h>
#include <cmath>
#include <d3d11.h>
#include <mutex>

char* _MESSAGE(const char* fmt, ...);

namespace ImGuiImpl
{
	std::vector<std::string> additionalKeywords;
	int additionalKeywords_count = 0;
	bool legacyFlag = true;

#define LF(f) (legacyFlag ? (f) : (f) / 1000.0f)
#define FL(f) (legacyFlag ? (f) : (f) * 1000.0f)

	std::unique_ptr<float[]> LFA(float arr[], size_t size)
	{
		std::unique_ptr<float[]> arrNew(new float[size]);

		for (size_t i = 0; i < size; i++) {
			arrNew[i] = LF(arr[i]);
		}

		return arrNew;
	}

	std::unique_ptr<float[]> FLA(float arr[], size_t size)
	{
		std::unique_ptr<float[]> arrNew(new float[size]);

		for (size_t i = 0; i < size; i++) {
			arrNew[i] = FL(arr[i]);
		}

		return arrNew;
	}

	ImGuiImplClass* ImGuiImplClass::GetSington()
	{
		static ImGuiImplClass instance;
		return &instance;
	}

	using namespace ScopeData;
	using namespace DirectX;
	Hook::D3D::ScopeEffectShaderData scopeData;
	Hook::D3D* d3d;

	ScopeData::ScopeProfile* currData;
	ScopeData::ScopeDataHandler* sdh;

	bool bInitZoomData = false;
	std::atomic_bool equippedZoomDataAvailable{ false };

	// Attaches a hover tooltip to the widget submitted immediately before.
	void Tip(const char* text)
	{
		ImGui::SetItemTooltip("%s", text);
	}

	void ImGuiImplClass::UpdateWeaponInstance(RE::TESObjectWEAP::InstanceData* instanceData)
	{
		// Publish availability only. The renderer thread must not retain or
		// dereference the live instance that the game thread owns.
		equippedZoomDataAvailable.store(
			instanceData && instanceData->zoomData,
			std::memory_order_release);
	}

	void ImGuiImplClass::UpdateImGuiData()
	{
		currData = sdh->GetCurrentScopeProfile();
	}

	// Persistent request consumed by the game-thread update hook. A one-shot
	// transition is not sufficient because Fallout can lower the weapon again
	// while a blocking menu is open.
	std::atomic<int> pendingForcedAim{ 0 };

	namespace
	{
		std::mutex editorPreviewMutex;
		EditorPreviewSnapshot editorPreview{};
		std::mutex authoredZoomMutex;
		AuthoredZoomSnapshot authoredZoomSnapshot{};
		std::atomic<ProfileRequest> pendingProfileAction{
			ProfileRequest::kNone
		};
		std::mutex profileSaveMutex;
		std::unique_ptr<ScopeData::ScopeProfile> pendingProfileSave;
		MENU_WINDOW scopeEditorWindow = nullptr;
		F4SEMenuFramework::Model::HudElement* scopeVisualProbe = nullptr;
		// The popout is intended to be an interactive editor by default.
		// Actual framework input blocking is armed only after the main panel's
		// close sweep, so this preference cannot cause the popout to be closed.
		std::atomic_bool captureMouseOutsidePanel{ true };
		std::atomic_bool frameworkMenuOpen{ false };
	}

	void PublishAuthoredZoomSnapshot(
		const ScopeData::ZoomDataOverwrite* authoredValues,
		std::uint64_t selectionRevision)
	{
		std::scoped_lock lock(authoredZoomMutex);
		authoredZoomSnapshot = {};
		authoredZoomSnapshot.selectionRevision = selectionRevision;
		if (authoredValues) {
			authoredZoomSnapshot.values = *authoredValues;
			authoredZoomSnapshot.available = true;
		}
	}

	AuthoredZoomSnapshot GetAuthoredZoomSnapshot()
	{
		std::scoped_lock lock(authoredZoomMutex);
		return authoredZoomSnapshot;
	}

	void PublishEditorPreview(
		const ScopeData::ZoomDataOverwrite& zoomOverride,
		std::uint64_t selectionRevision,
		float magnification,
		float imageDenoise,
		float imageSharpen,
		float fishEyeStrength,
		float fishEyePower,
		float edgeRefractionStrength,
		float edgeRefractionWidth,
		float edgeChromaticAberration,
		float reticleMagnification,
		float reticleSize,
		float reticleOffsetX,
		float reticleOffsetY,
		float eyeBoxRadius,
		float vignetteReach,
		float vignetteSharpness,
		float eyeBoxMaxTravel,
		float sceneParallaxStrength,
		float opticalLagStrength,
		float reticleShadowStrength,
		float reticleParallaxStrength,
		float sceneDepth,
		float shadowDepth,
		float imageStillness,
		float axialBreathing,
		float recenterSpeed,
		float tubeDepth,
		float lensOffsetX,
		float lensOffsetY,
		float lensScale,
		const ScopeData::Breathing& breathing)
	{
		std::scoped_lock lock(editorPreviewMutex);
		editorPreview.zoomOverride = zoomOverride;
		editorPreview.selectionRevision = selectionRevision;
		editorPreview.magnification =
			std::clamp(magnification, 1.0F, 15.0F);
		editorPreview.imageDenoise =
			std::clamp(imageDenoise, 0.0F, 1.0F);
		editorPreview.imageSharpen =
			std::clamp(imageSharpen, 0.0F, 1.0F);
		editorPreview.fishEyeStrength =
			std::clamp(fishEyeStrength, 0.0F, 2.0F);
		editorPreview.fishEyePower =
			std::clamp(fishEyePower, 0.5F, 6.0F);
		editorPreview.edgeRefractionStrength =
			std::clamp(edgeRefractionStrength, 0.0F, 0.25F);
		editorPreview.edgeRefractionWidth =
			std::clamp(edgeRefractionWidth, 0.02F, 0.5F);
		editorPreview.edgeChromaticAberration =
			std::clamp(edgeChromaticAberration, 0.0F, 2.0F);
		editorPreview.reticleMagnification =
			std::clamp(reticleMagnification, 0.25F, 8.0F);
		editorPreview.reticleSize =
			std::clamp(reticleSize, 0.01F, 128.0F);
		editorPreview.reticleOffsetX =
			std::clamp(reticleOffsetX, -1000.0F, 1000.0F);
		editorPreview.reticleOffsetY =
			std::clamp(reticleOffsetY, -1000.0F, 1000.0F);
		editorPreview.reticleShadowStrength =
			std::clamp(reticleShadowStrength, 0.0F, 1.0F);
		editorPreview.reticleParallaxStrength =
			std::clamp(reticleParallaxStrength, 0.0F, 4.0F);
		// These profile values also drive the physical
		// ScopeFade pupil. Publishing copies here preserves live editing
		// without sharing the menu-owned profile with the game/render threads.
		editorPreview.eyeBoxRadius =
			std::clamp(eyeBoxRadius, 0.01F, 20.0F);
		editorPreview.vignetteReach =
			std::clamp(vignetteReach, 1.01F, 20.0F);
		editorPreview.vignetteSharpness =
			std::clamp(vignetteSharpness, 0.1F, 20.0F);
		editorPreview.eyeBoxMaxTravel =
			std::clamp(eyeBoxMaxTravel, 0.0F, 4.0F);
		editorPreview.sceneParallaxStrength =
			std::clamp(sceneParallaxStrength, 0.0F, 2.0F);
		editorPreview.opticalLagStrength =
			std::clamp(opticalLagStrength, 0.0F, 4.0F);
		editorPreview.sceneDepth =
			std::clamp(sceneDepth, 0.0F, 4.0F);
		editorPreview.shadowDepth =
			std::clamp(shadowDepth, 0.0F, 4.0F);
		editorPreview.imageStillness =
			std::clamp(imageStillness, 0.0F, 1.0F);
		editorPreview.axialBreathing =
			std::clamp(axialBreathing, 0.0F, 4.0F);
		editorPreview.recenterSpeed =
			std::clamp(recenterSpeed, 0.1F, 10.0F);
		editorPreview.tubeDepth =
			std::clamp(tubeDepth, 0.0F, 1.0F);
		editorPreview.lensOffsetX =
			std::clamp(lensOffsetX, -1.0F, 1.0F);
		editorPreview.lensOffsetY =
			std::clamp(lensOffsetY, -1.0F, 1.0F);
		editorPreview.lensScale =
			std::clamp(lensScale, 0.25F, 2.0F);
		editorPreview.breathing.rate =
			std::clamp(breathing.rate, 0.0F, 4.0F);
		editorPreview.breathing.sway =
			std::clamp(breathing.sway, 0.0F, 1.0F);
		editorPreview.breathing.drift =
			std::clamp(breathing.drift, 0.0F, 1.0F);
		editorPreview.breathing.figure =
			std::clamp(breathing.figure, 0.0F, 1.0F);
		editorPreview.breathing.hold =
			std::clamp(breathing.hold, 0.0F, 1.0F);
		editorPreview.breathing.pupilFollow =
			std::clamp(breathing.pupilFollow, 0.0F, 2.0F);
		editorPreview.active = true;
	}

	EditorPreviewSnapshot GetEditorPreviewSnapshot()
	{
		std::scoped_lock lock(editorPreviewMutex);
		return editorPreview;
	}

	void ClearEditorPreview()
	{
		std::scoped_lock lock(editorPreviewMutex);
		editorPreview.active = false;
	}

	void RequestProfileAction(ProfileRequest request)
	{
		pendingProfileAction.store(request, std::memory_order_release);
	}

	ProfileRequest ConsumeProfileAction()
	{
		return pendingProfileAction.exchange(
			ProfileRequest::kNone,
			std::memory_order_acq_rel);
	}

	void RequestProfileSave(const ScopeData::ScopeProfile& profile)
	{
		std::scoped_lock lock(profileSaveMutex);
		pendingProfileSave =
			std::make_unique<ScopeData::ScopeProfile>(profile);
	}

	std::unique_ptr<ScopeData::ScopeProfile> ConsumeProfileSave()
	{
		std::scoped_lock lock(profileSaveMutex);
		return std::move(pendingProfileSave);
	}

	namespace
	{
		// Whether the menu is currently forcing the player to hold aim.
		bool bHoldAim = false;

		void ApplyPopoutInputCapture()
		{
			if (!scopeEditorWindow) {
				return;
			}
			const bool shouldCapture =
				scopeEditorWindow->IsOpen.load(std::memory_order_acquire) &&
				captureMouseOutsidePanel.load(std::memory_order_acquire) &&
				!frameworkMenuOpen.load(std::memory_order_acquire);
			scopeEditorWindow->BlockUserInput.store(
				shouldCapture,
				std::memory_order_release);
		}

		void SetHoldAim(bool enabled)
		{
			bHoldAim = enabled;
			// F4SE Menu Framework callbacks run on the renderer thread. Only
			// publish the request here; HookedUpdate performs all player and
			// animation work on Fallout's game thread.
			pendingForcedAim.store(
				enabled ? 1 : 0,
				std::memory_order_release);
		}
	}

	void AbandonZoomPreview()
	{
		// The game-thread selection transaction restores its own authored
		// baseline. Clearing this copied request is sufficient and cannot
		// dereference an instance that was replaced during a weapon swap.
		ClearEditorPreview();
		// This path ends the edit session without EndEditSession, so release
		// the forced aim here too; otherwise game input stays ignored with no
		// menu left to turn it off.
		if (bHoldAim) {
			SetHoldAim(false);
		}
		if (scopeEditorWindow) {
			scopeEditorWindow->BlockUserInput.store(
				false,
				std::memory_order_release);
		}
	}

	bool ImGuiImplClass::CheckAndInit()
	{
		if (!sdh)
			sdh = ScopeData::ScopeDataHandler::GetSingleton();

		if (!sdh)
			return false;
		return true;
	}

	void GeneralSettingsSection()
	{
		auto& settings = MagnaScope::GetSettings();

		ImGui::SeparatorText("General");

		bool changed = false;
		changed |= ImGui::Checkbox("Automatic STS Scopes", &settings.autoSTS);
		Tip("Applies the scope effect to weapons set up for See Through Scopes\n"
			"without needing a MagnaScope profile or patch.\n"
			"Takes effect the next time you swap weapons or reload a save.");

		changed |= ImGui::DragFloat(
			"Default Circle Size", &settings.defaultMaskDiameter, 1.0F, 64.0F, 2160.0F, "%.0f px");
		Tip("Diameter of the magnified circle for automatically detected scopes,\n"
			"in 1080p reference pixels (scales with your resolution).\n"
			"Only affects scopes detected after the change.");

		changed |= ImGui::DragFloat(
			"Default Magnification", &settings.defaultMagnification, 0.05F, 1.0F, 10.0F, "%.2fx");
		Tip("Starting magnification inside the circle for automatically detected\n"
			"scopes, on top of the sighted zoom the weapon already has.\n"
			"Only affects scopes detected after the change.");

		changed |= ImGui::DragFloat(
			"Scroll Zoom Range", &settings.zoomSpread, 0.05F, 1.0F, 10.0F, "%.2fx");
		Tip("Mouse wheel zoom headroom while aiming: the wheel can raise the\n"
			"magnification up to Default Magnification times this value.\n"
			"1.00x disables the extra zoom. Only affects newly detected scopes.");

		if (changed) {
			settings.Save();
		}
	}

	void KeyBindingSection(int& nvgComboKeyIndex, int& nvgMainKeyIndex)
	{
		if (nvgComboKeyIndex == -1 || nvgMainKeyIndex == -1) {
			return;
		}

		ImGui::SeparatorText("Hotkeys");

		nvgComboKeyIndex = sdh->comboNVKey + 1;
		nvgMainKeyIndex = sdh->nvKey + 1;

		if (ImGui::Combo("Night Vision Modifier Key", &nvgComboKeyIndex, mainKey, static_cast<int>(std::size(mainKey)))) {
			sdh->SetNVGHotKeyCombo(nvgComboKeyIndex - 1);
		}
		Tip("Optional key held together with the toggle key to switch night vision.\n"
			"Set to NONE to use the toggle key alone.");

		if (ImGui::Combo("Night Vision Toggle Key", &nvgMainKeyIndex, mainKey, static_cast<int>(std::size(mainKey)))) {
			sdh->SetNVGHotKeyMain(nvgMainKeyIndex - 1);
		}
		Tip("Key that switches the scope's night vision effect on and off while aiming.\n"
			"Night vision must be enabled for the scope under Effects.");
	}

	void ResetUIData(ImGuiImplClass* ins)
	{
		if (d3d && d3d->bRefreshChar.exchange(
					   false,
					   std::memory_order_acq_rel)) {
			// Profile selection and live game-object access happen in
			// HookedUpdate. The renderer thread only copies the profile that
			// the game thread has already selected.
			currData = sdh->GetCurrentScopeProfile();
			if (!currData)
				return;

			auto data = currData;
			legacyFlag = data->legacyMode;

			ins->bLegacyMode = data->legacyMode;
			ins->UsingSTS_UI = ScopeDataHandler::GetSingleton()->GetCurrentScopeProfile()->UsingSTS;
			ins->scopeFrame_UI = data->scopeFrame;
			ins->IsCircle_UI = data->shaderData.IsCircle;
			ins->camDepth_UI = data->shaderData.camDepth;
			ins->ReticleSize_UI = data->shaderData.ReticleSize;
			memcpy(ins->reticle_Offset, data->shaderData.reticle_Offset, 2 * sizeof(float));
			ins->minZoom_UI = data->shaderData.minZoom;
			ins->maxZoom_UI = data->shaderData.maxZoom;
			ins->fovBase_UI = data->shaderData.fovAdjust;

			memcpy(ins->PositionOffset_UI, data->shaderData.PositionOffset, 2 * sizeof(float));
			memcpy(ins->OriPositionOffset_UI, data->shaderData.OriPositionOffset, 2 * sizeof(float));
			memcpy(ins->Size_UI, data->shaderData.Size, 2 * sizeof(float));
			memcpy(ins->OriSize_UI, data->shaderData.OriSize, 2 * sizeof(float));
			memcpy(ins->Size_rect_UI, data->shaderData.rectSize, 4 * sizeof(float));

			ins->fishEyeStrength_UI = data->shaderData.fishEyeStrength;
			ins->fishEyePower_UI = data->shaderData.fishEyePower;
			ins->edgeRefractionStrength_UI =
				data->shaderData.edgeRefractionStrength;
			ins->edgeRefractionWidth_UI =
				data->shaderData.edgeRefractionWidth;
			ins->edgeChromaticAberration_UI =
				data->shaderData.edgeChromaticAberration;
			ins->imageDenoise_UI = data->shaderData.imageDenoise;
			ins->imageSharpen_UI = data->shaderData.imageSharpen;
			ins->reticleMagnification_UI =
				data->shaderData.reticleMagnification;
			ins->reticleShadowStrength_UI =
				std::isfinite(data->shaderData.reticleShadowStrength) ?
					std::clamp(data->shaderData.reticleShadowStrength, 0.0F, 1.0F) :
					0.0F;
			ins->reticleParallaxStrength_UI =
				std::isfinite(data->shaderData.reticleParallaxStrength) ?
					std::clamp(data->shaderData.reticleParallaxStrength, 0.0F, 4.0F) :
					1.0F;
			ins->radius_UI = data->shaderData.parallax.radius;
			ins->relativeFogRadius_UI = data->shaderData.parallax.relativeFogRadius;
			ins->scopeSwayAmount_UI = data->shaderData.parallax.scopeSwayAmount;
			ins->maxTravel_UI = data->shaderData.parallax.maxTravel;
			ins->sceneParallaxStrength_UI =
				data->shaderData.sceneParallaxStrength;
			ins->opticalLagStrength_UI =
				std::isfinite(data->shaderData.opticalLagStrength) ?
					std::clamp(
						data->shaderData.opticalLagStrength,
						0.0F,
						4.0F) :
					1.0F;
			ins->sceneDepth_UI =
				std::isfinite(data->shaderData.parallax.sceneDepth) ?
					std::clamp(data->shaderData.parallax.sceneDepth, 0.0F, 4.0F) :
					1.0F;
			ins->imageStillness_UI =
				std::isfinite(data->shaderData.parallax.imageStillness) ?
					std::clamp(data->shaderData.parallax.imageStillness, 0.0F, 1.0F) :
					0.0F;
			ins->axialBreathing_UI =
				std::isfinite(data->shaderData.parallax.axialBreathing) ?
					std::clamp(data->shaderData.parallax.axialBreathing, 0.0F, 4.0F) :
					0.0F;
			ins->tubeDepth_UI =
				std::isfinite(data->shaderData.parallax.tubeDepth) ?
					std::clamp(data->shaderData.parallax.tubeDepth, 0.0F, 1.0F) :
					0.0F;
			ins->recenterSpeed_UI =
				std::isfinite(data->shaderData.parallax.recenterSpeed) ?
					std::clamp(data->shaderData.parallax.recenterSpeed, 0.1F, 10.0F) :
					1.0F;
			ins->lensOffset_UI[0] =
				std::isfinite(data->shaderData.lensOffset[0]) ?
					std::clamp(data->shaderData.lensOffset[0], -1.0F, 1.0F) :
					0.0F;
			ins->lensOffset_UI[1] =
				std::isfinite(data->shaderData.lensOffset[1]) ?
					std::clamp(data->shaderData.lensOffset[1], -1.0F, 1.0F) :
					0.0F;
			ins->lensScale_UI =
				std::isfinite(data->shaderData.lensScale) ?
					std::clamp(data->shaderData.lensScale, 0.25F, 2.0F) :
					1.0F;
			const auto& profileBreathing = data->shaderData.breathing;
			ins->breathRate_UI =
				std::isfinite(profileBreathing.rate) ?
					std::clamp(profileBreathing.rate, 0.0F, 4.0F) :
					0.25F;
			ins->breathSway_UI =
				std::isfinite(profileBreathing.sway) ?
					std::clamp(profileBreathing.sway, 0.0F, 1.0F) :
					0.0F;
			ins->breathDrift_UI =
				std::isfinite(profileBreathing.drift) ?
					std::clamp(profileBreathing.drift, 0.0F, 1.0F) :
					0.0F;
			ins->breathFigure_UI =
				std::isfinite(profileBreathing.figure) ?
					std::clamp(profileBreathing.figure, 0.0F, 1.0F) :
					0.25F;
			ins->breathHold_UI =
				std::isfinite(profileBreathing.hold) ?
					std::clamp(profileBreathing.hold, 0.0F, 1.0F) :
					0.0F;
			ins->breathPupilFollow_UI =
				std::isfinite(profileBreathing.pupilFollow) ?
					std::clamp(profileBreathing.pupilFollow, 0.0F, 2.0F) :
					1.0F;
			ins->shadowDepth_UI =
				std::isfinite(data->shaderData.parallax.shadowDepth) ?
					std::clamp(data->shaderData.parallax.shadowDepth, 0.0F, 4.0F) :
					1.0F;
			ins->selectionRevision_UI =
				GetAuthoredZoomSnapshot().selectionRevision;

			ins->bEnableZMove = data->shaderData.bEnableZMove;
			ins->bEnableNVGEffect = data->shaderData.bCanEnableNV;
			ins->nvIntensity_UI = data->shaderData.nvIntensity;
			ins->baseWeaponPos_UI = data->shaderData.baseWeaponPos;
			ins->MovePercentage_UI = data->shaderData.movePercentage;

			ins->bDisableWhileBolt = data->shaderData.bBoltDisable;

			ins->Imgui_ZDO = data->zoomDataOverwrite;

			additionalKeywords_count = data->additionalKeywords.size();
			additionalKeywords = data->additionalKeywords;
		}
	}

	void ImGuiImplClass::ReloadData()
	{
		const bool pressed = ImGui::Button("Reload Profile", { 150, 0 });
		Tip("Discards unsaved changes and restores the values from the profile on disk.");
		if (pressed) {
			if (sdh->GetCurrentScopeProfile()) {
				// Disk reload and selection restoration are game-thread work.
				// Clear the copied preview now, then let HookedUpdate reload,
				// restore the authored baseline, and republish UI values.
				ClearEditorPreview();
				RequestProfileAction(ProfileRequest::kReload);
			}
		}
	}

	void ImGuiImplClass::SaveData()
	{
		const bool pressed = ImGui::Button("Save Profile", { 150, 0 });
		Tip("Writes the current values to the profile file so they persist.\n"
			"Automatic scopes save a new profile under Data/F4SE/Plugins/MagnaScope/Auto.");
		if (pressed) {
			bIsSaving.store(true, std::memory_order_release);
			currData = sdh->GetCurrentScopeProfile();
			if (!currData) {
				bIsSaving.store(false, std::memory_order_release);
				logger::error(
					"Scope settings were not saved because no profile is selected");
				return;
			}
			// Build a detached value snapshot. HookedUpdate validates its
			// profile identity, applies it to the selected profile, writes the
			// JSON, and reselects the weapon entirely on the game thread.
			auto editedProfile = *currData;
			editedProfile.legacyMode = bLegacyMode;
			Hook::D3D::bLegacyMode = bLegacyMode;

			editedProfile.UsingSTS = UsingSTS_UI;
			editedProfile.scopeFrame = scopeFrame_UI;
			editedProfile.shaderData.IsCircle = IsCircle_UI;
			editedProfile.shaderData.bCanEnableNV = bEnableNVGEffect;
			editedProfile.shaderData.baseWeaponPos = baseWeaponPos_UI;
			editedProfile.shaderData.bEnableZMove = bEnableZMove;
			editedProfile.shaderData.movePercentage = MovePercentage_UI;
			editedProfile.shaderData.camDepth = camDepth_UI;
			editedProfile.shaderData.ReticleSize = ReticleSize_UI;
			editedProfile.shaderData.minZoom = minZoom_UI;
			editedProfile.shaderData.maxZoom = maxZoom_UI;
			editedProfile.shaderData.reticle_Offset[0] = reticle_Offset[0];
			editedProfile.shaderData.reticle_Offset[1] = reticle_Offset[1];
			editedProfile.shaderData.PositionOffset[0] = PositionOffset_UI[0];
			editedProfile.shaderData.PositionOffset[1] = PositionOffset_UI[1];
			editedProfile.shaderData.OriPositionOffset[0] = OriPositionOffset_UI[0];
			editedProfile.shaderData.OriPositionOffset[1] = OriPositionOffset_UI[1];
			editedProfile.shaderData.Size[0] = Size_UI[0];
			editedProfile.shaderData.Size[1] = Size_UI[1];
			editedProfile.shaderData.OriSize[0] = OriSize_UI[0];
			editedProfile.shaderData.OriSize[1] = OriSize_UI[1];
			editedProfile.shaderData.fishEyeStrength = fishEyeStrength_UI;
			editedProfile.shaderData.fishEyePower = fishEyePower_UI;
			editedProfile.shaderData.edgeRefractionStrength =
				edgeRefractionStrength_UI;
			editedProfile.shaderData.edgeRefractionWidth =
				edgeRefractionWidth_UI;
			editedProfile.shaderData.edgeChromaticAberration =
				edgeChromaticAberration_UI;
			editedProfile.shaderData.imageDenoise = imageDenoise_UI;
			editedProfile.shaderData.imageSharpen = imageSharpen_UI;
			editedProfile.shaderData.reticleMagnification =
				reticleMagnification_UI;
			editedProfile.shaderData.reticleShadowStrength =
				std::clamp(reticleShadowStrength_UI, 0.0F, 1.0F);
			editedProfile.shaderData.reticleParallaxStrength =
				std::clamp(reticleParallaxStrength_UI, 0.0F, 4.0F);
			editedProfile.shaderData.parallax.radius = radius_UI;
			editedProfile.shaderData.parallax.relativeFogRadius = relativeFogRadius_UI;
			editedProfile.shaderData.parallax.scopeSwayAmount = scopeSwayAmount_UI;
			editedProfile.shaderData.parallax.maxTravel = maxTravel_UI;
			editedProfile.shaderData.sceneParallaxStrength =
				sceneParallaxStrength_UI;
			editedProfile.shaderData.opticalLagStrength =
				std::isfinite(opticalLagStrength_UI) ?
					std::clamp(opticalLagStrength_UI, 0.0F, 4.0F) :
					1.0F;
			editedProfile.shaderData.parallax.sceneDepth =
				std::clamp(sceneDepth_UI, 0.0F, 4.0F);
			editedProfile.shaderData.parallax.shadowDepth =
				std::clamp(shadowDepth_UI, 0.0F, 4.0F);
			editedProfile.shaderData.parallax.imageStillness =
				std::clamp(imageStillness_UI, 0.0F, 1.0F);
			editedProfile.shaderData.parallax.axialBreathing =
				std::clamp(axialBreathing_UI, 0.0F, 4.0F);
			editedProfile.shaderData.parallax.recenterSpeed =
				std::clamp(recenterSpeed_UI, 0.1F, 10.0F);
			editedProfile.shaderData.parallax.tubeDepth =
				std::clamp(tubeDepth_UI, 0.0F, 1.0F);
			editedProfile.shaderData.lensOffset[0] =
				std::clamp(lensOffset_UI[0], -1.0F, 1.0F);
			editedProfile.shaderData.lensOffset[1] =
				std::clamp(lensOffset_UI[1], -1.0F, 1.0F);
			editedProfile.shaderData.lensScale =
				std::clamp(lensScale_UI, 0.25F, 2.0F);
			editedProfile.shaderData.breathing.rate =
				std::clamp(breathRate_UI, 0.0F, 4.0F);
			editedProfile.shaderData.breathing.sway =
				std::clamp(breathSway_UI, 0.0F, 1.0F);
			editedProfile.shaderData.breathing.drift =
				std::clamp(breathDrift_UI, 0.0F, 1.0F);
			editedProfile.shaderData.breathing.figure =
				std::clamp(breathFigure_UI, 0.0F, 1.0F);
			editedProfile.shaderData.breathing.hold =
				std::clamp(breathHold_UI, 0.0F, 1.0F);
			editedProfile.shaderData.breathing.pupilFollow =
				std::clamp(breathPupilFollow_UI, 0.0F, 2.0F);
			editedProfile.shaderData.bBoltDisable = bDisableWhileBolt;
			editedProfile.shaderData.nvIntensity = nvIntensity_UI;
			editedProfile.shaderData.fovAdjust = fovBase_UI;

			editedProfile.shaderData.rectSize[0] = Size_rect_UI[0];
			editedProfile.shaderData.rectSize[1] = Size_rect_UI[1];
			editedProfile.shaderData.rectSize[2] = Size_rect_UI[2];
			editedProfile.shaderData.rectSize[3] = Size_rect_UI[3];

			// The override editor works on Imgui_ZDO directly; the live weapon
			// only mirrors it as a preview, so save the editor values.
			editedProfile.zoomDataOverwrite = Imgui_ZDO;

			editedProfile.additionalKeywords = additionalKeywords;

			RequestProfileSave(editedProfile);

			bIsSaving.store(false, std::memory_order_release);
		}
	}

	void ImGuiImplClass::MapScopeShaderEffect()
	{
		Hook::D3D::bLegacyMode = bLegacyMode;

		scopeData.ScopeEffect_Offset = { LF(PositionOffset_UI[0]), LF(PositionOffset_UI[1]) };

		scopeData.ScopeEffect_OriPositionOffset = { LF(OriPositionOffset_UI[0]), LF(OriPositionOffset_UI[1]) };
		scopeData.ScopeEffect_Size = { LF(Size_UI[0]), LF(Size_UI[1]) };
		scopeData.ScopeEffect_OriSize = { OriSize_UI[0], OriSize_UI[1] };

		scopeData.ReticleSize = ReticleSize_UI;
		scopeData.reticle_Offset = { reticle_Offset[0] / 1000.0F, reticle_Offset[1] / 1000.0F };

		scopeData.parallax_Radius = radius_UI;
		scopeData.parallax_relativeFogRadius = relativeFogRadius_UI;
		scopeData.parallax_scopeSwayAmount = scopeSwayAmount_UI;
		scopeData.parallax_maxTravel = maxTravel_UI;
		scopeData.scopeDepth = { sceneDepth_UI, shadowDepth_UI };
		scopeData.scopeDepthSeparation = {
			imageStillness_UI,
			axialBreathing_UI,
			1.0F,
			tubeDepth_UI
		};

		scopeData.BaseWeaponPos = baseWeaponPos_UI;
		scopeData.MovePercentage = MovePercentage_UI;
		scopeData.EnableZMove = bEnableZMove;
		scopeData.EnableNV = bEnableNVGEffect;

		scopeData.isCircle = IsCircle_UI;
		scopeData.camDepth = camDepth_UI;
		scopeData.baseFovAdjustTarget = fovBase_UI;

		auto Size_rect_UI_A = LFA(Size_rect_UI, 4);
		scopeData.rect = { Size_rect_UI_A[0], Size_rect_UI_A[1], Size_rect_UI_A[2], Size_rect_UI_A[3] };

		scopeData.nvIntensity = nvIntensity_UI;

		scopeData.FishEyeStrength = fishEyeStrength_UI;
		scopeData.FishEyePower = fishEyePower_UI;

		// Feed the unsaved zoom bounds to the renderer so the magnification
		// sliders preview live while edit mode is on.
		Hook::D3D::editZoomMin = minZoom_UI;
		Hook::D3D::editZoomMax = maxZoom_UI;

		d3d->MapScopeEffectBuffer(scopeData);
	}

	void ImGuiImplClass::MainMenuSection()
	{
		if (!ImGui::CollapsingHeader("Behavior", ImGui::ImGuiTreeNodeFlags_DefaultOpen)) {
			return;
		}

		ImGui::Checkbox("Legacy Mode", &bLegacyMode);
		Tip("Renders the magnification as a screen-space circle over the scope.\n"
			"Turn off only for weapons patched with an marked lens material.\n"
			"Takes effect after Save Profile.");
		Hook::D3D::bLegacyMode = bLegacyMode;

		ImGui::Checkbox("Keep Scope Geometry (STS Mode)", &UsingSTS_UI);
		Tip("Leaves the scope's own lens and reticle visible instead of hiding them\n"
			"while the effect runs. Keep enabled for See Through Scopes setups.");

		ImGui::Checkbox("Hide Effect During Bolt Cycling", &bDisableWhileBolt);
		Tip("Temporarily removes the magnified image while the bolt or charging\n"
			"animation plays, so the effect does not float over the moving weapon.");

		ImGui::DragInt("Effect Delay (ms)", (int*)&scopeFrame_UI, 1, 0, 5000);
		Tip("Wait this long after aiming before the magnified image appears,\n"
			"to line up with the aim-in animation.");

		ImGui::Spacing();

		if (ImGui::TreeNode("Additional Keywords")) {
			ImGui::TextWrapped(
				"This profile only activates when the player also has every keyword "
				"listed here. Use it to bind a profile to one specific scope "
				"attachment when a weapon has interchangeable optics.");

			ImGui::InputInt("Keyword Count", &additionalKeywords_count);
			Tip("Number of keyword slots. Set to 0 if this profile should apply\n"
				"regardless of which attachments are equipped.");

			if (additionalKeywords_count < additionalKeywords.size())
				additionalKeywords.pop_back();

			additionalKeywords_count = std::max(0, std::min(100, additionalKeywords_count));

			for (int i = 0; i < additionalKeywords_count; i++) {
				std::string label = "Keyword " + std::to_string(i + 1);

				std::array<char, 256> text{};
				if (i < additionalKeywords.size()) {
					strncpy_s(text.data(), text.size(), additionalKeywords[i].c_str(), _TRUNCATE);
				}
				ImGui::InputTextWithHint(
					label.c_str(),
					"Editor ID, e.g. AnimsXM2010_scopeKM",
					text.data(),
					text.size());
				Tip("Keyword editor ID that must be present on the player.\n"
					"Save Profile to apply keyword changes.");

				if (i < additionalKeywords.size())
					additionalKeywords[i] = text.data();
				else
					additionalKeywords.emplace_back(text.data());
			}
			ImGui::TreePop();
		}

		ImGui::Spacing();

		// These controls edit only copied profile data. RenderImgui publishes
		// the complete preview after every section, and HookedUpdate applies
		// it to the selected BGSZoomData from the game thread.
		ImGui::Checkbox(
			"Override Sighted Zoom and Camera",
			&Imgui_ZDO.enableZoomDateOverwrite);
		const bool overridesAllowed =
			MagnaScope::GetSettings().AllowsOverrides();
		if (!overridesAllowed) {
			Imgui_ZDO.enableZoomDateOverwrite = false;
		}
		Tip("Overrides the weapon's own sighted zoom and camera position while this\n"
			"profile is active. Changes below preview live on the equipped weapon\n"
			"and are stored with Save Profile.\n"
			"Warning: lowering the FOV multiplier on a tube scope can leave the\n"
			"camera inside the scope model, which shows up as a black screen.");
		if (!overridesAllowed) {
			ImGui::TextDisabled(
				"Override preview is disabled by the current verification stage.");
		} else {
			const auto authoredZoom = GetAuthoredZoomSnapshot();
			const bool authoredZoomMatchesSelection =
				authoredZoom.available &&
				authoredZoom.selectionRevision == selectionRevision_UI;
			if (!authoredZoomMatchesSelection) {
				ImGui::BeginDisabled();
			}
			if (ImGui::Button("Use Authored Zoom Data")) {
				// Copy only the fields that Fallout actually stores in
				// BGSZoomData. Lens-only magnification remains independent.
				Imgui_ZDO.fovMul = authoredZoom.values.fovMul;
				Imgui_ZDO.x = authoredZoom.values.x;
				Imgui_ZDO.y = authoredZoom.values.y;
				Imgui_ZDO.z = authoredZoom.values.z;
				Imgui_ZDO.enableZoomDateOverwrite = true;
			}
			Tip("Copies the selected scope's pre-MagnaScope FOV multiplier and\n"
				"camera X/Y/Z offset as an editable starting point. Fallout's\n"
				"BGSZoomData has no camera rotation fields, so none are copied.");
			if (!authoredZoomMatchesSelection) {
				ImGui::EndDisabled();
			}
		}
		if (overridesAllowed && Imgui_ZDO.enableZoomDateOverwrite) {
			if (equippedZoomDataAvailable.load(std::memory_order_acquire)) {
				ImGui::DragFloat("FOV Multiplier", &Imgui_ZDO.fovMul, 0.01F, 0, 30, "%.3f");
				Tip("The weapon's sighted zoom strength. Lower values zoom the whole\n"
					"first-person view in further while aiming.");
				const bool cameraOverrideAllowed =
					MagnaScope::GetSettings().AllowsCameraOverrides();
				if (!cameraOverrideAllowed) {
					ImGui::BeginDisabled();
				}
				ImGui::DragFloat("Camera Offset X", &Imgui_ZDO.x, 0.1F, -50, 50, "%.3f");
				Tip("Sideways camera position while aiming, in game units.");
				ImGui::DragFloat("Camera Offset Y", &Imgui_ZDO.y, 0.1F, -50, 50, "%.3f");
				Tip("Forward and back camera position while aiming, in game units.\n"
					"More negative moves the eye away from the scope.");
				ImGui::DragFloat("Camera Offset Z", &Imgui_ZDO.z, 0.1F, -50, 50, "%.3f");
				Tip("Vertical camera position while aiming, in game units.");
				if (!cameraOverrideAllowed) {
					ImGui::EndDisabled();
					ImGui::TextDisabled(
						"Camera preview is disabled for Stage 2a.");
				}

				// Companion control: the scope overlay's magnification, so the
				// circle zoom can be rebalanced right where the game zoom is
				// being overridden. Shares its value with the Magnification
				// slider under Magnified Image.
				ImGui::DragFloat(
					"Scope Magnification",
					&minZoom_UI,
					0.01F,
					1.0F,
					15.0F,
					"%.2fx");
				Tip("Magnification inside the scope circle. Raise it to compensate\n"
					"when you lower the FOV multiplier above, so the scope keeps its\n"
					"optical zoom while the rest of the screen stays wide.");
			} else {
				ImGui::TextWrapped("The equipped weapon has no zoom data to override.");
			}
		}
	}

	void ImGuiImplClass::ShaderDataSection()
	{
		if (ImGui::CollapsingHeader("Magnified Image")) {
			ImGui::Checkbox("Circular Area", &IsCircle_UI);
			Tip("Draws the magnified image as a circle. Turn off for a rectangular\n"
				"area, which fits holographic sights and camera-style scopes.");

			ImGui::DragFloat("Eye Relief Response", &camDepth_UI, 0.01F, 0, 15);
			Tip("How strongly the magnified image shifts against your view movement,\n"
				"simulating eye relief behind the scope. Higher values move less.");

			ImGui::Spacing();

			ImGui::DragFloat(
				"Magnification",
				&minZoom_UI,
				0.01F,
				1.0F,
				15.0F,
				"%.2fx");
			Tip("Magnification inside the scope when you start aiming. For automatic\n"
				"STS scopes this stacks on top of the scope's own zoom.\n"
				"Previews live while aiming in edit mode.");
			ImGui::DragFloat("Max Scroll Magnification", &maxZoom_UI, 0.01F, 1.0F, 15.0F, "%.2fx");
			Tip("Upper limit for the mouse wheel zoom while aiming.");

			ImGui::Spacing();

			ImGui::DragFloat(
				"Lens Size",
				&lensScale_UI,
				0.005F,
				0.25F,
				2.0F,
				"%.3f");
			Tip("Diameter of the sight picture, as a fraction of the scope's\n"
				"own glass. 1 fills it. Below 1 leaves a ring of tube wall\n"
				"around the image; above 1 pushes the image past the glass so\n"
				"the housing crops it and no ring is visible.\n"
				"For automatic STS scopes this is the control that resizes the\n"
				"magnified area -- Circle Size under the legacy overlay does\n"
				"nothing here, because the scope's own geometry is the lens.");
			ImGui::DragFloat2(
				"Lens Center",
				lensOffset_UI,
				0.002F,
				-1.0F,
				1.0F,
				"%.3f");
			Tip("Moves the sight picture inside the housing, in units of the\n"
				"scope's own radius, along the scope's axes -- so it stays put\n"
				"when the weapon rolls. Use this when the magnified circle sits\n"
				"off-center in the scope model. Positive X is right, positive Y\n"
				"is up as the optic is oriented. This moves the magnified image,\n"
				"its shadow, and the exit pupil together; the reticle keeps its\n"
				"own Reticle Offset.");

			ImGui::Spacing();

			ImGui::DragFloat("Fish Eye Strength", &fishEyeStrength_UI, 0.01F, 0.0F, 2.0F, "%.2f");
			Tip("Bends the image toward the edge of the lens like a wide-angle camera,\n"
				"similar to the scopes in Modern Warfare 2019. 0 turns it off.");
			ImGui::DragFloat("Fish Eye Curve", &fishEyePower_UI, 0.01F, 0.5F, 6.0F, "%.2f");
			Tip("How sharply the bend ramps up toward the edge of the lens.\n"
				"Higher values keep the center flat and push the distortion\n"
				"out to the rim.");
			ImGui::DragFloat(
				"Edge Refraction Strength",
				&edgeRefractionStrength_UI,
				0.001F,
				0.0F,
				0.25F,
				"%.3f");
			Tip("Adds a subtle glass-like radial displacement only near the\n"
				"edge of the lens. 0 disables the effect.");
			ImGui::DragFloat(
				"Edge Refraction Width",
				&edgeRefractionWidth_UI,
				0.005F,
				0.02F,
				0.5F,
				"%.3f");
			Tip("Controls how far the edge-refraction band reaches toward the\n"
				"center. Smaller values confine it more tightly to the rim.");
			ImGui::DragFloat(
				"Edge Chromatic Aberration",
				&edgeChromaticAberration_UI,
				0.01F,
				0.0F,
				2.0F,
				"%.2f");
			Tip("Separates red and blue by a bounded number of source pixels\n"
				"inside the refracted rim. 0 disables color separation.");
			ImGui::DragFloat(
				"Edge-Aware Cleanup",
				&imageDenoise_UI,
				0.01F,
				0.0F,
				1.0F,
				"%.2f");
			Tip("Blends nearby samples only when their colors are similar.\n"
				"This can soften magnification shimmer without smearing strong\n"
				"edges. It cannot reconstruct detail missing from the frame.");
			ImGui::DragFloat(
				"Image Sharpen",
				&imageSharpen_UI,
				0.01F,
				0.0F,
				1.0F,
				"%.2f");
			Tip("Restores local contrast after magnification or cleanup.\n"
				"High values can emphasize halos and temporal artifacts.");

			ImGui::Spacing();

			if (bLegacyMode)
				ImGui::DragFloat2("Circle Position", PositionOffset_UI, 0.1F, -3840, 3840);
			else
				ImGui::DragFloat2("Circle Position", PositionOffset_UI, 0.1F, -1000, 1000, "%.2f");
			Tip("Moves the magnified area on screen, in 1080p reference pixels from\n"
				"the scope's center. Use it to line the circle up with the lens.");

			if (bLegacyMode) {
				ImGui::DragFloat2("Circle Size", Size_UI, 1.0F, 0, 3840, "%.4f");
				Tip("Diameter of the magnified circle, in 1080p reference pixels.");
			} else {
				if (IsCircle_UI) {
					ImGui::DragFloat2("Circle Size", Size_UI, 1.0F, 0, 3840, "%.4f");
					Tip("Diameter of the magnified circle, in 1080p reference pixels.");
				} else {
					ImGui::DragFloat4("Rectangle Bounds", Size_rect_UI, 1.0F, -1200, 1200, "%.2f");
					Tip("Left, top, right and bottom bounds of the rectangular area,\n"
						"in 1080p reference pixels.");
				}
			}

			ImGui::Spacing();

			if (bLegacyMode)
				ImGui::DragFloat2("Source Position", OriPositionOffset_UI, 0.1F, -3840, 3840);
			else
				ImGui::DragFloat2("Source Position", OriPositionOffset_UI, 0.1F, -1000, 1000, "%.2f");
			Tip("Moves the area of the scene that gets magnified, without moving the\n"
				"circle itself. Use it when the zoomed image looks off-center.");

			ImGui::Spacing();

			ImGui::DragFloat("Reticle Size", &ReticleSize_UI, 0.01F, 0, 128);
			Tip("Scales the isolated STS reticle around its authored geometric\n"
				"center. 4.00 preserves authored size before the STS reticle\n"
				"magnification multiplier is applied.");
			ImGui::DragFloat2("Reticle Offset", reticle_Offset, 0.01F, -1000.0F, 1000.0F);
			Tip("Moves the isolated reticle in the optic's local X/Z frame. The\n"
				"offset follows optic roll and inertia without moving the aperture.");
			if (currData && currData->autoProfile) {
				ImGui::DragFloat(
					"STS Reticle Magnification",
					&reticleMagnification_UI,
					0.01F,
					0.25F,
					8.0F,
					"%.2fx");
				Tip("Scales the STS-authored 3D reticle around its own geometric\n"
					"vertex center. 1.00x preserves the authored size and remains\n"
					"independent of scene magnification.");
				ImGui::DragFloat(
					"Reticle Shadow Strength",
					&reticleShadowStrength_UI,
					0.01F,
					0.0F,
					1.0F,
					"%.2f");
				Tip("Controls how strongly the exit-pupil shadow dims the reticle. 0 keeps the reticle visible across the aperture.");
				ImGui::DragFloat(
					"Reticle Parallax Strength",
					&reticleParallaxStrength_UI,
					0.01F,
					0.0F,
					4.0F,
					"%.2f");
				Tip("Scales reticle motion with optical parallax without changing its size.");
			}
		}

		if (ImGui::CollapsingHeader("Effects")) {
			ImGui::Checkbox("Night Vision", &bEnableNVGEffect);
			Tip("Allows a night-vision tint inside the scope, switched in game with\n"
				"the hotkeys configured above.");
			ImGui::DragFloat("Night Vision Intensity", &nvIntensity_UI, 0.1F, 0, 1000);
			Tip("Brightness gain of the night-vision effect.");

			ImGui::Spacing();

			ImGui::Checkbox("Recoil Zoom Response", &bEnableZMove);
			Tip("Shrinks and grows the magnified image as the weapon kicks toward\n"
				"and away from your eye, simulating scope shadow under recoil.");

			if (bEnableZMove) {
				Hook::D3D::GameConstBuffer* gameConstBuffer = d3d->GetGameConstBuffer();
				float currWeaponPos =
					powf(gameConstBuffer->weaponPos.x - gameConstBuffer->rootPos.x, 2) + powf(gameConstBuffer->weaponPos.y - gameConstBuffer->rootPos.y, 2) + powf(gameConstBuffer->weaponPos.z - gameConstBuffer->rootPos.z, 2);

				ImGui::Text("Current weapon distance: %.3f", currWeaponPos);
				ImGui::DragFloat("Resting Weapon Distance", &baseWeaponPos_UI, 0.05F);
				Tip("The weapon distance while aiming at rest. Match this to the value\n"
					"shown above while standing still and aiming.");
				ImGui::DragFloat("Response Strength", &MovePercentage_UI, 0.01F, -10, 10);
				Tip("How strongly recoil movement scales the image. 0 disables it.");
			}
		}
	}

	void ImGuiImplClass::ParallaxDataSection()
	{
		if (ImGui::CollapsingHeader("Eye Box and Vignette")) {
			ImGui::DragFloat("Eye Box Radius", &radius_UI, 0.01F, 0, 20);
			Tip("How far your eye can wander from the scope's axis before the image\n"
				"starts to fog out, measured relative to the detected ScopeFade\n"
				"aperture. Larger values are more forgiving.");
			ImGui::DragFloat("Vignette Reach", &relativeFogRadius_UI, 0.01F, 0, 20);
			Tip("How far the dark vignette reaches into the image from the edge.\n"
				"Higher values darken the scope edge sooner.");
			ImGui::DragFloat("Vignette Sharpness", &scopeSwayAmount_UI, 0.01F, 0, 20);
			Tip("How abruptly the image transitions into the dark edge.\n"
				"Higher values give a harder edge.");
			if (currData && currData->autoProfile) {
				ImGui::DragFloat(
					"Maximum Eye Travel",
					&maxTravel_UI,
					0.01F,
					0.0F,
					4.0F);
				Tip("Clamps how far the measured exit pupil can move across the\n"
					"ScopeFade aperture during sway, recoil, or weapon inertia.");
			} else {
				// Preserve the original scope-rendering control and JSON meaning for
				// explicit profiles. Automatic STS profiles reinterpret this
				// scalar only inside their physical ScopeFade shader.
				ImGui::DragFloat(
					"Maximum Brightness",
					&maxTravel_UI,
					0.01F,
					0.0F,
					20.0F);
				Tip("Brightness cap for the magnified image. 1.0 shows the scene at full\n"
					"brightness; lower values tint the whole scope darker.");
			}
			ImGui::DragFloat(
				"Image Lag",
				&imageStillness_UI,
				0.01F,
				0.0F,
				1.0F,
				"%.2f");
			Tip("How far the magnified image trails the reticle when you swing\n"
				"the camera. 0 locks the image to the optic so nothing lags. 1\n"
				"holds it world-static while the housing swings over it, which\n"
				"reads as a long tube with the image far behind the glass.\n"
				"Recenter Speed below controls how fast it catches back up.\n"
				"\n"
				"This replaces Scene Parallax Strength and Lens Depth\n"
				"Separation, which multiplied a second path that could not hold\n"
				"the image still at any setting. It behaves identically at\n"
				"every magnification.");
			ImGui::DragFloat(
				"Shadow Depth Separation",
				&shadowDepth_UI,
				0.01F,
				0.0F,
				4.0F,
				"%.2f");
			Tip(
				"Sets the virtual distance between the fixed aperture and the\n"
				"exit-pupil shadow. Higher values strengthen lateral shadow travel\n"
				"and subtle fore/aft pupil breathing.");
			ImGui::DragFloat(
				"Optical Lag Strength",
				&opticalLagStrength_UI,
				0.01F,
				0.0F,
				4.0F,
				"%.2f");
			Tip("Scales transient weapon-motion response for the exit-pupil\n"
				"shadow and the reticle. 0 disables it, 1 uses the measured\n"
				"movement, higher exaggerates it. It does not affect how far\n"
				"the image lags -- Image Lag alone governs that.");
			ImGui::DragFloat(
				"Axial Breathing",
				&axialBreathing_UI,
				0.01F,
				0.0F,
				4.0F,
				"%.2f");
			Tip("Apparent size change as you move toward or away from the target.\n"
				"0 keeps apparent size fixed, which is usually what you want:\n"
				"any value here makes walking forward and backward subtly zoom\n"
				"the image. Independent of Image Lag, so lateral lag can be\n"
				"raised without adding depth wobble.");
			ImGui::DragFloat(
				"Tube Depth",
				&tubeDepth_UI,
				0.01F,
				0.0F,
				1.0F,
				"%.2f");
			Tip("Recesses the magnified image toward the front of the tube so a\n"
				"ring of shadow separates it from the rear aperture. 0 fills the\n"
				"glass; higher values open that gap and give the optic real\n"
				"depth for the exit pupil to slide across.");
			ImGui::DragFloat(
				"Recenter Speed",
				&recenterSpeed_UI,
				0.01F,
				0.1F,
				10.0F,
				"%.2f");
			Tip("How quickly the image catches back up to the reticle, and the\n"
				"shadow to centre, once motion stops. 1 is the tuned default,\n"
				"lower is slower and more floaty, higher snaps back sooner.\n"
				"This is the companion to Image Lag: that sets how far behind\n"
				"the image falls, this sets how long it stays there.");
		}

		if (ImGui::CollapsingHeader("Breathing")) {
			ImGui::TextWrapped(
				"Slow cyclic sway of the sight picture. Independent of the "
				"recoil and inertia above: those are transient and settle back "
				"to centre, this never stops while you are aiming.");
			ImGui::Spacing();

			ImGui::DragFloat(
				"Breathing Sway",
				&breathSway_UI,
				0.001F,
				0.0F,
				1.0F,
				"%.3f");
			Tip("Vertical amplitude, in scope radii. This is the main control:\n"
				"0 disables breathing entirely. Around 0.05 is a calm hold and\n"
				"0.2 is winded. The amount is apparent motion, so it looks the\n"
				"same through a 4x and a 12x rather than becoming unusable at\n"
				"high magnification.");
			ImGui::DragFloat(
				"Breathing Drift",
				&breathDrift_UI,
				0.001F,
				0.0F,
				1.0F,
				"%.3f");
			Tip("Horizontal amplitude, in scope radii. Real breathing is mostly\n"
				"vertical, so keeping this well below Sway reads best. Equal\n"
				"values give a circular wander.");
			ImGui::DragFloat(
				"Breathing Rate",
				&breathRate_UI,
				0.005F,
				0.0F,
				4.0F,
				"%.3f Hz");
			Tip("Breaths per second. 0.25 is about fifteen a minute, a resting\n"
				"rate. Raise it toward 0.5 for exertion. Changing this bends\n"
				"the curve forward from where it is rather than jumping the\n"
				"image, so it is safe to drag while aiming.");
			ImGui::DragFloat(
				"Breathing Figure",
				&breathFigure_UI,
				0.005F,
				0.0F,
				1.0F,
				"%.3f");
			Tip("Phase lead of the horizontal axis over the vertical, in turns.\n"
				"0 traces a straight diagonal, 0.25 an ellipse, and values in\n"
				"between the leaning figure-eight a real hold wanders through.\n"
				"Has no visible effect unless Drift is above 0.");
			ImGui::DragFloat(
				"Breathing Hold",
				&breathHold_UI,
				0.005F,
				0.0F,
				1.0F,
				"%.3f");
			Tip("Flattens the turning points so the drift dwells at the top and\n"
				"bottom of each breath instead of sweeping through at constant\n"
				"speed. 0 is a pure sine; higher values give the pause at the\n"
				"end of a breath that makes the timing readable.");
			ImGui::DragFloat(
				"Breathing Pupil Follow",
				&breathPupilFollow_UI,
				0.01F,
				0.0F,
				2.0F,
				"%.2f");
			Tip("How much of the sway the exit-pupil shadow takes. The image\n"
				"and the pupil sit at different depths in a real optic, so they\n"
				"need not move together: 0 holds the shadow perfectly still\n"
				"while the scene swims, 1 moves them as one, and above 1 the\n"
				"shadow leads. The reticle never translates with breathing --\n"
				"only its shadow does.");
		}
	}

	ImGuiImplClass::ImGuiImplClass()
	{
		if (!sdh)
			sdh = ScopeData::ScopeDataHandler::GetSingleton();

		d3d = Hook::D3D::GetSington();
		currData = sdh->GetCurrentScopeProfile();
	}

	void ImGuiImplClass::RenderImgui()
	{
		auto* instance = GetSington();
		if (!instance->CheckAndInit()) {
			ImGui::TextUnformatted("Unable to access critical data.");
			return;
		}

		ImGui::PushItemWidth(ImGui::GetFontSize() * 14.0F);

		GeneralSettingsSection();
		KeyBindingSection(instance->nvgComboKeyIndex, instance->nvgMainKeyIndex);

		ResetUIData(instance);

		ImGui::SeparatorText("Equipped Scope");

		if (!currData) {
			ImGui::TextWrapped(
				"No scope profile is active. Equip a weapon with an STS-configured scope, "
				"then reopen this page.");
			ImGui::PopItemWidth();
			return;
		}

		if (currData->autoProfile) {
			ImGui::TextUnformatted("Profile: automatic (detected STS scope)");
			ImGui::TextDisabled("%s", currData->keywordName.c_str());
			ImGui::TextWrapped(
				"This scope works without a patch. Saving below creates an editable "
				"profile under Data/F4SE/Plugins/MagnaScope/Auto.");
		} else {
			ImGui::TextUnformatted("Profile: from file");
			ImGui::TextDisabled("%s", currData->path.c_str());
		}

		ImGui::Spacing();

		bool editMode =
			Hook::D3D::bEnableEditMode.load(std::memory_order_acquire);
		if (ImGui::Checkbox("Edit Mode", &editMode)) {
			Hook::D3D::bEnableEditMode.store(
				editMode,
				std::memory_order_release);
			if (!editMode) {
				// HookedUpdate observes the cleared request, restores the
				// stored profile override, and republishes magnification.
				ClearEditorPreview();
			}
		}
		Tip("Live-preview changes on the equipped scope while you aim.\n"
			"Closing the menu without saving reverts the preview.");

		ImGui::SameLine();
		if (ImGui::Checkbox("Hold Aim", &bHoldAim)) {
			SetHoldAim(bHoldAim);
		}
		Tip("Forces the character to hold the weapon aimed while this menu is\n"
			"open, so you can adjust values while looking through the scope.\n"
			"Game controls are suspended while this is on. Released\n"
			"automatically when the menu closes.");

		if (!editMode) {
			ImGui::TextWrapped("Enable Edit Mode to preview and save changes for the equipped scope.");
			ImGui::PopItemWidth();
			return;
		}

		instance->ReloadData();
		ImGui::SameLine();
		instance->SaveData();
		ImGui::Spacing();

		instance->MainMenuSection();
		instance->ShaderDataSection();
		instance->ParallaxDataSection();
		instance->MapScopeShaderEffect();
		PublishEditorPreview(
			instance->Imgui_ZDO,
			instance->selectionRevision_UI,
			instance->minZoom_UI,
			instance->imageDenoise_UI,
			instance->imageSharpen_UI,
			instance->fishEyeStrength_UI,
			instance->fishEyePower_UI,
			instance->edgeRefractionStrength_UI,
			instance->edgeRefractionWidth_UI,
			instance->edgeChromaticAberration_UI,
			instance->reticleMagnification_UI,
			instance->ReticleSize_UI,
			instance->reticle_Offset[0],
			instance->reticle_Offset[1],
			instance->radius_UI,
			instance->relativeFogRadius_UI,
			instance->scopeSwayAmount_UI,
			instance->maxTravel_UI,
			instance->sceneParallaxStrength_UI,
			instance->opticalLagStrength_UI,
			instance->reticleShadowStrength_UI,
			instance->reticleParallaxStrength_UI,
			instance->sceneDepth_UI,
			instance->shadowDepth_UI,
			instance->imageStillness_UI,
			instance->axialBreathing_UI,
			instance->recenterSpeed_UI,
			instance->tubeDepth_UI,
			instance->lensOffset_UI[0],
			instance->lensOffset_UI[1],
			instance->lensScale_UI,
			ScopeData::Breathing{
				instance->breathRate_UI,
				instance->breathSway_UI,
				instance->breathDrift_UI,
				instance->breathFigure_UI,
				instance->breathHold_UI,
				instance->breathPupilFollow_UI });

		ImGui::PopItemWidth();
	}

	namespace
	{
		void __stdcall RenderScopeVisualProbe()
		{
			const auto& settings = MagnaScope::GetSettings();
			if (!settings.AllowsVisualProbe()) {
				return;
			}

			const auto projection =
				Hook::D3D::GetSington()->GetLensProjectionSnapshot();
			if (!projection.renderEnabled || !projection.automaticSTS ||
				!projection.trackingReady ||
				projection.sourceWidth <= 0.0F ||
				projection.sourceHeight <= 0.0F ||
				projection.radiusX <= 0.0F ||
				projection.radiusY <= 0.0F ||
				projection.activationProgress <= 0.0F) {
				return;
			}

			const auto* io = ImGui::GetIO();
			if (!io || io->DisplaySize.x <= 0.0F ||
				io->DisplaySize.y <= 0.0F) {
				return;
			}

			// WorldPointToScreen publishes coordinates in the game render
			// viewport. Menu Framework draws in its displayed viewport, which
			// can differ when an upscaler or frame-generation proxy is active.
			const float scaleX = io->DisplaySize.x / projection.sourceWidth;
			const float scaleY = io->DisplaySize.y / projection.sourceHeight;
			const ImGui::ImVec2 center{
				projection.centerX * scaleX,
				projection.centerY * scaleY
			};
			// Project each source axis independently, then choose the smaller
			// displayed radius so a nonuniform upscaler viewport cannot push
			// the circular diagnostic outside the physical aperture.
			const float fullRadius = std::min(
				projection.radiusX * scaleX,
				projection.radiusY * scaleY);
			// The center remains locked to the current ScopeFade projection.
			// Scaling only radius and alpha produces a smooth aim-in transition
			// without adding the positional lag seen in the first probe.
			const float radius =
				fullRadius * projection.activationProgress;
			if (!std::isfinite(center.x) || !std::isfinite(center.y) ||
				!std::isfinite(radius) || radius <= 1.0F) {
				return;
			}

			auto* drawList = ImGui::GetForegroundDrawList();
			if (!drawList) {
				return;
			}
			ImGui::ImDrawListManager::AddCircleFilled(
				drawList,
				center,
				radius,
				ImGui::ColorConvertFloat4ToU32(
					ImGui::ImVec4{
						0.0F,
						0.86F,
						1.0F,
						0.28F * projection.activationProgress }),
				96);
			ImGui::ImDrawListManager::AddCircle(
				drawList,
				center,
				radius,
				ImGui::ColorConvertFloat4ToU32(
					ImGui::ImVec4{
						0.0F,
						0.94F,
						1.0F,
						0.86F * projection.activationProgress }),
				96,
				4.0F);

			static std::once_flag loggedVisualProbe;
			std::call_once(loggedVisualProbe, [&] {
				logger::info(
					"Stage 4a.2 ScopeFade-plane HUD aperture probe visible: "
					"center=({:.2f}, {:.2f}), aim=({:.2f}, {:.2f}), "
					"fullRadius={:.2f}, display={}x{}; "
					"no MagnaScope shaders or render-target binds",
					center.x,
					center.y,
					projection.aimCenterX * scaleX,
					projection.aimCenterY * scaleY,
					fullRadius,
					io->DisplaySize.x,
					io->DisplaySize.y);
			});
		}

		// Ends the customization session: turns off edit mode, reverts the
		// unsaved zoom preview on the live weapon, and reloads the on-screen
		// effect from the stored profile values.
		void EndEditSession()
		{
			if (scopeEditorWindow) {
				scopeEditorWindow->BlockUserInput.store(
					false,
					std::memory_order_release);
			}
			Hook::D3D::bEnableEditMode.store(
				false,
				std::memory_order_release);
			ClearEditorPreview();
			if (bHoldAim) {
				SetHoldAim(false);
			}
			if (d3d) {
				// Selection restoration is requested rather than executed
				// from this renderer-thread callback.
				RequestProfileAction(ProfileRequest::kReselect);
			}
		}

		void __stdcall OnFrameworkEvent(F4SEMenuFramework::Events::Type type)
		{
			if (type == F4SEMenuFramework::Events::kBeforeRender) {
				Hook::D3D::GetSington()->RenderFromFramework();
			} else if (type == F4SEMenuFramework::Events::kOpenMenu) {
				frameworkMenuOpen.store(true, std::memory_order_release);
				// WindowManager closes every blocking plugin window before it
				// dispatches kCloseMenu. Keep the popout nonblocking while the
				// main panel owns input so it survives that close sweep.
				if (scopeEditorWindow) {
					scopeEditorWindow->BlockUserInput.store(
						false,
						std::memory_order_release);
				}
			} else if (type == F4SEMenuFramework::Events::kCloseMenu) {
				frameworkMenuOpen.store(false, std::memory_order_release);
				// The editor popout is a non-pausing window that survives the
				// Mod Control Panel. Arm mouse capture only after the
				// framework's blocking-window close sweep has completed.
				if (scopeEditorWindow && scopeEditorWindow->IsOpen.load()) {
					ApplyPopoutInputCapture();
					if (!captureMouseOutsidePanel.load(
							std::memory_order_acquire) &&
						bHoldAim) {
						SetHoldAim(false);
					}
					return;
				}
				if (scopeEditorWindow) {
					scopeEditorWindow->BlockUserInput.store(
						false,
						std::memory_order_release);
				}
				EndEditSession();
			}
		}
	}

	bool RegisterMenu()
	{
		static bool registered = false;
		if (registered) {
			return true;
		}
		if (!F4SEMenuFramework::IsInstalled()) {
			logger::warn("F4SE Menu Framework is not loaded; scope customization is unavailable");
			return false;
		}

		F4SEMenuFramework::SetSection("MagnaScope");
		F4SEMenuFramework::AddSectionItem("Scope Customization", RenderMenu);
		// Non-pausing window (second argument false): the framework keeps
		// rendering it after the Mod Control Panel closes, so the editor can
		// stay on screen while aiming through the scope.
		scopeEditorWindow = F4SEMenuFramework::AddWindow(RenderPopout, false);
		scopeVisualProbe =
			F4SEMenuFramework::AddHudElement(RenderScopeVisualProbe);
		if (!scopeVisualProbe) {
			logger::error("F4SE Menu Framework HUD probe registration failed");
			return false;
		}
		const auto eventHandle = F4SEMenuFramework::Events::RegisterPriority(
			OnFrameworkEvent,
			-100.0F);
		if (eventHandle < 0) {
			logger::error("F4SE Menu Framework render-event registration failed");
			return false;
		}
		Hook::D3D::GetSington()->SetFrameworkRenderAnchor(true);
		registered = true;
		logger::info("Registered scope customization with F4SE Menu Framework");
		return true;
	}

	void __stdcall RenderMenu()
	{
		bool popoutOpen = scopeEditorWindow && scopeEditorWindow->IsOpen.load();
		if (ImGui::Checkbox("Open controls in popout window", &popoutOpen) && scopeEditorWindow) {
			if (!popoutOpen) {
				scopeEditorWindow->BlockUserInput.store(
					false,
					std::memory_order_release);
				scopeEditorWindow->IsOpen.store(
					false,
					std::memory_order_release);
			} else {
				// Keep the popout nonblocking until the framework has finished
				// its close sweep. kCloseMenu then applies the user's capture
				// preference without the framework mistaking this persistent
				// editor for a blocking window that must also be closed.
				scopeEditorWindow->BlockUserInput.store(
					false,
					std::memory_order_release);
				scopeEditorWindow->IsOpen.store(
					true,
					std::memory_order_release);
				if (!F4SEMenuFramework::CloseMenu()) {
					// Older framework builds do not export CloseMenu. The
					// popout remains open and nonblocking, and the user can
					// close the main panel normally without a crash.
					static std::once_flag loggedMissingCloseMenu;
					std::call_once(loggedMissingCloseMenu, [] {
						logger::warn(
							"F4SE Menu Framework does not export CloseMenu; "
							"close its main panel manually to use the popout");
					});
				}
			}
		}
		Tip("Moves these controls into a movable, resizable window that stays on\n"
			"screen while aiming. Newer F4SE Menu Framework builds close the\n"
			"main panel automatically when this popout opens.");
		if (!scopeEditorWindow) {
			ImGui::TextWrapped("The popout window is unavailable.");
		}
		ImGui::Separator();
		if (popoutOpen) {
			ImGui::TextWrapped("Scope controls are open in the popout window.");
		} else {
			ImGuiImplClass::GetSington()->RenderImgui();
		}
	}

	void __stdcall RenderPopout()
	{
		auto* viewport = ImGui::GetMainViewport();
		if (viewport) {
			const float defaultWidth = std::clamp(
				viewport->Size.x * 0.30F,
				360.0F,
				500.0F);
			const float defaultHeight = std::clamp(
				viewport->Size.y * 0.72F,
				420.0F,
				720.0F);
			ImGui::SetNextWindowPos(
				{ viewport->Pos.x + 20.0F, viewport->Pos.y + 20.0F },
				ImGui::ImGuiCond_FirstUseEver,
				{ 0.0F, 0.0F });
			ImGui::SetNextWindowSize(
				{ defaultWidth, defaultHeight },
				ImGui::ImGuiCond_FirstUseEver);
		}

		if (ImGui::Begin("Scope Customization##MagnaScope", nullptr, ImGui::ImGuiWindowFlags_NoCollapse)) {
			if (ImGui::Button("Close") && scopeEditorWindow) {
				scopeEditorWindow->BlockUserInput.store(
					false,
					std::memory_order_release);
				scopeEditorWindow->IsOpen.store(false);
				// With the Mod Control Panel also closed there is nothing
				// left to edit from, so end the session and revert any
				// unsaved preview.
				if (!frameworkMenuOpen.load(std::memory_order_acquire)) {
					EndEditSession();
				}
			}
			bool captureMouse =
				captureMouseOutsidePanel.load(std::memory_order_acquire);
			if (ImGui::Checkbox(
					"Capture mouse when main menu is closed",
					&captureMouse)) {
				captureMouseOutsidePanel.store(
					captureMouse,
					std::memory_order_release);
				ApplyPopoutInputCapture();
				if (!captureMouse && bHoldAim) {
					SetHoldAim(false);
				}
			}
			Tip("When enabled, this popout captures the mouse and blocks game\n"
				"input after the main F4SE Menu Framework panel closes, so its\n"
				"controls remain interactive. Enable Hold Aim to remain sighted.\n"
				"Uncheck this to return mouse control to the game.");
			ImGui::Separator();
			ImGuiImplClass::GetSington()->RenderImgui();
		}
		ImGui::End();
	}
}
