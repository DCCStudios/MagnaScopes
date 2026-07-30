#include "ImGuiImpl.h"
#include "FTSData.h"
#include "Settings.h"
#include "hooking.h"
#include <d3d11.h>
#include <DirectXMath.h>


inline void InitCurrentScopeData();
char* _MESSAGE(const char* fmt, ...);

namespace ImGuiImpl
{
	std::vector<std::string> additionalKeywords;
	int additionalKeywords_count = 0;
	bool legacyFlag = true;
	bool bhasSaveZoomData = false;

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

	ScopeData::FTSData* currData;
	ScopeData::ScopeDataHandler* sdh;

	RE::PlayerCharacter* player;
	RE::PlayerCamera* pcam;
	bool bInitZoomData = false;
	RE::TESObjectWEAP::InstanceData* Imgui_InstanceData;

	// Attaches a hover tooltip to the widget submitted immediately before.
	void Tip(const char* text)
	{
		ImGui::SetItemTooltip("%s", text);
	}

	void RestoreOwnedZoomFields(
		RE::BGSZoomData::Data& destination,
		const RE::BGSZoomData::Data& source)
	{
		// The editor owns only sighted FOV and camera position. Preserve the
		// overlay and image-space modifier fields maintained by Fallout.
		destination.fovMult = source.fovMult;
		destination.cameraOffset = source.cameraOffset;
	}

	void ImGuiImplClass::UpdateWeaponInstance(RE::TESObjectWEAP::InstanceData* instanceData)
	{
		Imgui_InstanceData = instanceData;
	}

	void ImGuiImplClass::UpdateImGuiData()
	{
		currData = sdh->GetCurrentFTSData();
	}

	// Persistent request consumed by the game-thread update hook. A one-shot
	// transition is not sufficient because Fallout can lower the weapon again
	// while a blocking menu is open.
	std::atomic<int> pendingForcedAim{ 0 };

	namespace
	{
		// Whether the menu is currently forcing the player to hold aim.
		bool bHoldAim = false;

		void ApplyForcedAimImmediately(bool enabled)
		{
			auto* currentPlayer = RE::PlayerCharacter::GetSingleton();
			if (!currentPlayer || !currentPlayer->currentProcess) {
				return;
			}

			// The original FTS editor performed this transition directly from
			// its window handler before suppressing gameplay input. Menu
			// Framework may freeze game updates while its blocking editor is
			// open, so a game-thread-only request can otherwise never run.
			currentPlayer->SetInIronSightsImpl(enabled);
			const auto idleFormID = enabled ? 0x0004D32u : 0x0004AD9u;
			if (auto* idle = RE::TESForm::GetFormByID<RE::TESIdleForm>(idleFormID)) {
				currentPlayer->currentProcess->PlayIdle(*currentPlayer, idle, nullptr);
			}

			if (enabled && d3d && currData) {
				d3d->SetScopeEffect(true);
				d3d->EnableRender(true);
				d3d->QueryRender(true);
			}
		}

		void SetHoldAim(bool enabled)
		{
			bHoldAim = enabled;
			ApplyForcedAimImmediately(enabled);
			pendingForcedAim.store(enabled ? 1 : 0);
		}
	}

	void AbandonZoomPreview()
	{
		// Intentionally does not restore the snapshot: the instance it was
		// taken from no longer belongs to the equipped weapon and may be gone.
		bhasSaveZoomData = false;
		// This path ends the edit session without EndEditSession, so release
		// the forced aim here too; otherwise game input stays ignored with no
		// menu left to turn it off.
		if (bHoldAim) {
			SetHoldAim(false);
		}
	}

	bool ImGuiImplClass::CheckAndInit()
	{
		if (!sdh)
			sdh = ScopeData::ScopeDataHandler::GetSingleton();

		if (!player)
			player = RE::PlayerCharacter::GetSingleton();

		if (!pcam)
			pcam = RE::PlayerCamera::GetSingleton();

		if (!sdh || !player || !pcam)
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
		if (d3d && d3d->bRefreshChar) {
			// Consume the request before re-selection. If no supported scope is
			// equipped, leaving it set would retry initialization every frame.
			d3d->bRefreshChar = false;

			InitCurrentScopeData();
			currData = sdh->GetCurrentFTSData();
			if (!currData)
				return;

			auto data = currData;
			legacyFlag = data->legacyMode;

			ins->bLegacyMode = data->legacyMode;
			ins->UsingSTS_UI = ScopeDataHandler::GetSingleton()->GetCurrentFTSData()->UsingSTS;
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
			ins->radius_UI = data->shaderData.parallax.radius;
			ins->relativeFogRadius_UI = data->shaderData.parallax.relativeFogRadius;
			ins->scopeSwayAmount_UI = data->shaderData.parallax.scopeSwayAmount;
			ins->maxTravel_UI = data->shaderData.parallax.maxTravel;

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
			if (auto data = sdh->GetCurrentFTSData()) {
				// Undo only the editor's live preview first. The selection
				// transaction below restores the authored baseline and applies
				// the profile loaded from disk at the correct lifecycle point.
				if (bhasSaveZoomData && Imgui_InstanceData && Imgui_InstanceData->zoomData) {
					RestoreOwnedZoomFields(
						Imgui_InstanceData->zoomData->zoomData,
						currOriZoomData);
				}
				bhasSaveZoomData = false;

				if (!data->autoProfile || std::filesystem::exists(data->path)) {
					sdh->ReloadFTSData(data);
				}
				d3d->bRefreshChar = true;
				ResetUIData(this);
			}
		}
	}

	void ImGuiImplClass::SaveData()
	{
		const bool pressed = ImGui::Button("Save Profile", { 150, 0 });
		Tip("Writes the current values to the profile file so they persist.\n"
			"Automatic scopes save a new profile under Data/F4SE/Plugins/FTS/Auto.");
		if (pressed) {
			bIsSaving = true;
			currData = sdh->GetCurrentFTSData();
			if (!currData || !Imgui_InstanceData || !Imgui_InstanceData->zoomData) {
				bIsSaving = false;
				logger::error("Scope settings were not saved because the weapon instance is unavailable");
				return;
			}
			currData->legacyMode = bLegacyMode;
			Hook::D3D::bLegacyMode = bLegacyMode;

			currData->UsingSTS = UsingSTS_UI;
			currData->scopeFrame = scopeFrame_UI;
			currData->shaderData.IsCircle = IsCircle_UI;
			currData->shaderData.bCanEnableNV = bEnableNVGEffect;
			currData->shaderData.baseWeaponPos = baseWeaponPos_UI;
			currData->shaderData.bEnableZMove = bEnableZMove;
			currData->shaderData.movePercentage = MovePercentage_UI;
			currData->shaderData.camDepth = camDepth_UI;
			currData->shaderData.ReticleSize = ReticleSize_UI;
			currData->shaderData.minZoom = minZoom_UI;
			currData->shaderData.maxZoom = maxZoom_UI;
			currData->shaderData.reticle_Offset[0] = reticle_Offset[0];
			currData->shaderData.reticle_Offset[1] = reticle_Offset[1];
			currData->shaderData.PositionOffset[0] = PositionOffset_UI[0];
			currData->shaderData.PositionOffset[1] = PositionOffset_UI[1];
			currData->shaderData.OriPositionOffset[0] = OriPositionOffset_UI[0];
			currData->shaderData.OriPositionOffset[1] = OriPositionOffset_UI[1];
			currData->shaderData.Size[0] = Size_UI[0];
			currData->shaderData.Size[1] = Size_UI[1];
			currData->shaderData.OriSize[0] = OriSize_UI[0];
			currData->shaderData.OriSize[1] = OriSize_UI[1];
			currData->shaderData.fishEyeStrength = fishEyeStrength_UI;
			currData->shaderData.fishEyePower = fishEyePower_UI;
			currData->shaderData.parallax.radius = radius_UI;
			currData->shaderData.parallax.relativeFogRadius = relativeFogRadius_UI;
			currData->shaderData.parallax.scopeSwayAmount = scopeSwayAmount_UI;
			currData->shaderData.parallax.maxTravel = maxTravel_UI;
			currData->shaderData.bBoltDisable = bDisableWhileBolt;
			currData->shaderData.nvIntensity = nvIntensity_UI;
			currData->shaderData.fovAdjust = fovBase_UI;

			currData->shaderData.rectSize[0] = Size_rect_UI[0];
			currData->shaderData.rectSize[1] = Size_rect_UI[1];
			currData->shaderData.rectSize[2] = Size_rect_UI[2];
			currData->shaderData.rectSize[3] = Size_rect_UI[3];

			// The override editor works on Imgui_ZDO directly; the live weapon
			// only mirrors it as a preview, so save the editor values.
			currData->zoomDataOverwrite = Imgui_ZDO;

			currData->additionalKeywords = additionalKeywords;

			sdh->SetCurrentFTSData(currData);
			sdh->WriteCurrentFTSData();

			// Re-select immediately so the saved override becomes the live
			// persisted state, then repopulate every editor field from that
			// state. The old ordering called ResetUIData before setting this
			// flag, so the refresh was silently skipped.
			d3d->bRefreshChar = true;
			bhasSaveZoomData = false;
			ResetUIData(this);

			bIsSaving = false;
		}
	}

	void ImGuiImplClass::MapScopeShaderEffect()
	{
		Hook::D3D::bLegacyMode = bLegacyMode;

		scopeData.ScopeEffect_Offset = { LF(PositionOffset_UI[0]), LF(PositionOffset_UI[1]) };

		scopeData.ScopeEffect_OriPositionOffset = {  LF(OriPositionOffset_UI[0]),  LF(OriPositionOffset_UI[1]) };
		scopeData.ScopeEffect_Size = { LF(Size_UI[0]), LF(Size_UI[1]) };
		scopeData.ScopeEffect_OriSize = { OriSize_UI[0], OriSize_UI[1] };

		scopeData.ReticleSize = ReticleSize_UI;
		scopeData.reticle_Offset = { reticle_Offset[0] / 1000.0F, reticle_Offset[1] / 1000.0F };

		scopeData.parallax_Radius = radius_UI;
		scopeData.parallax_relativeFogRadius = relativeFogRadius_UI;
		scopeData.parallax_scopeSwayAmount = scopeSwayAmount_UI;
		scopeData.parallax_maxTravel = maxTravel_UI;

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
			"Turn off only for weapons patched with an FTS lens material.\n"
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

		if (ImGui::TreeNode("Additional Keywords"))
		{
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

		// The sliders edit the profile's override values (Imgui_ZDO), not the
		// live weapon directly. The edited values are pushed onto the weapon
		// every frame as a preview; the snapshot taken on enable restores the
		// weapon when the override is unchecked or the menu closes unsaved.
		const bool overrideToggled =
			ImGui::Checkbox("Override Sighted Zoom and Camera", &Imgui_ZDO.enableZoomDateOverwrite);
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
		if (!overridesAllowed)
		{
			ImGui::TextDisabled(
				"Override preview is disabled by the current verification stage.");
		}
		else if (Imgui_ZDO.enableZoomDateOverwrite)
		{
			if (Imgui_InstanceData && Imgui_InstanceData->zoomData)
			{
				auto& liveZoom = Imgui_InstanceData->zoomData->zoomData;

				if (!bhasSaveZoomData)
				{
					currOriZoomData = liveZoom;
					bhasSaveZoomData = true;
				}

				if (overrideToggled)
				{
					// Just switched on: start editing from the weapon's own
					// values so nothing jumps.
					Imgui_ZDO.fovMul = liveZoom.fovMult;
					Imgui_ZDO.x = liveZoom.cameraOffset.x;
					Imgui_ZDO.y = liveZoom.cameraOffset.y;
					Imgui_ZDO.z = liveZoom.cameraOffset.z;
				}

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
				if (ImGui::DragFloat("Scope Magnification", &minZoom_UI, 0.01F, 1.0F, 15.0F, "%.2fx")) {
					d3d->SetZoom(minZoom_UI);
				}
				Tip("Magnification inside the scope circle. Raise it to compensate\n"
					"when you lower the FOV multiplier above, so the scope keeps its\n"
					"optical zoom while the rest of the screen stays wide.");

				// Live preview: push the edited values onto the weapon.
				liveZoom.fovMult = Imgui_ZDO.fovMul;
				if (cameraOverrideAllowed) {
					liveZoom.cameraOffset = {
						Imgui_ZDO.x,
						Imgui_ZDO.y,
						Imgui_ZDO.z
					};
				}
			}
			else
			{
				ImGui::TextWrapped("The equipped weapon has no zoom data to override.");
			}
		}
		else if (bhasSaveZoomData)
		{
			// Just switched off: put the weapon back the way it was.
			if (Imgui_InstanceData && Imgui_InstanceData->zoomData)
			{
				RestoreOwnedZoomFields(
					Imgui_InstanceData->zoomData->zoomData,
					currOriZoomData);
			}
			bhasSaveZoomData = false;
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

			if (ImGui::DragFloat("Magnification", &minZoom_UI, 0.01F, 1.0F, 15.0F, "%.2fx")) {
				d3d->SetZoom(minZoom_UI);
			}
			Tip("Magnification inside the scope when you start aiming. For automatic\n"
				"STS scopes this stacks on top of the scope's own zoom.\n"
				"Previews live while aiming in edit mode.");
			ImGui::DragFloat("Max Scroll Magnification", &maxZoom_UI, 0.01F, 1.0F, 15.0F, "%.2fx");
			Tip("Upper limit for the mouse wheel zoom while aiming.");

			ImGui::Spacing();

			ImGui::DragFloat("Fish Eye Strength", &fishEyeStrength_UI, 0.01F, 0.0F, 2.0F, "%.2f");
			Tip("Bends the image toward the edge of the lens like a wide-angle camera,\n"
				"similar to the scopes in Modern Warfare 2019. 0 turns it off.");
			ImGui::DragFloat("Fish Eye Curve", &fishEyePower_UI, 0.01F, 0.5F, 6.0F, "%.2f");
			Tip("How sharply the bend ramps up toward the edge of the lens.\n"
				"Higher values keep the center flat and push the distortion\n"
				"out to the rim.");

			ImGui::Spacing();

			if (bLegacyMode)
				ImGui::DragFloat2("Circle Position", PositionOffset_UI, 0.1F, -3840, 3840);
			else
				ImGui::DragFloat2("Circle Position", PositionOffset_UI, 0.1F, -1000, 1000, "%.2f");
			Tip("Moves the magnified area on screen, in 1080p reference pixels from\n"
				"the scope's center. Use it to line the circle up with the lens.");

			if (bLegacyMode)
			{
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
			Tip("Scales the reticle texture drawn inside the magnified area.\n"
				"Has no effect when the profile ships without a reticle texture.");
			ImGui::DragFloat2("Reticle Offset", reticle_Offset, 0.01F, -1000.0F, 1000.0F);
			Tip("Moves the reticle texture inside the magnified area.");
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
				"starts to fog out. Larger values are more forgiving.");
			ImGui::DragFloat("Vignette Reach", &relativeFogRadius_UI, 0.01F, 0, 20);
			Tip("How far the dark vignette reaches into the image from the edge.\n"
				"Higher values darken the scope edge sooner.");
			ImGui::DragFloat("Vignette Sharpness", &scopeSwayAmount_UI, 0.01F, 0, 20);
			Tip("How abruptly the image transitions into the dark edge.\n"
				"Higher values give a harder edge.");
			ImGui::DragFloat("Maximum Brightness", &maxTravel_UI, 0.01F, 0, 20);
			Tip("Brightness cap for the magnified image. 1.0 shows the scene at full\n"
				"brightness; lower values tint the whole scope darker.");
		}
	}

	ImGuiImplClass::ImGuiImplClass()
	{
		if (!sdh)
			sdh = ScopeData::ScopeDataHandler::GetSingleton();

		d3d = Hook::D3D::GetSington();
		currData = sdh->GetCurrentFTSData();
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
				"No scope profile is active. Equip a weapon with an FTS profile or an "
				"STS-configured scope, then reopen this page.");
			ImGui::PopItemWidth();
			return;
		}

		if (currData->autoProfile) {
			ImGui::TextUnformatted("Profile: automatic (detected STS scope)");
			ImGui::TextDisabled("%s", currData->keywordName.c_str());
			ImGui::TextWrapped(
				"This scope works without a patch. Saving below creates an editable "
				"profile under Data/F4SE/Plugins/FTS/Auto.");
		} else {
			ImGui::TextUnformatted("Profile: from file");
			ImGui::TextDisabled("%s", currData->path.c_str());
		}

		ImGui::Spacing();

		if (ImGui::Checkbox("Edit Mode", &Hook::D3D::bEnableEditMode) &&
			!Hook::D3D::bEnableEditMode) {
			// Leaving edit mode without saving: revert the zoom preview and
			// reload the on-screen effect from the stored profile values.
			if (bhasSaveZoomData && Imgui_InstanceData && Imgui_InstanceData->zoomData) {
				RestoreOwnedZoomFields(
					Imgui_InstanceData->zoomData->zoomData,
					instance->currOriZoomData);
				bhasSaveZoomData = false;
			}
			if (d3d) {
				d3d->bRefreshChar = true;
				ResetUIData(instance);
				if (currData) {
					instance->MapScopeShaderEffect();
				}
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

		if (!Hook::D3D::bEnableEditMode) {
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

		ImGui::PopItemWidth();
	}

	namespace
	{
		MENU_WINDOW scopeEditorWindow = nullptr;
		F4SEMenuFramework::Model::HudElement* scopeVisualProbe = nullptr;

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
			Hook::D3D::bEnableEditMode = false;
			if (bHoldAim) {
				SetHoldAim(false);
			}
			auto* instance = ImGuiImplClass::GetSington();
			if (bhasSaveZoomData && Imgui_InstanceData && Imgui_InstanceData->zoomData) {
				RestoreOwnedZoomFields(
					Imgui_InstanceData->zoomData->zoomData,
					instance->currOriZoomData);
				bhasSaveZoomData = false;
			}
			if (d3d) {
				d3d->bRefreshChar = true;
				ResetUIData(instance);
				if (currData) {
					instance->MapScopeShaderEffect();
				}
			}
		}

		void __stdcall OnFrameworkEvent(F4SEMenuFramework::Events::Type type)
		{
			if (type == F4SEMenuFramework::Events::kBeforeRender) {
				Hook::D3D::GetSington()->RenderFromFramework();
			} else if (type == F4SEMenuFramework::Events::kCloseMenu) {
				// Hold Aim blocks the game's keyboard/mouse processing while
				// it is active. Whatever else happens on menu close, that
				// block must lift now; with the panel gone there is no
				// interactive UI left to turn it off.
				if (bHoldAim) {
					SetHoldAim(false);
				}
				// The editor popout is a non-pausing window that survives the
				// Mod Control Panel, so dismissing the panel while the popout
				// is open keeps the edit session and its live preview alive.
				if (scopeEditorWindow && scopeEditorWindow->IsOpen.load()) {
					return;
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
			scopeEditorWindow->IsOpen.store(popoutOpen);
		}
		Tip("Moves these controls into a movable, resizable window that stays on\n"
			"screen after this menu closes, so you can watch changes while aiming.\n"
			"Reopen this menu (same hotkey) when you need the mouse to adjust it.");
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
			ImGui::SetNextWindowPos(
				{ viewport->Pos.x + viewport->Size.x * 0.5F, viewport->Pos.y + viewport->Size.y * 0.5F },
				ImGui::ImGuiCond_Appearing,
				{ 0.5F, 0.5F });
			ImGui::SetNextWindowSize(
				{ viewport->Size.x * 0.65F, viewport->Size.y * 0.75F },
				ImGui::ImGuiCond_Appearing);
		}

		if (ImGui::Begin("Scope Customization##MagnaScope", nullptr, ImGui::ImGuiWindowFlags_NoCollapse)) {
			if (ImGui::Button("Close") && scopeEditorWindow) {
				scopeEditorWindow->IsOpen.store(false);
				// With the Mod Control Panel also closed there is nothing
				// left to edit from, so end the session and revert any
				// unsaved preview.
				if (!F4SEMenuFramework::IsAnyBlockingWindowOpened()) {
					EndEditSession();
				}
			}
			ImGui::Separator();
			ImGuiImplClass::GetSington()->RenderImgui();
		}
		ImGui::End();
	}
}

