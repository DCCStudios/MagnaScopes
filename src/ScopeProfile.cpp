#include "ScopeProfile.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <io.h>
#include <iostream>

using namespace std;

//bool __fastcall GetInstanceKeywordStr(RE::TESObjectWEAP::InstanceData* a_instance, std::string* a_prefix);
namespace ScopeData
{
	namespace
	{
		template <std::size_t N>
		void ReadFloatArray(
			const json& object,
			std::string_view key,
			float (&destination)[N],
			const std::array<std::string_view, N>& componentNames)
		{
			const auto it = object.find(key);
			if (it == object.end() || it->is_null()) {
				return;
			}

			const json* value = std::addressof(*it);
			if (value->is_array() && value->size() == 1 && value->front().is_array()) {
				value = std::addressof(value->front());
			}

			if (value->is_object()) {
				for (std::size_t index = 0; index < N; ++index) {
					if (const auto component = value->find(componentNames[index]);
						component != value->end() && component->is_number()) {
						destination[index] = component->get<float>();
					}
				}
			} else if (value->is_array()) {
				for (std::size_t index = 0; index < std::min(N, value->size()); ++index) {
					if ((*value)[index].is_number()) {
						destination[index] = (*value)[index].get<float>();
					}
				}
			}
		}

		void UpdateConfigValue(std::string_view key, const json& value)
		{
			const std::filesystem::path path = "Data\\F4SE\\Plugins\\MagnaScopeConfig.json";
			json data = json::object();
			try {
				if (std::ifstream input(path); input) {
					data = json::parse(input, nullptr, true, true);
				}
			} catch (const std::exception& error) {
				logger::error("MagnaScopeConfig.json is invalid; rebuilding it: {}", error.what());
			}

			try {
				data[std::string(key)] = value;
				std::filesystem::create_directories(path.parent_path());
				std::ofstream output(path, std::ios::trunc);
				if (!output) {
					throw std::runtime_error("file could not be opened");
				}
				output << data.dump(2) << '\n';
			} catch (const std::exception& error) {
				logger::error("Unable to update MagnaScopeConfig.json key {}: {}", key, error.what());
			}
		}

	}

	ScopeProfile::ScopeProfile(std::string pathO)
	{
		path = pathO;
	}

#pragma region jsonRead

	void from_json(const json& j, Parallax& p)
	{
		p.radius = j.value("radius", 0.0F);
		p.relativeFogRadius = j.value("relativeFogRadius", 0.0F);
		p.scopeSwayAmount = j.value("scopeSwayAmount", 0.0F);
		p.maxTravel = j.value("maxTravel", 0.0F);
		p.sceneDepth = j.value("sceneDepth", 1.0F);
		p.shadowDepth = j.value("shadowDepth", 1.0F);
	}

	void from_json(const json& j, ZoomDataOverwrite& z)
	{
		z.x = j.value("x", 0.0F);
		z.y = j.value("y", 0.0F);
		z.z = j.value("z", 0.0F);
		z.fovMul = j.value("fovMul", 1.0F);
		z.enableZoomDateOverwrite = j.value("enableZoomDateOverwrite", false);
	}

	void from_json(const json& j, ShaderData& s)
	{
		s.IsCircle = j.value("IsCircle", true);
		s.bEnableZMove = j.value("EnableZMove", false);
		s.bCanEnableNV = j.value("EnableNV", false);
		s.bBoltDisable = j.value("bBoltDisable", false);
		s.nvIntensity = j.value("nvIntensity", 3.0F);
		s.baseWeaponPos = j.value("BaseWeaponPos", 0.0F);
		s.movePercentage = j.value("ZMovePercentage", 0.0F);
		s.camDepth = j.value("CamDepth", 1.0F);
		s.minZoom = j.value("MinZoom", 1.0F);
		s.maxZoom = j.value("MaxZoom", 4.0F);

		ReadFloatArray(j, "PositionOffset", s.PositionOffset, { "x", "y" });
		ReadFloatArray(j, "OriPositionOffset", s.OriPositionOffset, { "x", "y" });
		ReadFloatArray(j, "Size", s.Size, { "x", "y" });
		ReadFloatArray(j, "OriSize", s.OriSize, { "x", "y" });
		ReadFloatArray(j, "rectSize", s.rectSize, { "x", "y", "z", "w" });
		ReadFloatArray(j, "reticle_Offset", s.reticle_Offset, { "x", "y" });

		s.ReticleSize = j.value("ReticleSize", 4.0F);
		s.fishEyeStrength = j.value("FishEyeStrength", 0.0F);
		s.fishEyePower = j.value("FishEyePower", 2.0F);
		s.edgeRefractionStrength =
			j.value("EdgeRefractionStrength", 0.0F);
		s.edgeRefractionWidth =
			j.value("EdgeRefractionWidth", 0.15F);
		s.edgeChromaticAberration =
			j.value("EdgeChromaticAberration", 0.0F);
		s.sceneParallaxStrength =
			j.value("SceneParallaxStrength", 0.0F);
		s.opticalLagStrength =
			j.value("OpticalLagStrength", 1.0F);
		s.imageDenoise = j.value("ImageDenoise", 0.0F);
		s.imageSharpen = j.value("ImageSharpen", 0.0F);
		s.reticleMagnification =
			j.value("ReticleMagnification", 1.0F);
		s.reticleShadowStrength =
			j.value("ReticleShadowStrength", 0.0F);
		s.reticleParallaxStrength =
			j.value("ReticleParallaxStrength", 1.0F);
		s.fovAdjust = j.value("fovAdjust", 0.0F);
		s.parallax = j.value("Parallax", Parallax());
	}

	void from_json(const json& j, ScopeProfile& f)
	{
		f.keywordName = j.value("keywordEditorID", "AUTO_Default");
		f.animFlavorEditorID = j.value("AnimFlavorKeywordEditorID", "");
		f.additionalKeywordsStr = j.value("AdditionalKeywords", "");

		f.additionalKeywords.clear();
		std::stringstream ss(f.additionalKeywordsStr);
		std::string token;
		while (getline(ss, token, ',')) {
			const auto first = token.find_first_not_of(" \t\r\n");
			if (first == std::string::npos) {
				continue;
			}
			const auto last = token.find_last_not_of(" \t\r\n");
			f.additionalKeywords.push_back(token.substr(first, last - first + 1));
		}

		f.legacyMode = j.value("LegacyMode", true);
		f.version = j.value("Version", 1);
		f.UsingSTS = j.value("UsingSTS", false);
		f.autoProfile = j.value("AutoProfile", false);
		f.sourcePlugin = j.value("SourcePlugin", "");
		f.sourceFormID = j.value("SourceFormID", 0U);
		f.scopeFrame = j.value("scopeFrame", 1);
		f.ZoomNodePath = j.value("ReticleTexturePath", "");

		f.shaderData = j.value("ShaderData", ShaderData());
		f.zoomDataOverwrite = j.value("ZoomDataOverwrite", ZoomDataOverwrite());
	}

#pragma endregion

#pragma region jsonWrite

	void to_json(json& j, const Parallax& p)
	{
		j = json{
			{ "radius", p.radius },
			{ "relativeFogRadius", p.relativeFogRadius },
			{ "scopeSwayAmount", p.scopeSwayAmount },
			{ "maxTravel", p.maxTravel },
			{ "sceneDepth", p.sceneDepth },
			{ "shadowDepth", p.shadowDepth }
		};
	}

	void to_json(json& j, const ZoomDataOverwrite& z)
	{
		j = json{
			{ "x", z.x },
			{ "y", z.y },
			{ "z", z.z },
			{ "fovMul", z.fovMul },
			{ "enableZoomDateOverwrite", z.enableZoomDateOverwrite }
		};
	}

	void to_json(json& j, const ShaderData& s)
	{
		j = json{
			{ "IsCircle", s.IsCircle },
			{ "EnableZMove", s.bEnableZMove },
			{ "EnableNV", s.bCanEnableNV },
			{ "bBoltDisable", s.bBoltDisable },
			{ "nvIntensity", s.nvIntensity },
			{ "BaseWeaponPos", s.baseWeaponPos },
			{ "ZMovePercentage", s.movePercentage },
			{ "CamDepth", s.camDepth },
			{ "MinZoom", s.minZoom },
			{ "MaxZoom", s.maxZoom },
			{ "PositionOffset", { { "x", s.PositionOffset[0] }, { "y", s.PositionOffset[1] } } },
			{ "OriPositionOffset", { { "x", s.OriPositionOffset[0] }, { "y", s.OriPositionOffset[1] } } },
			{ "Size", { { "x", s.Size[0] }, { "y", s.Size[1] } } },
			{ "OriSize", { { "x", s.OriSize[0] }, { "y", s.OriSize[1] } } },
			{ "rectSize", { { "x", s.rectSize[0] }, { "y", s.rectSize[1] }, { "z", s.rectSize[2] }, { "w", s.rectSize[3] } } },
			{ "reticle_Offset", { { "x", s.reticle_Offset[0] }, { "y", s.reticle_Offset[1] } } },

			{ "ReticleSize", s.ReticleSize },
			{ "FishEyeStrength", s.fishEyeStrength },
			{ "FishEyePower", s.fishEyePower },
			{ "EdgeRefractionStrength", s.edgeRefractionStrength },
			{ "EdgeRefractionWidth", s.edgeRefractionWidth },
			{ "EdgeChromaticAberration", s.edgeChromaticAberration },
			{ "SceneParallaxStrength", s.sceneParallaxStrength },
			{ "OpticalLagStrength", s.opticalLagStrength },
			{ "ImageDenoise", s.imageDenoise },
			{ "ImageSharpen", s.imageSharpen },
			{ "ReticleMagnification", s.reticleMagnification },
			{ "ReticleShadowStrength", s.reticleShadowStrength },
			{ "ReticleParallaxStrength", s.reticleParallaxStrength },
			{ "fovAdjust", s.fovAdjust },
			//
			{ "Parallax", s.parallax }
		};
	}

	// 为ScopeProfile类型定义to_json函数
	void to_json(json& j, const ScopeProfile& f)
	{
		std::ostringstream oss;
		for (int i = 0; i < f.additionalKeywords.size(); i++) {
			oss << f.additionalKeywords[i];
			if (i < f.additionalKeywords.size() - 1)
				oss << ",";
		}
		std::string tempkeyStr = oss.str();

		j = json{
			{ "keywordEditorID", f.keywordName },
			{ "AnimFlavorKeywordEditorID", f.animFlavorEditorID },
			{ "AdditionalKeywords", tempkeyStr },
			{ "LegacyMode", f.legacyMode },
			{ "path", f.path },
			{ "Version", f.version },
			{ "UsingSTS", f.UsingSTS },
			{ "AutoProfile", f.autoProfile },
			{ "SourcePlugin", f.sourcePlugin },
			{ "SourceFormID", f.sourceFormID },
			{ "scopeFrame", f.scopeFrame },
			{ "ReticleTexturePath", f.ZoomNodePath },
			//
			{ "ShaderData", f.shaderData },
			{ "ZoomDataOverwrite", f.zoomDataOverwrite }
		};
	}

#pragma endregion

	void ScopeDataHandler::TestingJson()
	{
	}

	void ScopeDataHandler::WriteCurrentScopeProfile()
	{
		if (!currentData) {
			return;
		}

		WriteAutoProfile(currentData);
	}

	void ScopeDataHandler::ReloadScopeProfile(ScopeProfile* data)
	{
		if (!data) {
			return;
		}

		try {
			std::ifstream input(data->path);
			if (!input) {
				throw std::runtime_error("file could not be opened");
			}
			const auto parsed = json::parse(input, nullptr, true, true);

			// Automatic profile files hold one entry per scope attachment;
			// reload only this profile's own entry. If the entry is missing
			// (never saved), keep the in-memory values instead of wiping them
			// with defaults.
			if (parsed.contains("Scopes") && parsed["Scopes"].is_object()) {
				const auto entryKey = data->omodKey.empty() ? std::string("Default") : data->omodKey;
				const auto& scopes = parsed["Scopes"];
				if (const auto entry = scopes.find(entryKey); entry != scopes.end()) {
					entry->get_to(*data);
					data->autoProfile = true;
				}
				return;
			}

			parsed.get_to(*data);
		} catch (const std::exception& error) {
			logger::error("Unable to reload scope profile {}: {}", data->path, error.what());
		}
	}

	void ScopeDataHandler::ReloadCurrentScopeProfile()
	{
	}

	void ScopeDataHandler::SetCurrentScopeProfile(ScopeProfile* data, bool containsAllAdditionkeyword)
	{
		currentData = data;
		if (data)
			data->containAlladditionalKeywords = containsAllAdditionkeyword;
	}

	ScopeProfile* ScopeDataHandler::GetCurrentScopeProfile()
	{
		return currentData;
	}

	ScopeDataHandler* ScopeDataHandler::GetSingleton()
	{
		static ScopeDataHandler singleton;
		return std::addressof(singleton);
	}

	bool ScopeDataHandler::ReadScopeData(string path)
	{
		if (path.find("__folder_managed_by_vortex") != std::string::npos)
			return false;

		try {
			std::ifstream input(path);
			if (!input) {
				throw std::runtime_error("file could not be opened");
			}

			const auto parsed = json::parse(input, nullptr, true, true);
			const auto applyAutomaticOpticsDefaults =
				[](const json& entry, ScopeProfile& data) {
					const auto shaderEntry = entry.find("ShaderData");
					if (shaderEntry == entry.end() ||
						!shaderEntry->is_object()) {
						return;
					}
					// Earlier generated profiles predate scene parallax. A
					// missing key must gain the automatic-profile default;
					// an authored zero remains an intentional opt-out.
					if (!shaderEntry->contains(
							"SceneParallaxStrength")) {
						data.shaderData.sceneParallaxStrength = 1.0F;
					}
					if (!std::isfinite(
							data.shaderData.opticalLagStrength)) {
						data.shaderData.opticalLagStrength = 1.0F;
					}
					data.shaderData.opticalLagStrength =
						std::clamp(
							data.shaderData.opticalLagStrength,
							0.0F,
							4.0F);
				};

			// Version 2 automatic profile file: one file per weapon, holding
			// one entry per scope attachment under "Scopes", keyed by the
			// sight's ZoomData form ("plugin:localFormID").
			if (parsed.contains("Scopes") && parsed["Scopes"].is_object()) {
				const auto filePlugin = parsed.value("SourcePlugin", "");
				const auto fileFormID = parsed.value("SourceFormID", 0U);
				bool loadedAny = false;
				for (const auto& [omodKey, entry] : parsed["Scopes"].items()) {
					auto data = std::make_unique<ScopeProfile>(path);
					entry.get_to(*data);
					data->autoProfile = true;
					applyAutomaticOpticsDefaults(entry, *data);
					if (data->sourcePlugin.empty()) {
						data->sourcePlugin = filePlugin;
					}
					if (data->sourceFormID == 0) {
						data->sourceFormID = fileFormID;
					}
					data->omodKey = omodKey;
					if (data->sourcePlugin.empty() || data->sourceFormID == 0) {
						logger::warn("Skipping auto profile entry {} in {}: missing weapon identity", omodKey, path);
						continue;
					}
					autoProfileMap.insert_or_assign(
						{ data->sourcePlugin, data->sourceFormID, omodKey },
						data.get());
					ownedData.push_back(std::move(data));
					loadedAny = true;
				}
				return loadedAny;
			}

			auto data = std::make_unique<ScopeProfile>(path);
			parsed.get_to(*data);

			auto* dataPointer = data.get();
			if (dataPointer->autoProfile && !dataPointer->sourcePlugin.empty() && dataPointer->sourceFormID != 0) {
				applyAutomaticOpticsDefaults(parsed, *dataPointer);
				// Earlier flat auto profile file (one weapon, one entry). Loads
				// as the file-wide default entry; the next save rewrites the
				// file in the per-scope container format.
				dataPointer->omodKey = "Default";
				autoProfileMap.insert_or_assign(
					{ dataPointer->sourcePlugin, dataPointer->sourceFormID, dataPointer->omodKey },
					dataPointer);
			} else {
				logger::warn(
					"Skipping non-automatic profile outside MagnaScope's STS contract: {}",
					path);
				return false;
			}
			ownedData.push_back(std::move(data));
			return true;
		} catch (const std::exception& error) {
			logger::error("Skipping invalid scope profile {}: {}", path, error.what());
			return false;
		}
	}

	int ScopeDataHandler::GetEffectIndex()
	{
		return PassRenderIndex;
	}

	void ScopeDataHandler::SetEffectIndex(int renderIndex)
	{
		PassRenderIndex = renderIndex;
		UpdateConfigValue("RenderPassIndex", PassRenderIndex);
	}

	void ScopeDataHandler::SetIsUpscaler(bool flag)
	{
		isUpscaler = flag;
	}

	bool* ScopeDataHandler::GetEnableRenderThroughUI()
	{
		return &isUpscaler;
	}

	void ScopeDataHandler::SaveEnableRenderBeforeUI(bool flag)
	{
		if (isUpscaler)
			return;
		bEnableRenderBeforeUI = flag;
		UpdateConfigValue("EnableRenderBeforeUI", bEnableRenderBeforeUI);
	}

	int ScopeDataHandler::GetBaseRenderCount()
	{
		return baseRenderCount;
	}

	void ScopeDataHandler::SetBaseRenderCount(int renderIndex)
	{
		baseRenderCount = renderIndex;
		UpdateConfigValue("BaseRenderCount", baseRenderCount);
	}

	int ComboKeyToInt(std::string comboStr)
	{
		std::for_each(
			comboStr.begin(),
			comboStr.end(),
			[](char& c) {
				c = ::tolower(c);
			});

		int comboNVKey;

		if (comboStr.compare("shift") == 0)
			comboNVKey = 160;
		else if (comboStr.compare("rshift") == 0)
			comboNVKey = 161;
		else if (comboStr.compare("ctrl") == 0)
			comboNVKey = 162;
		else if (comboStr.compare("rctrl") == 0)
			comboNVKey = 163;
		else if (comboStr.compare("alt") == 0)
			comboNVKey = 164;
		else if (comboStr.compare("ralt") == 0)
			comboNVKey = 165;
		else
			comboNVKey = -1;
		return comboNVKey;
	}

	void ScopeDataHandler::ReadDefaultScopeDataFile()
	{
		const std::filesystem::path path = "Data\\F4SE\\Plugins\\MagnaScopeConfig.json";
		json data = json::object();
		try {
			if (std::ifstream input(path); input) {
				data = json::parse(input, nullptr, true, true);
			}
		} catch (const std::exception& error) {
			logger::error("MagnaScopeConfig.json is invalid; using safe defaults: {}", error.what());
		}

		PassRenderIndex = data.value("RenderPassIndex", 1);
		bEnableRenderBeforeUI = data.value("EnableRenderBeforeUI", false);
		baseRenderCount = data.value("BaseRenderCount", 10);

		const auto combo = data.find("ComboNVKey");
		if (combo != data.end() && combo->is_string()) {
			comboNVKey = ComboKeyToInt(combo->get<std::string>());
		} else if (combo != data.end() && combo->is_number_integer()) {
			comboNVKey = combo->get<int>();
		} else {
			comboNVKey = -1;
		}

		nvKey = data.value("NvKey", 0);
		guiKey = data.value("guiKey", 117);

		data["RenderPassIndex"] = PassRenderIndex;
		data["EnableRenderBeforeUI"] = bEnableRenderBeforeUI;
		data["BaseRenderCount"] = baseRenderCount;
		data["ComboNVKey"] = comboNVKey;
		data["NvKey"] = nvKey;
		data["guiKey"] = guiKey;

		try {
			std::filesystem::create_directories(path.parent_path());
			std::ofstream output(path, std::ios::trunc);
			output << data.dump(2) << '\n';
		} catch (const std::exception& error) {
			logger::error("Unable to write MagnaScopeConfig.json defaults: {}", error.what());
		}
	}

	const char* ScopeDataHandler::GetNVGComboKeyStr()
	{
		const char* comboNVKeya;

		if (this->comboNVKey == 160)
			comboNVKeya = "Shift";
		else if (this->comboNVKey == 161)
			comboNVKeya = "RShift";
		else if (this->comboNVKey == 162)
			comboNVKeya = "Ctrl";
		else if (this->comboNVKey == 163)
			comboNVKeya = "RCtrl";
		else if (this->comboNVKey == 164)
			comboNVKeya = "Alt";
		else if (this->comboNVKey == 165)
			comboNVKeya = "RAlt";
		else
			comboNVKeya = "None";
		return comboNVKeya;
	}

	void ScopeDataHandler::SetNVGHotKeyCombo(int comboKey)
	{
		comboNVKey = comboKey;
		UpdateConfigValue("ComboNVKey", comboKey);
	}

	void ScopeDataHandler::SetNVGHotKeyMain(unsigned int mainkeycode)
	{
		nvKey = mainkeycode;
		UpdateConfigValue("NvKey", nvKey);
	}

	void ScopeDataHandler::SetGuiKey(unsigned int mainkeycode)
	{
		guiKey = mainkeycode;
		UpdateConfigValue("guiKey", guiKey);
	}

	void ScopeDataHandler::ReadCustomScopeDataFiles(std::string path)
	{
		files.clear();
		const std::filesystem::path root(path);
		if (!std::filesystem::exists(root)) {
			logger::info("Scope profile directory {} does not exist", root.string());
			return;
		}

		for (const auto& entry : std::filesystem::recursive_directory_iterator(
				 root,
				 std::filesystem::directory_options::skip_permission_denied)) {
			if (!entry.is_regular_file() || entry.path().extension() != ".json") {
				continue;
			}
			files.push_back(entry.path().string());
		}

		std::ranges::sort(files);
		for (const auto& file : files) {
			ReadScopeData(file);
		}
		logger::info("Loaded {} automatic STS scope profiles", autoProfileMap.size());
	}

	void ScopeDataHandler::ReloadZoomData(std::string path)
	{
		SetCurrentScopeProfile(nullptr);
		autoProfileMap.clear();
		ownedData.clear();
		ReadCustomScopeDataFiles(path);
		ReadDefaultScopeDataFile();
	}

	ScopeProfile* ScopeDataHandler::GetOrCreateAutoProfile(
		RE::TESObjectWEAP* weapon,
		const RE::BGSZoomData::Data& zoomData,
		std::string attachmentKey,
		float defaultDiameter,
		float defaultMagnification,
		float zoomSpread)
	{
		if (!weapon) {
			return nullptr;
		}

		const auto* sourceFile = weapon->GetFile(0);
		const auto sourcePlugin = sourceFile ?
		                              std::string(sourceFile->GetFilename()) :
		                              std::string("Fallout4.esm");
		const auto sourceFormID = sourceFile ? weapon->GetLocalFormID() : weapon->GetFormID();

		// BGSZoomData is often generated instance data and can have FormID zero,
		// which collapsed unrelated sights into one profile. The caller now
		// supplies the stable, sorted set of attached OMOD identities.
		const std::string omodKey =
			attachmentKey.empty() ? std::string("Default") : std::move(attachmentKey);

		const auto key = std::tuple{ sourcePlugin, sourceFormID, omodKey };
		if (const auto existing = autoProfileMap.find(key); existing != autoProfileMap.end()) {
			auto* profile = existing->second;
			// Upgrade only the exact former generated tuple. Values that differ
			// are user-authored and must remain untouched.
			if (profile &&
				std::abs(profile->shaderData.parallax.radius - 2.65F) < 0.0001F &&
				std::abs(profile->shaderData.parallax.relativeFogRadius - 9.0F) < 0.0001F &&
				std::abs(profile->shaderData.parallax.scopeSwayAmount - 3.0F) < 0.0001F &&
				std::abs(profile->shaderData.parallax.maxTravel - 1.0F) < 0.0001F) {
				profile->shaderData.parallax.radius = 2.0F;
				profile->shaderData.parallax.maxTravel = 4.0F;
				if (profile->shaderData.sceneParallaxStrength == 0.0F) {
					profile->shaderData.sceneParallaxStrength = 0.5F;
				}
				logger::info(
					"Upgraded former automatic eye-box defaults for [{}]",
					omodKey);
			}
			return profile;
		}

		// Builds before attachment enumeration used a generated BGSZoomData
		// FormID, commonly "Fallout4.esm:00000000". Preserve a user's tuning
		// when that is the sole legacy entry for this weapon, then save it
		// under the stable OMOD signature on the next explicit save.
		auto legacyEntry = autoProfileMap.end();
		auto defaultEntry = autoProfileMap.end();
		for (auto candidate = autoProfileMap.begin();
			candidate != autoProfileMap.end();
			++candidate) {
			const auto& [candidatePlugin, candidateFormID, candidateAttachment] =
				candidate->first;
			if (candidatePlugin != sourcePlugin ||
				candidateFormID != sourceFormID) {
				continue;
			}
			if (candidateAttachment.ends_with(":00000000")) {
				// Prefer the later generated-form entry over the older flat
				// "Default" migration because it contains the latest tuning.
				if (legacyEntry != autoProfileMap.end()) {
					legacyEntry = autoProfileMap.end();
					break;
				}
				legacyEntry = candidate;
			} else if (candidateAttachment == "Default") {
				defaultEntry = candidate;
			}
		}
		if (legacyEntry == autoProfileMap.end()) {
			legacyEntry = defaultEntry;
		}
		if (legacyEntry != autoProfileMap.end()) {
			auto* migrated = legacyEntry->second;
			const auto oldAttachment = std::get<2>(legacyEntry->first);
			autoProfileMap.erase(legacyEntry);
			migrated->omodKey = omodKey;
			migrated->keywordName =
				std::format("AUTO_{:08X} [{}]", sourceFormID, omodKey);
			autoProfileMap.emplace(key, migrated);
			logger::info(
				"Migrated automatic STS profile attachment identity from [{}] to [{}]",
				oldAttachment,
				omodKey);
			if (migrated &&
				std::abs(migrated->shaderData.parallax.radius - 2.65F) < 0.0001F &&
				std::abs(migrated->shaderData.parallax.relativeFogRadius - 9.0F) < 0.0001F &&
				std::abs(migrated->shaderData.parallax.scopeSwayAmount - 3.0F) < 0.0001F &&
				std::abs(migrated->shaderData.parallax.maxTravel - 1.0F) < 0.0001F) {
				migrated->shaderData.parallax.radius = 2.0F;
				migrated->shaderData.parallax.maxTravel = 4.0F;
				if (migrated->shaderData.sceneParallaxStrength == 0.0F) {
					migrated->shaderData.sceneParallaxStrength = 0.5F;
				}
			}
			return migrated;
		}

		std::string safePlugin = sourcePlugin;
		std::ranges::replace_if(
			safePlugin,
			[](const char value) {
				return value == '<' || value == '>' || value == ':' || value == '"' ||
			           value == '/' || value == '\\' || value == '|' || value == '?' || value == '*';
			},
			'_');

		const auto profilePath = std::filesystem::path("Data\\F4SE\\Plugins\\MagnaScope\\Auto") /
		                         std::format("{}_{:08X}.json", safePlugin, sourceFormID);
		auto profile = std::make_unique<ScopeProfile>(profilePath.string());
		profile->keywordName = std::format("AUTO_{:08X} [{}]", sourceFormID, omodKey);
		profile->omodKey = omodKey;
		profile->legacyMode = true;
		profile->UsingSTS = true;
		profile->autoProfile = true;
		profile->sourcePlugin = sourcePlugin;
		profile->sourceFormID = sourceFormID;
		profile->scopeFrame = 1;
		// A null SRV samples transparent black in D3D11, so automatic STS
		// profiles need no placeholder texture asset.
		profile->ZoomNodePath.clear();

		// Automatic profiles keep the weapon's own zoom untouched and layer
		// the scope effect on top of it. Neutralizing the game zoom is not
		// viable without per-scope camera retuning: STS sighted cameras sit
		// at the eyepiece and only see black scope interior at unzoomed FOV.
		// The overlay starts at the configured default magnification and the
		// mouse wheel can push it up to magnification * spread while aiming.
		(void)defaultMagnification;
		profile->shaderData.minZoom = 1.0F;
		profile->shaderData.maxZoom =
			profile->shaderData.minZoom * std::max(1.0F, zoomSpread);
		// Keep the center optically quiet and bend only subtly toward the rim.
		// The earlier 0.35 default visibly warped the entire image.
		profile->shaderData.fishEyeStrength = 0.05F;
		profile->shaderData.fishEyePower = 2.0F;
		profile->shaderData.edgeRefractionStrength = 0.05F;
		profile->shaderData.edgeRefractionWidth = 0.235F;
		profile->shaderData.edgeChromaticAberration = 2.0F;
		profile->shaderData.sceneParallaxStrength = 1.0F;
		profile->shaderData.opticalLagStrength = 0.2F;
		// The tested baseline leaves edge-aware cleanup disabled; a large
		// spatial kernel can turn stable object edges into shadow-like blobs.
		profile->shaderData.imageDenoise = 0.0F;
		profile->shaderData.imageSharpen = 0.5F;
		profile->shaderData.ReticleSize = 4.0F;
		profile->shaderData.reticle_Offset[0] = 0.0F;
		profile->shaderData.reticle_Offset[1] = 0.0F;
		profile->shaderData.reticleMagnification = 1.0F;
		profile->shaderData.reticleShadowStrength = 0.0F;
		profile->shaderData.reticleParallaxStrength = 1.0F;
		const float diameter = std::clamp(defaultDiameter, 64.0F, 2160.0F);
		profile->shaderData.Size[0] = diameter;
		profile->shaderData.Size[1] = diameter;
		profile->shaderData.OriSize[0] = diameter;
		profile->shaderData.OriSize[1] = diameter;
		// Tested MagnaScope baseline: the fog reaches black slightly inside the
		// mask edge, producing a dark eye-relief ring at the rim and a moving
		// scope shadow as the eye drifts away from the optical axis.
		profile->shaderData.parallax.radius = 1.55F;
		profile->shaderData.parallax.relativeFogRadius = 7.0F;
		profile->shaderData.parallax.scopeSwayAmount = 18.0F;
		profile->shaderData.parallax.maxTravel = 4.0F;
		profile->shaderData.parallax.sceneDepth = 1.0F;
		profile->shaderData.parallax.shadowDepth = 1.0F;

		// New automatic profiles begin from the weapon's authored sighted zoom
		// and camera offsets. Lens magnification remains neutral at 1x until the
		// user or a saved profile changes it, so STS alignment is not silently
		// replaced by a guessed FOV value.
		profile->zoomDataOverwrite.enableZoomDateOverwrite = true;
		profile->zoomDataOverwrite.fovMul = zoomData.fovMult;
		profile->zoomDataOverwrite.x = zoomData.cameraOffset.x;
		profile->zoomDataOverwrite.y = zoomData.cameraOffset.y;
		profile->zoomDataOverwrite.z = zoomData.cameraOffset.z;

		auto* result = profile.get();
		ownedData.push_back(std::move(profile));
		autoProfileMap.emplace(key, result);
		logger::info(
			"Synthesized STS auto profile for {}:{:08X} scope [{}] (weapon fovMult {:.2f}, overlay zoom up to {:.2f}x)",
			sourcePlugin,
			sourceFormID,
			omodKey,
			zoomData.fovMult,
			result->shaderData.maxZoom);
		return result;
	}

	bool ScopeDataHandler::WriteAutoProfile(ScopeProfile* data)
	{
		if (!data || !data->autoProfile || data->path.empty()) {
			return false;
		}

		try {
			const std::filesystem::path outputPath(data->path);

			// The file is shared by every scope attachment on this weapon, so
			// merge into the existing content instead of overwriting it.
			json fileJson = json::object();
			if (std::ifstream input(outputPath); input) {
				try {
					fileJson = json::parse(input, nullptr, true, true);
				} catch (const std::exception&) {
					fileJson = json::object();
				}
			}
			if (!fileJson.is_object() || !fileJson.contains("Scopes") || !fileJson["Scopes"].is_object()) {
				// Also covers legacy flat files: their single entry becomes
				// the "Default" entry of the new container format.
				json scopes = json::object();
				if (fileJson.is_object() && fileJson.value("AutoProfile", false)) {
					scopes["Default"] = fileJson;
				}
				fileJson = json::object();
				fileJson["Scopes"] = std::move(scopes);
			}

			fileJson["AutoProfileFile"] = 2;
			fileJson["SourcePlugin"] = data->sourcePlugin;
			fileJson["SourceFormID"] = data->sourceFormID;
			const std::string entryKey = data->omodKey.empty() ? "Default" : data->omodKey;
			fileJson["Scopes"][entryKey] = *data;

			std::filesystem::create_directories(outputPath.parent_path());
			std::ofstream output(outputPath, std::ios::trunc);
			if (!output) {
				throw std::runtime_error("file could not be opened");
			}
			output << fileJson.dump(2) << '\n';
			logger::info("Saved STS auto profile {} entry [{}]", outputPath.string(), entryKey);
			return true;
		} catch (const std::exception& error) {
			logger::error("Unable to save STS auto profile {}: {}", data->path, error.what());
			return false;
		}
	}

	std::vector<std::string_view> splitSV(std::string_view strv, std::string_view delims)
	{
		std::vector<std::string_view> output;
		size_t first = 0;

		while (first < strv.size()) {
			const auto second = strv.find_first_of(delims, first);

			if (first != second)
				output.emplace_back(strv.substr(first, second - first));

			if (second == std::string_view::npos)
				break;

			first = second + 1;
		}

		return output;
	}

	//vector<string> ScopeDataHandler::GetFilesName()
	//{
	//	return files;
	//}
}
