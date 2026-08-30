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

		// Windows-illegal characters plus a few that make a folder awkward to
		// type. Trailing dots and spaces are stripped because Windows silently
		// drops them, which would make a directory unreachable by the name we
		// recorded.
		std::string SanitiseForPath(std::string_view value)
		{
			std::string result;
			result.reserve(value.size());
			for (const char character : value) {
				switch (character) {
				case '<': case '>': case ':': case '"': case '/':
				case '\\': case '|': case '?': case '*':
					result.push_back('_');
					break;
				default:
					result.push_back(
						static_cast<unsigned char>(character) < 0x20 ?
							'_' :
							character);
					break;
				}
			}
			while (!result.empty() &&
				   (result.back() == '.' || result.back() == ' ')) {
				result.pop_back();
			}
			if (result.empty()) {
				result = "Unnamed";
			}
			// Keep well inside MAX_PATH once the root and the omod folder are
			// appended.
			if (result.size() > 64U) {
				result.resize(64U);
			}
			return result;
		}

		constexpr const char* kAutoRoot = "Data\\F4SE\\Plugins\\MagnaScope\\Auto";
		constexpr const char* kProfileFileName = "profile.json";

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

	bool ScopeProfile::UsesFolderLayout() const
	{
		if (path.empty()) {
			return false;
		}
		return std::filesystem::path(path).filename() == kProfileFileName;
	}

	std::string ScopeProfile::ProfileDirectory() const
	{
		if (!UsesFolderLayout()) {
			return {};
		}
		return std::filesystem::path(path).parent_path().string();
	}

	std::string ScopeProfile::ReticleDirectory() const
	{
		const auto directory = ProfileDirectory();
		if (directory.empty()) {
			return {};
		}
		return (std::filesystem::path(directory) / "reticles").string();
	}

	std::string ScopeDataHandler::BuildProfileDirectory(
		const std::string& weaponLabel,
		const std::string& sourcePlugin,
		std::uint32_t sourceFormID,
		const std::string& omodKey)
	{
		// The folder name is readable, but it is never the lookup key: every
		// key comes from the identity fields inside profile.json. Renaming a
		// folder by hand therefore only moves where its reticles live, and
		// cannot orphan the profile.
		const auto label = SanitiseForPath(
			weaponLabel.empty() ?
				std::filesystem::path(sourcePlugin).stem().string() :
				weaponLabel);
		const auto weaponFolder = std::format(
			"{} [{}_{:08X}]",
			label,
			SanitiseForPath(std::filesystem::path(sourcePlugin).stem().string()),
			sourceFormID);
		const auto scopeFolder =
			SanitiseForPath(omodKey.empty() ? std::string("Default") : omodKey);
		return (std::filesystem::path(kAutoRoot) / weaponFolder / scopeFolder)
			.string();
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
		p.imageStillness = j.value("imageStillness", 0.0F);
		p.axialBreathing = j.value("axialBreathing", 0.0F);
		p.recenterSpeed = j.value("recenterSpeed", 1.0F);
		p.strafeLag = j.value("strafeLag", 1.0F);
		p.tubeDepth = j.value("tubeDepth", 0.0F);
	}

	void from_json(const json& j, Breathing& b)
	{
		b.rate = j.value("rate", 0.25F);
		b.sway = j.value("sway", 0.0F);
		b.drift = j.value("drift", 0.0F);
		b.figure = j.value("figure", 0.25F);
		b.hold = j.value("hold", 0.0F);
		b.pupilFollow = j.value("pupilFollow", 1.0F);
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
		s.bCanEnableThermal = j.value("EnableThermal", false);
		s.bDefaultEnableNV = j.value("DefaultEnableNV", false);
		s.bDefaultEnableThermal = j.value("DefaultEnableThermal", false);
		s.bBoltDisable = j.value("bBoltDisable", false);
		s.nvIntensity = j.value("nvIntensity", 3.0F);
		s.nvNoise = j.value("nvNoise", 0.15F);
		s.nvBloom = j.value("nvBloom", 0.30F);
		s.nvTint = j.value("nvTint", 0);
		s.thermalPalette = j.value("thermalPalette", 0);
		s.thermalContrast = j.value("thermalContrast", 1.0F);
		s.thermalEdge = j.value("thermalEdge", 0.25F);
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
		s.magnificationFilter =
			std::clamp(j.value("MagnificationFilter", 0), 0, 2);
		s.reticleMagnification =
			j.value("ReticleMagnification", 1.0F);
		s.reticleShadowStrength =
			j.value("ReticleShadowStrength", 0.0F);
		s.reticleParallaxStrength =
			j.value("ReticleParallaxStrength", 1.0F);
		ReadFloatArray(j, "LensOffset", s.lensOffset, { "x", "y" });
		s.lensScale = j.value("LensScale", 1.54F);
		s.apertureSurface = j.value("ApertureSurface", std::string{});
		s.reticleSurface = j.value("ReticleSurface", std::string{});
		s.fovAdjust = j.value("fovAdjust", 0.0F);
		s.parallax = j.value("Parallax", Parallax());
		s.breathing = j.value("Breathing", Breathing());
	}

	void from_json(const json& j, ProfileVariant& v)
	{
		v.id = j.value("Id", 0U);
		v.magnification = j.value("Magnification", 1.0F);
		v.label = j.value("Label", "");
		v.shaderData = j.value("ShaderData", ShaderData());
		v.zoomDataOverwrite = j.value("ZoomDataOverwrite", ZoomDataOverwrite());
	}

	void from_json(const json& j, VariantSet& v)
	{
		v.enabled = j.value("Enabled", false);
		v.continuous = j.value("Continuous", false);
		v.stepSeconds = j.value("StepSeconds", 0.12F);
		v.nextId = j.value("NextId", 1U);
		v.defaultVariantId = j.value("DefaultVariantId", 0U);
		v.variants = j.value("Variants", std::vector<ProfileVariant>());
		// Ids are the co-save's key, so a hand-edited or pre-id file must be
		// repaired on load rather than left with colliding zeros.
		for (auto& variant : v.variants) {
			if (variant.id == 0U) {
				variant.id = v.nextId++;
			}
			v.nextId = std::max(v.nextId, variant.id + 1U);
		}
		std::ranges::stable_sort(
			v.variants,
			[](const ProfileVariant& left, const ProfileVariant& right) {
				return left.magnification < right.magnification;
			});
	}

	void from_json(const json& j, SecondarySight& s)
	{
		s.name = j.value("Name", "Iron Sights");
		s.zoomData = j.value("ZoomData", ZoomDataOverwrite());
		s.suppressOptics = j.value("SuppressOptics", true);
		s.transitionSeconds = j.value("TransitionSeconds", 0.18F);
	}

	void from_json(const json& j, OcclusionSettings& o)
	{
		o.enabled = j.value("Enabled", false);
		o.sphereRadius = j.value("SphereRadius", 5.0F);
		const auto offset =
			j.value("SphereOffset", std::vector<float>{ 0.0F, 0.0F, 0.0F });
		for (std::size_t index = 0;
			 index < 3U && index < offset.size();
			 ++index) {
			o.sphereOffset[index] = offset[index];
		}
		o.frontOnly = j.value("FrontOnly", false);
		o.disableOnSecondarySight = j.value("DisableOnSecondarySight", true);
		o.flipFront = j.value("FlipFront", false);
		o.excludedShapes =
			j.value("ExcludedShapes", std::vector<std::string>());
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
		f.omodKey = j.value("OmodKey", "");
		f.scopeFrame = j.value("scopeFrame", 1);
		f.ZoomNodePath = j.value("ReticleTexturePath", "");

		f.weaponLabel = j.value("WeaponLabel", "");

		f.shaderData = j.value("ShaderData", ShaderData());
		f.zoomDataOverwrite = j.value("ZoomDataOverwrite", ZoomDataOverwrite());

		f.variants = j.value("Variants", VariantSet());
		f.secondarySights = j.value("SecondarySights", std::vector<SecondarySight>());
		f.defaultReticleFile = j.value("DefaultReticleFile", "");
		f.customReticleScale = j.value("CustomReticleScale", 1.0F);
		// Absent on every pre-existing profile -> default-constructed ->
		// disabled; old files round-trip unchanged apart from gaining the key
		// on their next save.
		f.occlusion = j.value("Occlusion", OcclusionSettings());
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
			{ "shadowDepth", p.shadowDepth },
			{ "imageStillness", p.imageStillness },
			{ "axialBreathing", p.axialBreathing },
			{ "recenterSpeed", p.recenterSpeed },
			{ "strafeLag", p.strafeLag },
			{ "tubeDepth", p.tubeDepth }
		};
	}

	void to_json(json& j, const Breathing& b)
	{
		j = json{
			{ "rate", b.rate },
			{ "sway", b.sway },
			{ "drift", b.drift },
			{ "figure", b.figure },
			{ "hold", b.hold },
			{ "pupilFollow", b.pupilFollow }
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
			{ "EnableThermal", s.bCanEnableThermal },
			{ "DefaultEnableNV", s.bDefaultEnableNV },
			{ "DefaultEnableThermal", s.bDefaultEnableThermal },
			{ "bBoltDisable", s.bBoltDisable },
			{ "nvIntensity", s.nvIntensity },
			{ "nvNoise", s.nvNoise },
			{ "nvBloom", s.nvBloom },
			{ "nvTint", s.nvTint },
			{ "thermalPalette", s.thermalPalette },
			{ "thermalContrast", s.thermalContrast },
			{ "thermalEdge", s.thermalEdge },
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
			{ "MagnificationFilter", s.magnificationFilter },
			{ "ReticleMagnification", s.reticleMagnification },
			{ "ReticleShadowStrength", s.reticleShadowStrength },
			{ "ReticleParallaxStrength", s.reticleParallaxStrength },
			{ "LensOffset", { { "x", s.lensOffset[0] }, { "y", s.lensOffset[1] } } },
			{ "LensScale", s.lensScale },
			{ "ApertureSurface", s.apertureSurface },
			{ "ReticleSurface", s.reticleSurface },
			{ "fovAdjust", s.fovAdjust },
			//
			{ "Parallax", s.parallax },
			{ "Breathing", s.breathing }
		};
	}

	// 为ScopeProfile类型定义to_json函数
	void to_json(json& j, const ProfileVariant& v)
	{
		j = json{
			{ "Id", v.id },
			{ "Magnification", v.magnification },
			{ "Label", v.label },
			{ "ShaderData", v.shaderData },
			{ "ZoomDataOverwrite", v.zoomDataOverwrite }
		};
	}

	void to_json(json& j, const VariantSet& v)
	{
		j = json{
			{ "Enabled", v.enabled },
			{ "Continuous", v.continuous },
			{ "StepSeconds", v.stepSeconds },
			{ "NextId", v.nextId },
			{ "DefaultVariantId", v.defaultVariantId },
			{ "Variants", v.variants }
		};
	}

	void to_json(json& j, const SecondarySight& s)
	{
		j = json{
			{ "Name", s.name },
			{ "ZoomData", s.zoomData },
			{ "SuppressOptics", s.suppressOptics },
			{ "TransitionSeconds", s.transitionSeconds }
		};
	}

	void to_json(json& j, const OcclusionSettings& o)
	{
		j = json{
			{ "Enabled", o.enabled },
			{ "SphereRadius", o.sphereRadius },
			{ "SphereOffset",
				std::vector<float>{
					o.sphereOffset[0], o.sphereOffset[1], o.sphereOffset[2] } },
			{ "FrontOnly", o.frontOnly },
			{ "DisableOnSecondarySight", o.disableOnSecondarySight },
			{ "FlipFront", o.flipFront },
			{ "ExcludedShapes", o.excludedShapes }
		};
	}

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
			{ "OmodKey", f.omodKey },
			{ "scopeFrame", f.scopeFrame },
			{ "ReticleTexturePath", f.ZoomNodePath },
			//
			{ "WeaponLabel", f.weaponLabel },
			{ "ShaderData", f.shaderData },
			{ "ZoomDataOverwrite", f.zoomDataOverwrite },
			{ "Variants", f.variants },
			{ "SecondarySights", f.secondarySights },
			{ "DefaultReticleFile", f.defaultReticleFile },
			{ "CustomReticleScale", f.customReticleScale },
			{ "Occlusion", f.occlusion }
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

			// Folder layout: the file is this profile and nothing else.
			if (parsed.contains("MagnaScopeProfile")) {
				const auto keptPath = data->path;
				parsed.get_to(*data);
				data->path = keptPath;
				data->autoProfile = true;
				return;
			}

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

			// Version 3: folder-per-weapon layout, one profile.json per scope
			// attachment, with its reticle textures beside it. Identified by a
			// marker key rather than by its path, so the folder can be renamed.
			if (parsed.contains("MagnaScopeProfile")) {
				auto data = std::make_unique<ScopeProfile>(path);
				parsed.get_to(*data);
				data->autoProfile = true;
				applyAutomaticOpticsDefaults(parsed, *data);
				if (data->sourcePlugin.empty() || data->sourceFormID == 0) {
					logger::warn(
						"Skipping profile {}: missing weapon identity",
						path);
					return false;
				}
				if (data->omodKey.empty()) {
					data->omodKey = "Default";
				}
				autoProfileMap.insert_or_assign(
					{ data->sourcePlugin, data->sourceFormID, data->omodKey },
					data.get());
				ownedData.push_back(std::move(data));
				return true;
			}

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

		const auto comboThermal = data.find("ComboThermalKey");
		if (comboThermal != data.end() && comboThermal->is_string()) {
			comboThermalKey = ComboKeyToInt(comboThermal->get<std::string>());
		} else if (comboThermal != data.end() && comboThermal->is_number_integer()) {
			comboThermalKey = comboThermal->get<int>();
		} else {
			comboThermalKey = -1;
		}

		thermalKey = data.value("ThermalKey", 0);
		// Verbose logging (per-frame telemetry + hang.txt watchdog) is off by
		// default; the menu toggles it and it persists here.
		logger::g_verbose.store(
			data.value("VerboseLogging", false), std::memory_order_relaxed);
		guiKey = data.value("guiKey", 117);
		// Unbound by default. A key that does something out of the box would
		// collide with whatever the user or another mod already has bound, and
		// reticle cycling on an unexpected key is worse than a feature that
		// waits to be asked for.
		opticsKey = data.value("opticsKey", -1);

		data["RenderPassIndex"] = PassRenderIndex;
		data["EnableRenderBeforeUI"] = bEnableRenderBeforeUI;
		data["BaseRenderCount"] = baseRenderCount;
		data["ComboNVKey"] = comboNVKey;
		data["NvKey"] = nvKey;
		data["ComboThermalKey"] = comboThermalKey;
		data["ThermalKey"] = thermalKey;
		data["VerboseLogging"] =
			logger::g_verbose.load(std::memory_order_relaxed);
		data["guiKey"] = guiKey;
		data["opticsKey"] = opticsKey;

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

	void ScopeDataHandler::SetThermalHotKeyCombo(int comboKey)
	{
		comboThermalKey = comboKey;
		UpdateConfigValue("ComboThermalKey", comboKey);
	}

	void ScopeDataHandler::SetThermalHotKeyMain(unsigned int mainkeycode)
	{
		thermalKey = mainkeycode;
		UpdateConfigValue("ThermalKey", thermalKey);
	}

	void ScopeDataHandler::SetGuiKey(unsigned int mainkeycode)
	{
		guiKey = mainkeycode;
		UpdateConfigValue("guiKey", guiKey);
	}

	void ScopeDataHandler::SetVerboseLogging(bool enabled)
	{
		logger::g_verbose.store(enabled, std::memory_order_relaxed);
		UpdateConfigValue("VerboseLogging", enabled);
	}

	void ScopeDataHandler::SetOpticsKey(unsigned int mainkeycode)
	{
		opticsKey = static_cast<int>(mainkeycode);
		UpdateConfigValue("opticsKey", opticsKey);
	}

	std::vector<ScopeProfile*> ScopeDataHandler::AllAutoProfiles() const
	{
		std::vector<ScopeProfile*> result;
		result.reserve(ownedData.size());
		for (const auto& owned : ownedData) {
			if (owned && owned->autoProfile) {
				result.push_back(owned.get());
			}
		}
		return result;
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

		// Readable folder name, best effort. Fallout 4 strips EditorIDs from
		// most runtime forms, so GetFormEditorID is usually empty and the
		// weapon's full name is what actually identifies it to a human. The
		// FormID is appended by BuildProfileDirectory regardless, so two mods
		// shipping a weapon of the same name cannot collide.
		std::string weaponLabel;
		if (const auto* editorID = weapon->GetFormEditorID();
			editorID && editorID[0] != 0) {
			weaponLabel = editorID;
		}
		if (weaponLabel.empty()) {
			if (const auto fullName = weapon->GetFullName();
				fullName && fullName[0] != 0) {
				weaponLabel = fullName;
			}
		}

		const auto profilePath =
			std::filesystem::path(BuildProfileDirectory(
				weaponLabel, sourcePlugin, sourceFormID, omodKey)) /
			"profile.json";
		auto profile = std::make_unique<ScopeProfile>(profilePath.string());
		profile->keywordName = std::format("AUTO_{:08X} [{}]", sourceFormID, omodKey);
		profile->omodKey = omodKey;
		profile->weaponLabel = weaponLabel;
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
		// Every optical value below is taken from the hand-tuned SCAR-H
		// reference profile, OMOD[SCAR-H.esp:0002BE95] in
		// SCAR-H.esp_00002E1F.json, which is the look the project treats as
		// its baseline. Two deliberate exceptions, both noted where they
		// occur: the zoom data, and lensScale.
		//
		// Values that were previously guessed one at a time now come from one
		// optic that was actually tuned as a whole, so a new scope starts
		// coherent rather than as an assortment of independently plausible
		// numbers.

		// Magnification stays neutral at 1x regardless of the reference
		// profile's own zoom. STS alignment is not silently replaced by a
		// guessed FOV, and the mouse wheel can still push up to the spread
		// while aiming.
		(void)defaultMagnification;
		profile->shaderData.minZoom = 1.0F;
		profile->shaderData.maxZoom =
			profile->shaderData.minZoom * std::max(1.0F, zoomSpread);

		profile->shaderData.fishEyeStrength = 0.03F;
		profile->shaderData.fishEyePower = 0.5F;
		profile->shaderData.edgeRefractionStrength = 0.08F;
		profile->shaderData.edgeRefractionWidth = 0.265F;
		profile->shaderData.edgeChromaticAberration = 2.0F;
		profile->shaderData.sceneParallaxStrength = 2.0F;
		profile->shaderData.opticalLagStrength = 1.0F;
		profile->shaderData.imageDenoise = 0.0F;
		profile->shaderData.imageSharpen = 0.5F;
		profile->shaderData.ReticleSize = 4.0F;
		profile->shaderData.reticle_Offset[0] = 0.0F;
		profile->shaderData.reticle_Offset[1] = 0.0F;
		// One, deliberately, unlike the rest of these values. The others come
		// from the hand-tuned SCAR-H reference profile, but its 1.5 was tuned
		// against that scope's own authored reticle. STS draws the reticle at
		// the size the mesh author intended, so anything but 1.0 resizes every
		// automatically detected scope's aiming mark away from its authored
		// appearance before the user has touched a setting.
		profile->shaderData.reticleMagnification = 1.0F;
		profile->shaderData.reticleShadowStrength = 1.0F;
		profile->shaderData.reticleParallaxStrength = 0.0F;
		// Neutral placement: the sight picture starts exactly on the aperture
		// STS authored.
		profile->shaderData.lensOffset[0] = 0.0F;
		profile->shaderData.lensOffset[1] = 0.0F;
		// Close to the reference profile's 1.48, and no longer treated as an
		// exception. lensScale corrects the mask to a particular scope's mesh,
		// so it stays a per-scope adjustment -- but neutral was the wrong
		// starting point in practice: across the STS corpus the mask lands
		// consistently undersized at 1.0, and every scope needed the same
		// direction of correction before it looked right.
		//
		// The old worry was that an oversized mask paints over the scope body,
		// since a synthesized aperture has no authored silhouette to clip it.
		// This value is under the 2.0 clamp and was picked against real optics
		// rather than derived, so it errs on the safe side of that.
		profile->shaderData.lensScale = 1.54F;
		profile->shaderData.breathing.rate = 0.21F;
		profile->shaderData.breathing.sway = 0.02F;
		profile->shaderData.breathing.drift = 0.01F;
		profile->shaderData.breathing.figure = 0.25F;
		profile->shaderData.breathing.hold = 0.305F;
		profile->shaderData.breathing.pupilFollow = 1.0F;
		const float diameter = std::clamp(defaultDiameter, 64.0F, 2160.0F);
		profile->shaderData.Size[0] = diameter;
		profile->shaderData.Size[1] = diameter;
		profile->shaderData.OriSize[0] = diameter;
		profile->shaderData.OriSize[1] = diameter;
		profile->shaderData.parallax.radius = 4.0F;
		profile->shaderData.parallax.relativeFogRadius = 4.0F;
		profile->shaderData.parallax.scopeSwayAmount = 20.0F;
		profile->shaderData.parallax.maxTravel = 4.0F;
		profile->shaderData.parallax.sceneDepth = 3.0F;
		profile->shaderData.parallax.shadowDepth = 4.0F;
		profile->shaderData.parallax.imageStillness = 0.25F;
		profile->shaderData.parallax.axialBreathing = 2.0F;
		profile->shaderData.parallax.recenterSpeed = 0.6F;
		profile->shaderData.parallax.strafeLag = 0.5F;
		profile->shaderData.parallax.tubeDepth = 0.93F;

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

	void ScopeDataHandler::ForgetAutoProfile(const ScopeProfile* data)
	{
		if (!data) {
			return;
		}
		// Erase every lookup entry pointing at this profile. A weapon can reach
		// the same profile under more than one attachment key, and leaving any
		// of them behind would hand the deleted profile straight back.
		for (auto entry = autoProfileMap.begin();
			 entry != autoProfileMap.end();) {
			if (entry->second == data) {
				entry = autoProfileMap.erase(entry);
			} else {
				++entry;
			}
		}
		// The object stays in ownedData; see the header. Clearing the selection
		// is what forces the next ADS down the create path.
		if (currentData == data) {
			currentData = nullptr;
			currentPath.clear();
		}
	}

	bool ScopeDataHandler::WriteAutoProfile(ScopeProfile* data)
	{
		if (!data || !data->autoProfile) {
			return false;
		}

		try {
			// Migration is save-triggered and one-way. A profile still living in
			// the old shared-file layout is written to the folder layout on its
			// next explicit save, and the old file is deliberately left where it
			// is: nothing bulk-rewrites files the user did not ask to have
			// touched. The old file simply stops being the newest source for
			// this identity, and the reader prefers the folder layout.
			const bool migrating = !data->UsesFolderLayout();
			const std::string previousPath = data->path;
			if (migrating) {
				const auto directory = BuildProfileDirectory(
					data->weaponLabel,
					data->sourcePlugin,
					data->sourceFormID,
					data->omodKey);
				data->path =
					(std::filesystem::path(directory) / kProfileFileName)
						.string();
			}

			const std::filesystem::path outputPath(data->path);

			json fileJson = *data;
			fileJson["MagnaScopeProfile"] = 3;

			std::filesystem::create_directories(outputPath.parent_path());
			// The reticle folder is created up front so there is somewhere
			// obvious to drop textures without having to guess the name.
			std::error_code reticleError;
			std::filesystem::create_directories(
				std::filesystem::path(outputPath.parent_path()) / "reticles",
				reticleError);

			std::ofstream output(outputPath, std::ios::trunc);
			if (!output) {
				throw std::runtime_error("file could not be opened");
			}
			output << fileJson.dump(2) << '\n';

			if (migrating) {
				logger::info(
					"Migrated profile for [{}] from '{}' to '{}'; the old file "
					"was left in place and is no longer read for this scope",
					data->omodKey,
					previousPath,
					outputPath.string());
			} else {
				logger::info("Saved profile {}", outputPath.string());
			}
			return true;
		} catch (const std::exception& error) {
			logger::error(
				"Unable to save profile {}: {}", data->path, error.what());
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
