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
		// Moves the sampled scene beneath the fixed physical aperture as the
		// eye leaves the optical axis. This is expressed in aperture radii and
		// is separate from the exit-pupil shadow travel.
		float sceneParallaxStrength = 0.0F;
		// Multiplies transient ScopeFade-local eye motion before it drives the
		// exit pupil and scene counter-shift. It does not change the settled
		// center, so ordinary camera pitch/yaw cannot permanently offset the
		// optic.
		float opticalLagStrength = 1.0F;

		// Optional single-pass cleanup for the automatic STS scene sample.
		// Denoise is an edge-aware spatial blend, not temporal reconstruction;
		// sharpen restores local contrast after magnification. Both default to
		// zero so legacy FTS JSON retains its authored image unchanged.
		float imageDenoise = 0.0F;
		float imageSharpen = 0.0F;

		// Independent local scale for the STS-authored 3D reticle. 1 preserves
		// its authored size; this value never inherits scene magnification.
		float reticleMagnification = 1.0F;

		float fovAdjust = 0;
		Parallax parallax;
	};

	class FTSData
	{
	public:
		bool legacyMode = true;
		std::string path = "";
		int version = 1;
		std::string keywordName = "FTS_Default";
		std::string animFlavorEditorID = "FTS_NONE";
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

		FTSData(std::string pathO);
		//FTSData(json j, std::string pathO);
		//void ReloadFTSData();
	};

	class ScopeDataHandler
	{
	public:
		static ScopeDataHandler* GetSingleton();

		void ReloadFTSData(FTSData*);
		void ReadDefaultScopeDataFile();
		void ReadCustomScopeDataFiles(std::string path);
		void TestingJson();

		void WriteCurrentFTSData();
		void ReloadCurrentFTSData();

		void SetCurrentFTSData(FTSData* data, bool containsAllAdditionkeyword = true);
		FTSData* GetCurrentFTSData();
		FTSData* GetOrCreateAutoProfile(
			RE::TESObjectWEAP* weapon,
			const RE::BGSZoomData::Data& zoomData,
			std::string attachmentKey,
			float defaultDiameter,
			float defaultMagnification,
			float zoomSpread);
		bool WriteAutoProfile(FTSData* data);

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

		std::multimap<std::string, FTSData*>* GetScopeDataMap();

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
		std::multimap<std::string, FTSData*> ScopeDataMap;
		// Keyed by weapon plugin, weapon local FormID, and scope OMOD key
		// (see FTSData::omodKey), so each scope attachment on a weapon gets
		// its own profile entry.
		std::map<std::tuple<std::string, std::uint32_t, std::string>, FTSData*> autoProfileMap;
		std::vector<std::unique_ptr<FTSData>> ownedData;
		FTSData* currentData;
		std::string currentPath;
	};

}
