#pragma once

#pragma warning(push)
#pragma warning(disable: 4099)
#include "../../F4SE-Menu-Framework-3/resources/F4SEMenuFramework.h"
#pragma warning(pop)
#include "ScopeProfile.h"

#include <atomic>
#include <cstdint>

// The consumer header exposes the framework-owned ImGui API through
// ImGuiMCP. This alias keeps the customization code readable without linking
// another Dear ImGui context into MagnaScope.
namespace ImGui = ImGuiMCP;

const char* const mainKey[] = {
	"None",
	"Unknown",
	"VK_LBUTTON",
	"VK_RBUTTON",
	"VK_CANCEL",
	"VK_MBUTTON",
	"VK_XBUTTON1",
	"VK_XBUTTON2",
	"Unknown",
	"VK_BACK",
	"VK_TAB",
	"Unknown",
	"Unknown",
	"VK_CLEAR",
	"VK_RETURN",
	"Unknown",
	"Unknown",
	"VK_SHIFT",
	"VK_CONTROL",
	"VK_MENU",
	"VK_PAUSE",
	"VK_CAPITAL",
	"VK_KANA",
	"Unknown",
	"VK_JUNJA",
	"VK_FINAL",
	"VK_KANJI",
	"Unknown",
	"VK_ESCAPE",
	"VK_CONVERT",
	"VK_NONCONVERT",
	"VK_ACCEPT",
	"VK_MODECHANGE",
	"VK_SPACE",
	"VK_PRIOR",
	"VK_NEXT",
	"VK_END",
	"VK_HOME",
	"VK_LEFT",
	"VK_UP",
	"VK_RIGHT",
	"VK_DOWN",
	"VK_SELECT",
	"VK_PRINT",
	"VK_EXECUTE",
	"VK_SNAPSHOT",
	"VK_INSERT",
	"VK_DELETE",
	"VK_HELP",
	"0",
	"1",
	"2",
	"3",
	"4",
	"5",
	"6",
	"7",
	"8",
	"9",
	"Unknown",
	"Unknown",
	"Unknown",
	"Unknown",
	"Unknown",
	"Unknown",
	"Unknown",
	"A",
	"B",
	"C",
	"D",
	"E",
	"F",
	"G",
	"H",
	"I",
	"J",
	"K",
	"L",
	"M",
	"N",
	"O",
	"P",
	"Q",
	"R",
	"S",
	"T",
	"U",
	"V",
	"W",
	"X",
	"Y",
	"Z",
	"VK_LWIN",
	"VK_RWIN",
	"VK_APPS",
	"Unknown",
	"VK_SLEEP",
	"VK_NUMPAD0",
	"VK_NUMPAD1",
	"VK_NUMPAD2",
	"VK_NUMPAD3",
	"VK_NUMPAD4",
	"VK_NUMPAD5",
	"VK_NUMPAD6",
	"VK_NUMPAD7",
	"VK_NUMPAD8",
	"VK_NUMPAD9",
	"VK_MULTIPLY",
	"VK_ADD",
	"VK_SEPARATOR",
	"VK_SUBTRACT",
	"VK_DECIMAL",
	"VK_DIVIDE",
	"VK_F1",
	"VK_F2",
	"VK_F3",
	"VK_F4",
	"VK_F5",
	"VK_F6",
	"VK_F7",
	"VK_F8",
	"VK_F9",
	"VK_F10",
	"VK_F11",
	"VK_F12",
	"VK_F13",
	"VK_F14",
	"VK_F15",
	"VK_F16",
	"VK_F17",
	"VK_F18",
	"VK_F19",
	"VK_F20",
	"VK_F21",
	"VK_F22",
	"VK_F23",
	"VK_F24",
	"Unknown",
	"Unknown",
	"Unknown",
	"Unknown",
	"Unknown",
	"Unknown",
	"Unknown",
	"Unknown",
	"VK_NUMLOCK",
	"VK_SCROLL",
	"VK_OEM_NEC_EQUAL",
	"VK_OEM_FJ_MASSHOU",
	"VK_OEM_FJ_TOUROKU",
	"VK_OEM_FJ_LOYA",
	"VK_OEM_FJ_ROYA",
	"Unknown",
	"Unknown",
	"Unknown",
	"Unknown",
	"Unknown",
	"Unknown",
	"Unknown",
	"Unknown",
	"Unknown",
	"VK_LSHIFT",
	"VK_RSHIFT",
	"VK_LCONTROL",
	"VK_RCONTROL",
	"VK_LMENU",
	"VK_RMENU"
};

namespace ImGuiImpl
{
	// Menu Framework invokes draw callbacks on the renderer thread. Only
	// copied plain data crosses this bridge; live Fallout objects remain
	// exclusively owned by the game-thread update hook.
	struct EditorPreviewSnapshot
	{
		ScopeData::ZoomDataOverwrite zoomOverride{};
		std::uint64_t selectionRevision = 0;
		float magnification = 1.0F;
		float imageDenoise = 0.0F;
		float imageSharpen = 0.0F;
		float fishEyeStrength = 0.0F;
		float fishEyePower = 2.0F;
		float edgeRefractionStrength = 0.0F;
		float edgeRefractionWidth = 0.15F;
		float edgeChromaticAberration = 0.0F;
		float reticleMagnification = 1.0F;
		float reticleSize = 4.0F;
		float reticleOffsetX = 0.0F;
		float reticleOffsetY = 0.0F;
		float reticleShadowStrength = 0.0F;
		float reticleParallaxStrength = 1.0F;
		float eyeBoxRadius = 2.0F;
		float vignetteReach = 9.0F;
		float vignetteSharpness = 3.0F;
		float eyeBoxMaxTravel = 4.0F;
		float sceneParallaxStrength = 0.0F;
		float opticalLagStrength = 1.0F;
		float sceneDepth = 1.0F;
		float shadowDepth = 1.0F;
		float imageStillness = 0.0F;
		float axialBreathing = 0.0F;
		float recenterSpeed = 1.0F;
		float strafeLag = 1.0F;
		float tubeDepth = 0.0F;
		float lensOffsetX = 0.0F;
		float lensOffsetY = 0.0F;
		float lensScale = 1.0F;
		// Six related values, carried as a struct rather than six more
		// positional parameters on an already long publish call.
		ScopeData::Breathing breathing;
		// Pinned aperture shape name, empty for automatic. A string rather
		// than an index because the candidate list changes with the weapon.
		std::string apertureSurface;
		bool active = false;
	};

	// One aperture shape the equipped scope offers, discovered on the game
	// thread and copied here for the dropdown. Names carry arbitrary authored
	// suffixes, so the exact string is what gets pinned and saved.
	struct ApertureCandidateInfo
	{
		std::string name;
		// A 48-vertex ScopeFade annulus, the only topology the exact geometry
		// replay can drive. Shown in the list so the choice is informed.
		bool annulus{ false };
	};

	void PublishApertureCandidates(
		const std::vector<ApertureCandidateInfo>& candidates);
	[[nodiscard]] std::vector<ApertureCandidateInfo> GetApertureCandidates();

	struct AuthoredZoomSnapshot
	{
		ScopeData::ZoomDataOverwrite values{};
		std::uint64_t selectionRevision = 0;
		bool available = false;
	};

	enum class ProfileRequest : std::uint8_t
	{
		kNone,
		kReselect,
		kReload
	};

	bool RegisterMenu();
	void __stdcall RenderMenu();
	void __stdcall RenderPopout();
	// Drops the zoom preview snapshot without writing anything back to the
	// weapon. For when the equipped instance was replaced mid-edit and the
	// old instance may already be freed.
	void AbandonZoomPreview();

	// Forced-aim request from the menu (which renders on the D3D thread) to
	// the game thread: 1 = start aiming, 0 = stop, -1 = nothing pending.
	// HookedUpdate consumes it and drives the sighted state and idles.
	extern std::atomic<int> pendingForcedAim;
	void PublishAuthoredZoomSnapshot(
		const ScopeData::ZoomDataOverwrite* authoredValues,
		std::uint64_t selectionRevision);
	[[nodiscard]] AuthoredZoomSnapshot GetAuthoredZoomSnapshot();
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
		float strafeLag,
		float tubeDepth,
		float lensOffsetX,
		float lensOffsetY,
		float lensScale,
		const ScopeData::Breathing& breathing,
		const std::string& apertureSurface);
	[[nodiscard]] EditorPreviewSnapshot GetEditorPreviewSnapshot();
	void ClearEditorPreview();
	void RequestProfileAction(ProfileRequest request);
	[[nodiscard]] ProfileRequest ConsumeProfileAction();
	void RequestProfileSave(const ScopeData::ScopeProfile& profile);
	[[nodiscard]] std::unique_ptr<ScopeData::ScopeProfile> ConsumeProfileSave();

	class ImGuiImplClass
	{
	public:
		ImGuiImplClass();
		~ImGuiImplClass() {};
		static ImGuiImplClass* GetSington();

	public:
		std::atomic_bool bIsSaving{ false };

		ScopeData::ZoomDataOverwrite Imgui_ZDO;
		ScopeData::ZoomDataOverwrite ori_ZDO;

		bool bLegacyMode;
		bool UsingSTS_UI;
		int scopeFrame_UI;
		float fovBase_UI;
		bool IsCircle_UI;
		float camDepth_UI;
		float ReticleSize_UI;
		float reticle_Offset[2];
		float minZoom_UI;
		float maxZoom_UI;
		float PositionOffset_UI[2];
		float OriPositionOffset_UI[2];
		float Size_UI[2];
		float Size_rect_UI[4];
		float OriSize_UI[2];
		float fishEyeStrength_UI;
		float fishEyePower_UI;
		float edgeRefractionStrength_UI;
		float edgeRefractionWidth_UI;
		float edgeChromaticAberration_UI;
		float imageDenoise_UI;
		float imageSharpen_UI;
		float reticleMagnification_UI = 1.0F;
		float reticleShadowStrength_UI = 0.0F;
		float reticleParallaxStrength_UI = 1.0F;
		float radius_UI;
		float relativeFogRadius_UI;
		float scopeSwayAmount_UI;
		float maxTravel_UI;
		float sceneParallaxStrength_UI;
		float opticalLagStrength_UI = 1.0F;
		float sceneDepth_UI = 1.0F;
		float shadowDepth_UI = 1.0F;
		float imageStillness_UI = 0.0F;
		float axialBreathing_UI = 0.0F;
		float recenterSpeed_UI = 1.0F;
		float strafeLag_UI = 1.0F;
		std::string apertureSurface_UI;
		float tubeDepth_UI = 0.0F;
		float lensOffset_UI[2] = { 0.0F, 0.0F };
		float lensScale_UI = 1.0F;
		float breathRate_UI = 0.25F;
		float breathSway_UI = 0.0F;
		float breathDrift_UI = 0.0F;
		float breathFigure_UI = 0.25F;
		float breathHold_UI = 0.0F;
		float breathPupilFollow_UI = 1.0F;
		std::uint64_t selectionRevision_UI = 0;

		bool bEnableFG;
		bool bEnableZMove;
		bool bEnableNVGEffect;
		float nvIntensity_UI;
		float baseWeaponPos_UI;
		float MovePercentage_UI;

		int nvgComboKeyIndex = 0;
		int nvgMainKeyIndex = 0;
		bool bDisableWhileBolt = false;

		void MapScopeShaderEffect();

	public:
		void RenderImgui();
		bool CheckAndInit();
		void ReloadData();
		void SaveData();
		void MainMenuSection();
		void ShaderDataSection();
		void ParallaxDataSection();

		void UpdateWeaponInstance(RE::TESObjectWEAP::InstanceData*);
		void UpdateImGuiData();
	};

}
