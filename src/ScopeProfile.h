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

		float nvIntensity = 3;
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
		float lensScale = 1.0F;

		Breathing breathing;

		float fovAdjust = 0;
		Parallax parallax;
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

		ShaderData shaderData;
		ZoomDataOverwrite zoomDataOverwrite;

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

		int GetEffectIndex();
		void SetEffectIndex(int);

		bool* GetEnableRenderThroughUI();
		void SaveEnableRenderBeforeUI(bool);

		int GetBaseRenderCount();
		void SetBaseRenderCount(int);

		void SetNVGHotKeyCombo(int);
		void SetNVGHotKeyMain(unsigned int keycode);
		void SetGuiKey(unsigned int keycode);

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
		int guiKey = -1;

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
