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
		// 0 bilinear / 1 Catmull-Rom bicubic / 2 Lanczos-2. Structural, so
		// the variant resolver restores it from the base profile rather than
		// interpolating it.
		int magnificationFilter = 0;
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
		float lensScale = 1.54F;
		// Six related values, carried as a struct rather than six more
		// positional parameters on an already long publish call.
		ScopeData::Breathing breathing;
		// Pinned aperture shape name, empty for automatic. A string rather
		// than an index because the candidate list changes with the weapon.
		std::string apertureSurface;
		// Pinned aiming-mark shape name, empty for automatic. Same reasoning.
		std::string reticleSurface;
		// Scales the aperture activation. The secondary-sight blend fades the
		// whole optical composite out through the constant the scene replay and
		// the reticle layer already share, so the two cannot desynchronise.
		float apertureActivationScale = 1.0F;
		// Live secondary-sight eye shift, in ZoomData offset space and already
		// blend-scaled. The engine samples cameraOffset only at aim-in, so the
		// game thread converts this through the Camera node's frame and shifts
		// the first-person weapon each frame instead. Zero when no sight is up.
		float sightShiftX = 0.0F;
		float sightShiftY = 0.0F;
		float sightShiftZ = 0.0F;
		// Custom reticle: index into the discovered list, -1 for the authored
		// mesh, and the size of a texture reticle in aperture radii.
		int customReticleIndex = -1;
		float customReticleScale = 1.0F;
		bool active = false;

		// One clamp path for every producer.
		void Clamp();

		// Builds an unclamped-then-clamped snapshot straight from profile data.
		// This is what lets the variant resolver publish on the same channel the
		// editor uses without restating every range.
		[[nodiscard]] static EditorPreviewSnapshot FromProfile(
			const ScopeData::ShaderData& shaderData,
			const ScopeData::ZoomDataOverwrite& zoomOverride,
			std::uint64_t selectionRevision);
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

	// The live eye-to-lens geometry, measured while aiming. Converting authored
	// zoom onto the lens is a homothety of the camera about the aperture, so it
	// needs the actual vector from the eye to the optic -- there is no way to
	// derive that from BGSZoomData, whose offsets are relative to a default eye
	// position the engine never exposes.
	struct ApertureGeometrySnapshot
	{
		// The values BGSZoomData::cameraOffset should be *set* to in order to
		// put the aim reference on the view axis. Not a delta -- cameraOffset
		// is the camera-space vector from the first-person Camera node to the
		// point that should sit on the axis, which is why these are assigned
		// rather than accumulated.
		//
		// Derived the way SightHelper does it, that plugin being a working
		// implementation of exactly this alignment:
		//   diff = (aim->world.translate - camera->world.translate) / scale
		//   diff = camera->world.rotate * diff
		//   cameraOffset.x = diff.x;  cameraOffset.z = diff.y;
		// Camera-space X maps to offset X and camera-space Y maps to offset Z,
		// both positive. Deriving the signs from the projection's own
		// ndcX = -cameraPoint.x convention gives the opposite answer and
		// mis-aligns the sight; the reference wins over the inference.
		float offsetFrameX = 0.0F;
		float offsetFrameZ = 0.0F;
		// Forward depth to the aim reference. Reported only -- forward offset
		// plays no part in alignment.
		float offsetFrameY = 0.0F;
		// Straight-line eye-to-aim distance, for reporting.
		float distance = 0.0F;
		// Forward distance from the eye to the aperture plane. The binding
		// constraint on travel is the eye versus the glass, not versus the
		// reticle: on a long optic the reticle sits well beyond the lens, so a
		// target distance that looks reasonable measured to the reticle can put
		// the eye through the objective.
		float apertureForwardDistance = 0.0F;
		// Projected aperture radius in pixels, the quantity the conversion is
		// supposed to preserve and the one worth reporting afterwards.
		float projectedRadiusPixels = 0.0F;
		// Which weapon/attachment selection this was measured against. A
		// measurement from the previously equipped scope is worse than none:
		// it would let the conversion run against another optic's geometry and
		// silently produce a plausible-looking wrong answer.
		std::uint64_t selectionRevision = 0;
		bool available = false;
	};

	enum class ProfileRequest : std::uint8_t
	{
		kNone,
		kReselect,
		kReload,
		// Removes this scope's preset file and drops the cached in-memory
		// profile, so the next time the scope is selected it is synthesized
		// from defaults again.
		kDeletePreset,
		// Re-walks the reticle folders. Explicit rather than automatic: a
		// recursive directory read on every weapon swap, under MO2's virtual
		// file system, is not a bounded operation, and users add reticle files
		// between sessions rather than mid-firefight.
		kRescanReticles
	};

	// Reticle texture file names available to the selected scope, discovered on
	// the game thread and copied here for the editor and the hotkey cycler.
	void PublishDiscoveredReticles(const std::vector<std::string>& reticles);
	// Which variant the player is currently looking through, published by the
	// game thread. The editor reads this instead of the session map, which the
	// game thread owns and inserts into. -1 when no variant set is active.
	void PublishActiveVariantIndex(int index);
	[[nodiscard]] int ActiveVariantIndexForEditor();
	[[nodiscard]] std::vector<std::string> GetDiscoveredReticles();

	// Live occlusion preview: while the editor is open, the game thread builds
	// the sphere cull from these unsaved values instead of the profile's saved
	// ones, so the sphere can be tuned against what is on the sliders. The
	// editor publishes on every occlusion edit; closing the editor clears it.
	void PublishOcclusionPreview(
		const ScopeData::OcclusionSettings& settings, bool active);
	// False when no preview is active (editor closed or section untouched).
	[[nodiscard]] bool GetOcclusionPreview(ScopeData::OcclusionSettings& out);
	// Shape names under the scope's own subtree, published by the game thread
	// for the editor's exclude checklist. Optical surfaces are pre-filtered.
	void PublishOcclusionShapes(const std::vector<std::string>& names);
	[[nodiscard]] std::vector<std::string> GetOcclusionShapes();

	// True while the editor's Show Sphere checkbox is on. The sphere itself
	// is real translucent geometry drawn at the composite anchor
	// (Hook::D3D::DrawOcclusionSphereGeo); the game thread reads this to
	// decide whether to publish the matrices each tick.
	[[nodiscard]] bool OcclusionSphereGeoWanted();

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
	void PublishApertureGeometry(const ApertureGeometrySnapshot& geometry);
	[[nodiscard]] ApertureGeometrySnapshot GetApertureGeometry();
	void PublishEditorPreview(const EditorPreviewSnapshot& snapshot);
	[[nodiscard]] EditorPreviewSnapshot GetEditorPreviewSnapshot();
	void ClearEditorPreview();
	void RequestProfileAction(ProfileRequest request);
	[[nodiscard]] ProfileRequest ConsumeProfileAction();
	// writeToDisk false applies the snapshot to the in-memory profile only.
	// That is what makes editor changes outlive the edit session without
	// creating a preset file.
	void RequestProfileSave(
		const ScopeData::ScopeProfile& profile,
		bool writeToDisk = true);
	[[nodiscard]] std::unique_ptr<ScopeData::ScopeProfile> ConsumeProfileSave(
		bool& writeToDisk);

	class ImGuiImplClass
	{
	public:
		ImGuiImplClass();
		~ImGuiImplClass() {};
		static ImGuiImplClass* GetSington();

	public:
		std::atomic_bool bIsSaving{ false };

		// Snapshot of every editor value, laid over the supplied profile.
		// Shared by Save Profile and by the session-only apply that ends an
		// edit session, so the two cannot capture different sets of values.
		[[nodiscard]] ScopeData::ScopeProfile BuildEditedProfile(
			const ScopeData::ScopeProfile& base);

		// Just the shader half of the above, for capturing the sliders into a
		// variant. Implemented in terms of BuildEditedProfile so there is one
		// place that knows how a slider maps to a field.
		[[nodiscard]] ScopeData::ShaderData BuildEditedShaderData(
			const ScopeData::ShaderData& base);

		// Captures the sliders into whichever variant is being edited, so
		// switching variants or adding one does not discard the current values.
		void StoreUIIntoEditedVariant();

		ScopeData::ZoomDataOverwrite Imgui_ZDO;
		ScopeData::ZoomDataOverwrite ori_ZDO;

		bool bLegacyMode = true;
		bool UsingSTS_UI = false;
		int scopeFrame_UI = 1;
		float fovBase_UI = 0.0F;
		bool IsCircle_UI = true;
		float camDepth_UI = 1.0F;
		float ReticleSize_UI = 4.0F;
		float reticle_Offset[2] = { 0.0F, 0.0F };
		float minZoom_UI = 1.0F;
		float maxZoom_UI = 4.0F;
		float PositionOffset_UI[2] = { 0.0F, 0.0F };
		float OriPositionOffset_UI[2] = { 0.0F, 0.0F };
		float Size_UI[2] = { 200.0F, 0.0F };
		float Size_rect_UI[4] = { 235.0F, 200.0F, 775.0F, 760.0F };
		float OriSize_UI[2] = { 200.0F, 0.0F };
		float fishEyeStrength_UI = 0.0F;
		float fishEyePower_UI = 2.0F;
		float edgeRefractionStrength_UI = 0.0F;
		float edgeRefractionWidth_UI = 0.15F;
		float edgeChromaticAberration_UI = 0.0F;
		float imageDenoise_UI = 0.0F;
		float imageSharpen_UI = 0.0F;
		int magnificationFilter_UI = 0;
		float reticleMagnification_UI = 1.0F;
		float reticleShadowStrength_UI = 0.0F;
		float reticleParallaxStrength_UI = 1.0F;
		float radius_UI = 2.0F;
		float relativeFogRadius_UI = 9.0F;
		float scopeSwayAmount_UI = 3.0F;
		float maxTravel_UI = 4.0F;
		float sceneParallaxStrength_UI = 0.0F;
		float opticalLagStrength_UI = 1.0F;
		float sceneDepth_UI = 1.0F;
		float shadowDepth_UI = 1.0F;
		float imageStillness_UI = 0.0F;
		float axialBreathing_UI = 0.0F;
		float recenterSpeed_UI = 1.0F;
		float strafeLag_UI = 1.0F;
		std::string apertureSurface_UI;
		std::string reticleSurface_UI;
		float tubeDepth_UI = 0.0F;
		float lensOffset_UI[2] = { 0.0F, 0.0F };
		float lensScale_UI = 1.54F;
		float breathRate_UI = 0.25F;
		float breathSway_UI = 0.0F;
		float breathDrift_UI = 0.0F;
		float breathFigure_UI = 0.25F;
		float breathHold_UI = 0.0F;
		float breathPupilFollow_UI = 1.0F;
		std::uint64_t selectionRevision_UI = 0;

		// --- variants, secondary sights, reticles -------------------------
		// Editing copies rather than the live profile: the profile is owned by
		// the game thread and this runs on the renderer thread.
		bool variantsEnabled_UI = false;
		bool variantsContinuous_UI = false;
		float variantStepSeconds_UI = 0.12F;
		std::vector<ScopeData::ProfileVariant> variants_UI;
		std::uint32_t variantsNextId_UI = 1;
		std::uint32_t variantDefaultId_UI = 0;
		// Which variant the sliders are currently editing. Zero means the base
		// profile, which is also what a disabled variant set edits.
		//
		// This is why the editor is a composition with the resolver rather than
		// an override of it: without it, every variant after the first would be
		// authored while looking through variant zero.
		std::uint32_t editedVariantId_UI = 0;
		std::vector<ScopeData::SecondarySight> secondarySights_UI;
		int editedSecondarySight_UI = 0;
		// Live-preview the selected secondary sight's zoom data on the weapon so
		// it can be aligned with the same controls as the primary optic. Off by
		// default: it moves the camera to the secondary sight while the editor is
		// open, which is not what someone tuning the main optic wants.
		bool showSecondarySightZoom_UI = false;
		std::string defaultReticleFile_UI;
		float customReticleScale_UI = 1.0F;
		int opticsKeyIndex_UI = 0;
		// Sphere occlusion of the scope's own front geometry; edited whole and
		// published as a live preview while the editor is open.
		ScopeData::OcclusionSettings occlusion_UI;
		// One-shot: the section publishes the loaded values on its first
		// render so the preview channel never serves a stale profile.
		bool occlusionPreviewPublished_UI = false;
		// Draws the cull sphere's outline over the game while tuning it.
		// Session-only editor aid, deliberately not part of ScopeProfile; both
		// the checkbox and the overlay run on the framework's render thread.
		// Atomic to match alignmentCrosshair_UI: the editor window and the HUD
		// overlay are separate Menu Framework callbacks.
		std::atomic_bool occlusionShowSphere_UI{ false };

		// True once ResetUIData has copied the selected profile into the _UI
		// members above. They have no constructor, so before that they hold
		// whatever was in the allocation, and anything that writes them back
		// into a profile has to check this first.
		bool uiValuesLoaded = false;

		// Draws a full-screen crosshair on the exact centre of the viewport,
		// which is where a shot lands. The optic's own reticle is the point of
		// aim, and the two agree only once the camera offsets are right, so
		// this is the reference the offsets are dialled against.
		//
		// Deliberately not part of ScopeProfile: it is an alignment aid for
		// whoever is authoring a profile, not a property of the scope, and
		// persisting it would leave it drawn over somebody's game.
		//
		// Atomic because the HUD callback and the editor window are separate
		// Menu Framework callbacks.
		std::atomic_bool alignmentCrosshair_UI{ false };

		bool bEnableFG;
		bool bEnableZMove;
		bool bEnableNVGEffect;
		float nvIntensity_UI;
		bool bEnableThermalEffect;
		bool bDefaultNVEffect;
		bool bDefaultThermalEffect;
		float nvNoise_UI;
		float nvBloom_UI;
		int nvTint_UI;
		int thermalPalette_UI;
		float thermalContrast_UI;
		float thermalEdge_UI;
		float baseWeaponPos_UI;
		float MovePercentage_UI;

		bool bDisableWhileBolt = false;

		void MapScopeShaderEffect();

	public:
		void RenderImgui();
		bool CheckAndInit();
		void ReloadData();
		void SaveData();
		void DeletePresetData();
		void MainMenuSection();
		void ShaderDataSection();
		void ParallaxDataSection();
		void VariantSection();
		void SecondarySightSection();
		void ReticleSection();
		void OcclusionSection();

		void UpdateWeaponInstance(RE::TESObjectWEAP::InstanceData*);
		void UpdateImGuiData();
	};

}
