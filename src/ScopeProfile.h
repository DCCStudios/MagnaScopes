#pragma once

#include <nlohmann/json.hpp>
#include <tuple>
#include <unordered_set>

using json = nlohmann::json;
namespace ScopeData
{
	std::vector<std::string_view> splitSV(std::string_view strv, std::string_view delims = " ");

	struct CullingData
	{
		CullingData(int a, int b)
		{
			IndexCount = a;
			StrideCount = b;
		}
		int IndexCount;
		int StrideCount;
	};

	struct Parallax
	{
		float radius = 2;
		float relativeFogRadius = 9;
		float scopeSwayAmount = 3;
		float maxTravel = 4;
		// Retired. Scaled a second, delta-space image-lag path that no shader
		// reads any more. Kept so existing profiles round-trip unchanged and
		// the version migrations below still compile; not shown in the editor.
		float sceneDepth = 1.0F;
		// Virtual distance between the authored aperture and the exit-pupil
		// shadow. Values above one exaggerate pupil travel.
		float shadowDepth = 1.0F;
		// Lens Lag: how far the visible opening swings off centre as the camera
		// moves, like an exit pupil sliding across the glass. It scales the
		// exit-pupil displacement only. The magnified image is never delayed or
		// displaced -- an earlier version shifted the sampled region instead,
		// which showed the player a piece of world they were not pointing at
		// and made the whole optic read as sluggish at every setting.
		//
		// Keeps its old JSON key so profiles round-trip.
		float imageStillness = 0.0F;
		// Fore/aft apparent-size breathing, independent of sceneDepth so
		// lateral parallax cannot make the image read as moving closer.
		float axialBreathing = 0.0F;
		// Settling rate multiplier for pupil and image recentering.
		float recenterSpeed = 1.0F;
		// How much player translation drives lens lag, relative to how much
		// turning does. Separate because the two have very different inputs:
		// a pan moves a distant reference point across the screen, a strafe
		// moves the camera a few game units, and one gain cannot suit both.
		float strafeLag = 1.0F;
		// How far the magnified image is recessed toward the front of the
		// tube, leaving a ring of shadow between it and the rear aperture.
		float tubeDepth = 0.0F;
	};

	// Breathing sway applied to the magnified image and the exit pupil.
	//
	// This is deliberately NOT routed through the eye-box travel that carries
	// recoil and weapon inertia. That path is filtered by a recentering
	// follower whose whole job is to pull transient motion back to zero, so a
	// continuous oscillation fed into it would be progressively cancelled: the
	// baseline would learn the sine and the effect would fade out while the
	// slider still read a large value. Breathing is its own term.
	struct Breathing
	{
		// Cycles per second. 0.25 is roughly fifteen breaths a minute.
		float rate = 0.25F;
		// Vertical and horizontal amplitude, in aperture radii of apparent
		// motion. Apparent rather than angular: the shift is divided by
		// magnification so a scope's sway reads the same at 4x and 12x
		// instead of becoming unusable at the top of the range.
		float sway = 0.0F;
		float drift = 0.0F;
		// Phase lead of the horizontal axis over the vertical, in turns.
		// 0 traces a diagonal line, 0.25 an ellipse, and values between them
		// the leaning figure-eight a real hold wanders through.
		float figure = 0.25F;
		// Waveform shape. 0 is a pure sine. Higher values flatten the turning
		// points so the drift dwells at the extremes, which is what the pause
		// at the end of a breath actually looks like through glass.
		float hold = 0.0F;
		// How much of the sway the exit pupil takes. The image and the pupil
		// are at different depths in a real optic, so they need not move
		// together; 0 keeps the shadow perfectly still while the image swims.
		float pupilFollow = 1.0F;
	};

	struct ZoomDataOverwrite
	{
		float x = 0;
		float y = 0;
		float z = 0;
		float fovMul = 1;
		bool enableZoomDateOverwrite = false;
	};

	struct ShaderData
	{
		bool IsCircle = true;
		bool bEnableZMove = false;
		bool bCanEnableNV = false;
		bool bBoltDisable = false;
		// Per-profile permission for the thermal overlay (parallels
		// bCanEnableNV). The live hotkey toggle is bEnableThermal in hooking.
		bool bCanEnableThermal = false;
		// Start each mode ON when a permitting scope is selected, instead of
		// requiring the hotkey each time. Seeded into the live global flags at
		// scope selection; the hotkey still toggles live afterward. Only takes
		// effect when the matching permission (bCanEnable*) is also set.
		bool bDefaultEnableNV = false;
		bool bDefaultEnableThermal = false;

		float nvIntensity = 3;
		// Night-vision realism controls (nvIntensity is the gain). Thermal
		// controls follow. These mirror the appended b5 fields in hooking.h /
		// Triangle.hlsli.
		float nvNoise = 0.15F;         // scintillation grain amount
		float nvBloom = 0.30F;         // bright-source bloom
		int nvTint = 0;                // 0 = green phosphor, 1 = white phosphor
		int thermalPalette = 0;        // 0 white-hot, 1 black-hot, 2 ironbow
		float thermalContrast = 1.0F;  // thermal punch
		float thermalEdge = 0.25F;     // silhouette edge emphasis
		float baseWeaponPos = 0;
		float movePercentage = 0;

		float camDepth = 1.0F;

		float minZoom = 1.0F;
		float maxZoom = 4.0F;

		float PositionOffset[2] = { 0.0F, 0.0F };
		float OriPositionOffset[2] = { 0.0F, 0.0F };

		float Size[2] = { 200.0F, 0.0F };
		float OriSize[2] = { 200.0F, 0.0F };
		float rectSize[4] = { 235.0f, 200.0f, 775.0f, 760.0f };

		float ReticleSize = 4;
		float reticle_Offset[2] = { 0.0f, 0.0f };

		// MW2019-style lens distortion inside the magnified area. Strength 0
		// disables it; power controls how sharply the bend ramps toward the
		// edge of the lens (higher keeps the center flat).
		float fishEyeStrength = 0.0F;
		float fishEyePower = 2.0F;
		// Optional edge-only lens refraction. Strength controls the geometric
		// displacement at the rim; width controls how far the effect reaches
		// toward the optical center.
		float edgeRefractionStrength = 0.0F;
		float edgeRefractionWidth = 0.15F;
		float edgeChromaticAberration = 0.0F;
		// Retired alongside parallax.sceneDepth: the delta-space image-lag
		// path these two scaled is gone. Kept for profile round-tripping and
		// the version migrations below; not shown in the editor.
		float sceneParallaxStrength = 0.0F;
		// Multiplies transient ScopeFade-local eye motion before it drives the
		// exit pupil. It does not change the settled center, so ordinary
		// camera pitch/yaw cannot permanently offset the optic. The magnified
		// image is deliberately not scaled by this: Image Lag alone governs
		// how far the image trails.
		float opticalLagStrength = 1.0F;

		// Optional single-pass cleanup for the automatic STS scene sample.
		// Denoise is an edge-aware spatial blend, not temporal reconstruction;
		// sharpen restores local contrast after magnification. Both default to
		// zero so existing MagnaScope profiles retain their authored image unchanged.
		float imageDenoise = 0.0F;
		float imageSharpen = 0.0F;
		// Reconstruction filter for the magnified sample: 0 = bilinear (the
		// original path, so existing profiles render identically), 1 =
		// Catmull-Rom bicubic, 2 = Lanczos-2. Magnification is an upsample of
		// the already-rendered frame; a negative-lobe kernel preserves edge
		// slopes through that upsample where bilinear's tent kernel turns
		// every source texel into a soft blob. Structural (not per-variant):
		// a filter choice is a property of the optic's rendering, not of a
		// magnification stop, and interpolating between kernels mid-blend is
		// meaningless.
		int magnificationFilter = 0;

		// Independent local scale for the STS-authored 3D reticle. 1 preserves
		// its authored size; this value never inherits scene magnification.
		float reticleMagnification = 1.0F;
		float reticleShadowStrength = 0.0F;
		float reticleParallaxStrength = 1.0F;

		// Placement and size of the sight picture inside the authored housing,
		// for the geometry-replay path. See Through Scopes publishes where its
		// ScopeFade mesh sits, which is not always where a given scope model
		// wants the optical image; the legacy Size/PositionOffset fields above
		// drive the old overlay path only and do nothing here. The offset is in
		// aperture radii along the optic's own X/Z axes so it rolls with the
		// weapon.
		float lensOffset[2] = { 0.0F, 0.0F };
		float lensScale = 1.54F;

		// Which authored shape supplies the aperture, by name.
		//
		// Empty means automatic: the first usable candidate in the order
		// ScopeFade, ScopeViewParts, ScopeAiming. Authored names carry
		// arbitrary numeric suffixes -- ScopeViewParts:378, ScopeAiming:78 --
		// so candidates are matched by prefix and the exact discovered name is
		// stored here once the editor pins one.
		//
		// Only a ScopeFade annulus of 48 vertices and 48 triangles can drive
		// the exact geometry replay: its fill shader derives lens coordinates
		// from primitive order on that specific topology. Any other shape still
		// supplies the aperture's projection, eye box and mask, but the
		// magnified image comes from the screen-space path instead.
		std::string apertureSurface;

		// Which authored shape is the aiming mark, when the editor pins one.
		// Empty means automatic, which only recognises shapes whose name begins
		// with "Reticle" or "Dot". Plenty of weapon-mod meshes name theirs
		// something else entirely -- the MW2019 Grau's 4x optic has no Reticle
		// node at all -- and those cannot be found by any naming rule that is
		// also safe to apply automatically.
		std::string reticleSurface;

		Breathing breathing;

		float fovAdjust = 0;
		Parallax parallax;
	};

	// One magnification stop of a variable-power optic.
	//
	// Only numeric optical values vary per variant. The structural fields that
	// also live in ShaderData -- apertureSurface, reticleSurface, IsCircle and
	// the bool flags -- are owned by the base profile and are overwritten from
	// it after interpolation. They are not lerpable, and changing
	// apertureSurface mid-blend would re-trigger aperture discovery and
	// geometry selection on every frame of the transition.
	//
	// The whole ShaderData is copied rather than a hand-picked lerpable subset,
	// because a subset would need maintaining in lockstep with ShaderData
	// forever and would silently drop any field added later.
	struct ProfileVariant
	{
		// Stable identity, assigned once and never reused within a profile.
		// The co-save stores this rather than a list index: inserting a variant
		// shifts every index, which would silently select the wrong optic on
		// load. Magnification is the sort key and the display value, not the
		// identity -- two variants a rounding error apart are indistinguishable
		// to a user and would collapse unpredictably.
		std::uint32_t id = 0;
		float magnification = 1.0F;
		std::string label;
		ShaderData shaderData;
		ZoomDataOverwrite zoomDataOverwrite;
	};

	struct VariantSet
	{
		// Opt-in. Absent from a profile means disabled, so every existing
		// profile keeps behaving exactly as it does today.
		bool enabled = false;
		// false steps between variants, true blends continuously.
		bool continuous = false;
		// Ease time for a stepped change, so a ratchet does not pop.
		float stepSeconds = 0.12F;
		// Never decreases; guarantees ids stay unique across edits.
		std::uint32_t nextId = 1;
		// Authored starting variant for a character with no co-save entry.
		std::uint32_t defaultVariantId = 0;
		// Sorted ascending by magnification.
		std::vector<ProfileVariant> variants;
	};

	// An offset iron sight or piggyback optic: new zoom data plus a lerp.
	// Deliberately flat -- no magnification variants of its own.
	struct SecondarySight
	{
		std::string name = "Iron Sights";
		ZoomDataOverwrite zoomData;
		// Fade the aperture and magnification out while this sight is up.
		// Offset irons are not magnified.
		bool suppressOptics = true;
		float transitionSeconds = 0.18F;
	};

	// Sphere occlusion: hides the parts of the scope's own meshes that sit
	// inside a glass-anchored sphere (and optionally in front of the glass), so
	// housing geometry cannot clutter the sight picture. Per-triangle -- only
	// the triangles inside the sphere are removed, the rest of each mesh stays
	// -- via an index-buffer substitution at draw time; nothing outside the
	// scope's own NIF is ever touched. One set per profile, disabled by
	// default.
	struct OcclusionSettings
	{
		bool enabled = false;
		// World-unit radius of the cull sphere.
		float sphereRadius = 5.0F;
		// Offset of the sphere centre from the glass centre, in the glass
		// plane's local axes (X = right, Y = optical axis, Z = up).
		float sphereOffset[3] = { 0.0F, 0.0F, 0.0F };
		// Restrict culling to triangles on the objective side of the glass
		// plane, so the eyepiece side can never be cut into.
		bool frontOnly = false;
		// Flip which side of the glass counts as "front" for NIFs whose
		// authored plane normal points the other way.
		bool flipFront = false;
		// Suspend the cull while a secondary sight is selected (or blending).
		// The sphere is tuned against the primary optic's eye line; from a
		// canted or top-mounted sight the same sphere cuts visible housing.
		bool disableOnSecondarySight = true;
		// Shape names never culled. The reticle, dot, aperture, and ScopeFade
		// surfaces are always protected regardless of this list.
		std::vector<std::string> excludedShapes;
	};

	class ScopeProfile
	{
	public:
		bool legacyMode = true;
		std::string path = "";
		int version = 1;
		std::string keywordName = "AUTO_Default";
		std::string animFlavorEditorID;
		std::string additionalKeywordsStr;
		std::vector<std::string> additionalKeywords = std::vector<std::string>();

		bool containAlladditionalKeywords = true;

		bool UsingSTS = false;
		bool autoProfile = false;
		std::string sourcePlugin;
		std::uint32_t sourceFormID = 0;
		// For automatic profiles: identifies the equipped attachment set inside
		// the weapon's shared profile file. It is derived from the sorted OMOD
		// FormIDs on the first-person weapon instance, not BGSZoomData (which is
		// commonly a generated form with FormID zero).
		std::string omodKey;
		unsigned int scopeFrame = 1;
		std::string ZoomNodePath;

		// Readable weapon name for the folder-per-weapon layout. Best effort:
		// Fallout 4 strips EditorIDs from most runtime forms, so this falls back
		// to the weapon's full name and then to the plugin stem. It is display
		// only -- lookup keys always come from the identity fields above, never
		// from a path, so renaming a folder by hand cannot break matching.
		std::string weaponLabel;

		ShaderData shaderData;
		ZoomDataOverwrite zoomDataOverwrite;

		// Magnification variants, secondary sights, and the authored default
		// reticle. Session state (which of these is currently selected) lives in
		// the co-save instead, because it has to fork when a save forks.
		VariantSet variants;
		std::vector<SecondarySight> secondarySights;
		// File name inside the profile's reticles folder; empty means the
		// authored 3D reticle mesh.
		std::string defaultReticleFile;
		// Size of a texture reticle, in aperture radii.
		float customReticleScale = 1.0F;
		// Sphere occlusion of the scope's own front geometry. Per profile, not
		// per variant: ShaderData is copied into every variant, so a setting
		// that must stay single-instance cannot live there.
		OcclusionSettings occlusion;

		// Directory holding this profile, when it uses the folder-per-weapon
		// layout. Empty for a legacy single-file profile.
		[[nodiscard]] std::string ProfileDirectory() const;
		// <profile directory>\reticles, or empty on the legacy layout.
		[[nodiscard]] std::string ReticleDirectory() const;
		[[nodiscard]] bool UsesFolderLayout() const;

		ScopeProfile(std::string pathO);
		//ScopeProfile(json j, std::string pathO);
		//void ReloadScopeProfile();
	};

	class ScopeDataHandler
	{
	public:
		static ScopeDataHandler* GetSingleton();

		void ReloadScopeProfile(ScopeProfile*);
		void ReadDefaultScopeDataFile();
		void ReadCustomScopeDataFiles(std::string path);
		void TestingJson();

		void WriteCurrentScopeProfile();
		void ReloadCurrentScopeProfile();

		void SetCurrentScopeProfile(ScopeProfile* data, bool containsAllAdditionkeyword = true);
		ScopeProfile* GetCurrentScopeProfile();
		ScopeProfile* GetOrCreateAutoProfile(
			RE::TESObjectWEAP* weapon,
			const RE::BGSZoomData::Data& zoomData,
			std::string attachmentKey,
			float defaultDiameter,
			float defaultMagnification,
			float zoomSpread);
		bool WriteAutoProfile(ScopeProfile* data);
		// Drops the cached automatic profile so the next GetOrCreateAutoProfile
		// synthesizes it from defaults again.
		//
		// Only the lookup entry is erased. The object itself stays owned by
		// ownedData because callers hold raw pointers to it -- currentData among
		// them -- and freeing it here would leave those dangling for the rest of
		// the session. One orphaned profile per delete is the cheap side of that
		// trade.
		void ForgetAutoProfile(const ScopeProfile* data);

		// Every loaded automatic profile, for reticle discovery to walk. Raw
		// pointers into ownedData; valid until ReloadZoomData clears it.
		[[nodiscard]] std::vector<ScopeProfile*> AllAutoProfiles() const;

		void SetOpticsKey(unsigned int keycode);

		// Directory a folder-layout profile for this identity would live in.
		// Used by the save path to migrate a legacy profile, and by reticle
		// discovery to find the textures beside it.
		[[nodiscard]] static std::string BuildProfileDirectory(
			const std::string& weaponLabel,
			const std::string& sourcePlugin,
			std::uint32_t sourceFormID,
			const std::string& omodKey);

		int GetEffectIndex();
		void SetEffectIndex(int);

		bool* GetEnableRenderThroughUI();
		void SaveEnableRenderBeforeUI(bool);

		int GetBaseRenderCount();
		void SetBaseRenderCount(int);

		void SetNVGHotKeyCombo(int);
		void SetNVGHotKeyMain(unsigned int keycode);
		void SetThermalHotKeyCombo(int);
		void SetThermalHotKeyMain(unsigned int keycode);
		void SetGuiKey(unsigned int keycode);
		void SetVerboseLogging(bool enabled);

		void SetIsUpscaler(bool);
		const char* GetNVGComboKeyStr();

		//bool ZoomDataWrite(RE::TESObjectWEAP::InstanceData* GetSington);
		void ReloadZoomData(std::string path);

	private:
		bool ReadScopeData(std::string path);
		ScopeDataHandler() = default;
		ScopeDataHandler(const ScopeDataHandler&) = delete;
		ScopeDataHandler(ScopeDataHandler&&) = delete;
		~ScopeDataHandler() = default;

		ScopeDataHandler& operator=(const ScopeDataHandler&) = delete;
		ScopeDataHandler& operator=(ScopeDataHandler&&) = delete;

	public:
		bool bEnableRenderBeforeUI = false;
		int comboNVKey = -1;
		int nvKey = -1;
		int comboThermalKey = -1;
		int thermalKey = -1;
		int guiKey = -1;
		// Optics key: tap cycles reticles, hold + scroll switches secondary
		// sights. Unbound by default so it cannot collide with another mod's
		// binding or the user's own; the editor binds it.
		int opticsKey = -1;

	private:
		int baseRenderCount = 0;
		int PassRenderIndex = 1;

		bool isUpscaler = false;
		std::vector<std::string> files;
		//CSimpleIniA iniForZoom;
		// Keyed by weapon plugin, weapon local FormID, and scope OMOD key
		// (see ScopeProfile::omodKey), so each scope attachment on a weapon gets
		// its own profile entry.
		std::map<std::tuple<std::string, std::uint32_t, std::string>, ScopeProfile*> autoProfileMap;
		std::vector<std::unique_ptr<ScopeProfile>> ownedData;
		ScopeProfile* currentData;
		std::string currentPath;
	};

}
