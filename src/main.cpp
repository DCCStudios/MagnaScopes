#include "FTSData.h"
#include "EyeBoxRecentering.h"
#include "ImGuiImpl.h"
#include "Settings.h"
#include <hooking.h>
using namespace RE;
using namespace BSScript;
using namespace std;

namespace Plugin
{
	static constexpr auto NAME = "MagnaScope"sv;
	static constexpr auto VERSION = REL::Version{ 0, 11, 0 };
}

namespace
{
	std::filesystem::path GetPluginDirectory()
	{
		HMODULE module = nullptr;
		if (!GetModuleHandleExW(
				GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&GetPluginDirectory),
				&module)) {
			return {};
		}

		std::wstring buffer(32768, L'\0');
		const DWORD length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
		if (length == 0 || length >= buffer.size()) {
			return {};
		}
		buffer.resize(length);
		return std::filesystem::path(buffer).parent_path();
	}

	float ReadIniFloat(
		const std::filesystem::path& path,
		const wchar_t* key,
		float fallback)
	{
		wchar_t buffer[64]{};
		const std::wstring fallbackText = std::to_wstring(fallback);
		GetPrivateProfileStringW(
			L"AutoSTS",
			key,
			fallbackText.c_str(),
			buffer,
			static_cast<DWORD>(std::size(buffer)),
			path.c_str());
		wchar_t* parseEnd = nullptr;
		const float value = std::wcstof(buffer, &parseEnd);
		return parseEnd != buffer ? value : fallback;
	}

}

namespace MagnaScope
{
	void Settings::Load()
	{
		const auto path = GetPluginDirectory() / L"MagnaScope.ini";
		verificationStage = std::clamp<std::uint32_t>(
			GetPrivateProfileIntW(
				L"Diagnostics",
				L"VerificationStage",
				0,
				path.c_str()),
			0U,
			5U);
		verificationAuxiliaryPassThroughHooks =
			GetPrivateProfileIntW(
				L"Diagnostics",
				L"AuxiliaryPassThroughHooks",
				0,
				path.c_str()) != 0;
		verificationAuxiliaryObservationHooks =
			GetPrivateProfileIntW(
				L"Diagnostics",
				L"AuxiliaryObservationHooks",
				0,
				path.c_str()) != 0;
		verificationAuxiliaryWorldPass =
			GetPrivateProfileIntW(
				L"Diagnostics",
				L"AuxiliaryWorldPass",
				0,
				path.c_str()) != 0;
		verificationCameraOverride =
			GetPrivateProfileIntW(
				L"Diagnostics",
				L"CameraOverride",
				0,
				path.c_str()) != 0;
		verificationTaaCapture =
			GetPrivateProfileIntW(
				L"Diagnostics",
				L"TAACapture",
				0,
				path.c_str()) != 0;
		verificationVisualProbe =
			GetPrivateProfileIntW(
				L"Diagnostics",
				L"VisualProbe",
				0,
				path.c_str()) != 0;
		verificationGeometryProbe =
			GetPrivateProfileIntW(
				L"Diagnostics",
				L"GeometryProbe",
				0,
				path.c_str()) != 0;
		verificationGeometryMagnification =
			GetPrivateProfileIntW(
				L"Diagnostics",
				L"GeometryMagnification",
				0,
				path.c_str()) != 0;
		autoSTS = GetPrivateProfileIntW(L"AutoSTS", L"Enabled", 1, path.c_str()) != 0;
		defaultMaskDiameter = ReadIniFloat(path, L"DefaultMaskDiameter", 700.0F);
		defaultMagnification = ReadIniFloat(path, L"DefaultMagnification", 1.0F);
		zoomSpread = ReadIniFloat(path, L"ZoomSpread", 1.5F);
		logger::info(
			"Config loaded from {}: verification stage={}, auxiliary pass-through hooks={}, auxiliary observation hooks={}, auxiliary world pass={}, camera override={}, TAA capture={}, visual probe={}, geometry probe={}, geometry magnification={}, AutoSTS={}, diameter={}, magnification={}, zoom spread={}",
			path.string(),
			verificationStage,
			verificationAuxiliaryPassThroughHooks,
			verificationAuxiliaryObservationHooks,
			verificationAuxiliaryWorldPass,
			verificationCameraOverride,
			verificationTaaCapture,
			verificationVisualProbe,
			verificationGeometryProbe,
			verificationGeometryMagnification,
			autoSTS,
			defaultMaskDiameter,
			defaultMagnification,
			zoomSpread);
	}

	void Settings::Save() const
	{
		const auto path = GetPluginDirectory() / L"MagnaScope.ini";
		WritePrivateProfileStringW(
			L"Diagnostics",
			L"VerificationStage",
			std::to_wstring(verificationStage).c_str(),
			path.c_str());
		WritePrivateProfileStringW(
			L"Diagnostics",
			L"AuxiliaryPassThroughHooks",
			verificationAuxiliaryPassThroughHooks ? L"1" : L"0",
			path.c_str());
		WritePrivateProfileStringW(
			L"Diagnostics",
			L"AuxiliaryObservationHooks",
			verificationAuxiliaryObservationHooks ? L"1" : L"0",
			path.c_str());
		WritePrivateProfileStringW(
			L"Diagnostics",
			L"AuxiliaryWorldPass",
			verificationAuxiliaryWorldPass ? L"1" : L"0",
			path.c_str());
		WritePrivateProfileStringW(
			L"Diagnostics",
			L"CameraOverride",
			verificationCameraOverride ? L"1" : L"0",
			path.c_str());
		WritePrivateProfileStringW(
			L"Diagnostics",
			L"TAACapture",
			verificationTaaCapture ? L"1" : L"0",
			path.c_str());
		WritePrivateProfileStringW(
			L"Diagnostics",
			L"VisualProbe",
			verificationVisualProbe ? L"1" : L"0",
			path.c_str());
		WritePrivateProfileStringW(
			L"Diagnostics",
			L"GeometryProbe",
			verificationGeometryProbe ? L"1" : L"0",
			path.c_str());
		WritePrivateProfileStringW(
			L"Diagnostics",
			L"GeometryMagnification",
			verificationGeometryMagnification ? L"1" : L"0",
			path.c_str());
		WritePrivateProfileStringW(L"AutoSTS", L"Enabled", autoSTS ? L"1" : L"0", path.c_str());
		WritePrivateProfileStringW(
			L"AutoSTS", L"DefaultMaskDiameter", std::to_wstring(defaultMaskDiameter).c_str(), path.c_str());
		WritePrivateProfileStringW(
			L"AutoSTS", L"DefaultMagnification", std::to_wstring(defaultMagnification).c_str(), path.c_str());
		WritePrivateProfileStringW(
			L"AutoSTS", L"ZoomSpread", std::to_wstring(zoomSpread).c_str(), path.c_str());
		logger::info(
			"Config saved: verification stage={}, auxiliary pass-through hooks={}, auxiliary observation hooks={}, auxiliary world pass={}, camera override={}, TAA capture={}, visual probe={}, geometry probe={}, geometry magnification={}, AutoSTS={}, diameter={}, magnification={}, zoom spread={}",
			verificationStage,
			verificationAuxiliaryPassThroughHooks,
			verificationAuxiliaryObservationHooks,
			verificationAuxiliaryWorldPass,
			verificationCameraOverride,
			verificationTaaCapture,
			verificationVisualProbe,
			verificationGeometryProbe,
			verificationGeometryMagnification,
			autoSTS,
			defaultMaskDiameter,
			defaultMagnification,
			zoomSpread);
	}

	Settings& GetSettings()
	{
		static Settings instance;
		return instance;
	}
}

namespace
{
	MagnaScope::Settings& settings = MagnaScope::GetSettings();
}

bool bNeedToUpdateFTSData = true;
bool bChangeAnimFlag = false;
bool nvgFlag = false;
bool hasCombo = false;
bool hasNvgCommit = false;
bool InGameFlag = false;
bool IsHooked = false;
bool bHasStartedScope = false;
bool testingFlag = false;
bool hasUpdateSighted = false;
bool hasEjectShellCasing = false;

bool bEnableScope = false;
float gameDeltaZoom = 1;
float scopeTimer = 0;

//ReshadeImpl::Impl* reshadeImpl;
ScopeData::ScopeDataHandler* sdh;
PlayerCharacter* player;
PlayerCamera* pcam;

RE::BGSKeyword* an_45;
RE::BGSKeyword* AnimsXM2010_scopeKH45;
RE::BGSKeyword* AnimsXM2010_scopeKM;
RE::BGSKeyword* AX50_toounScope_K;
RE::BGSKeyword* AnimsAX50_scopeKH45;
RE::BGSKeyword* QMW_AnimsQBZ191M_on;
RE::BGSKeyword* QMW_AnimsQBZ191M_off;
RE::BGSKeyword* QMW_AnimsRU556M_on;
RE::BGSKeyword* QMW_AnimsRU556M_off;
RE::BGSKeyword* Tull_SideAimKeyword;
RE::BGSKeyword* Tull_SupportKeyword;
RE::BGSKeyword* AX50_toounScope_L;
RE::BGSKeyword* AnimsAX50_scopeK;

RE::NiAVObject* scopeNode;
RE::NiAVObject* camNode;
RE::NiAVObject* weaponNode;
RE::NiAVObject* rootNode;
Hook::D3D* hookIns;

struct STSApertureSelection
{
	// The mask belongs to the front optical plane. STS authors normally use
	// ScopeFade:0 for this purpose, so its transform supplies the screen
	// center and depth even when the reticle is authored farther down the tube.
	RE::NiAVObject* opticalPlane{ nullptr };
	// STS guarantees ScopeFade but does not guarantee a filled glass surface.
	// MagnaScope therefore identifies the required ScopeFade draw and expands
	// its standardized annular topology into a filled disc on the render
	// thread. The authored object remains under ScopeAiming, so Fallout owns
	// its animation, culling, depth, and draw order.
	RE::NiAVObject* renderSurface{ nullptr };
	// The reticle remains a separate aim reference. Its authored offset is not
	// forced to screen center and will later drive the magnified sample origin.
	RE::NiAVObject* aimReference{ nullptr };
	// ReticleNode is a subtree in STS meshes. Capture every renderable child,
	// because glow, markings, and recoil variants may be separate draw calls.
	std::vector<RE::NiAVObject*> reticleSurfaces;
	RE::NiAVObject* extentReference{ nullptr };
	RE::NiPoint3 worldCenter{};
	RE::NiPoint3 previousWorldCenter{};
	RE::NiPoint3 aimWorldCenter{};
	float worldRadius{ 0.0F };
};

struct AutomaticSTSTrackingState
{
	const RE::NiAVObject* aperture{ nullptr };
	std::uint32_t adsSamples{ 0 };
	float activationSeconds{ 0.0F };
	bool loggedReady{ false };
};

AutomaticSTSTrackingState automaticSTSTracking{};

struct AutomaticSTSEyeBoxTrackingState
{
	// These pointers are identity tokens only. They are never dereferenced
	// after the current frame, so a scene-graph rebuild cannot turn the
	// calibration state into a stale-pointer read.
	const RE::NiAVObject* apertureIdentity{ nullptr };
	const void* profileIdentity{ nullptr };
	RE::NiPoint3 baselineEyeLocal{};
	RE::NiPoint3 candidateEyeLocal{};
	RE::NiPoint3 previousEyeLocal{};
	Hook::D3D::PhysicalEyeBoxSample lastValidSample{};
	float stableSeconds{ 0.0F };
	bool hasCandidate{ false };
	bool hasPrevious{ false };
	bool baselineReady{ false };
	bool hasLastValidSample{ false };
};

AutomaticSTSEyeBoxTrackingState automaticSTSEyeBoxTracking{};

void ResetAutomaticSTSEyeBoxTracking()
{
	automaticSTSEyeBoxTracking = {};
}

void ResetAutomaticSTSProjectionTracking()
{
	automaticSTSTracking = {};
	if (hookIns) {
		hookIns->InvalidateLensProjection();
	}
}

void InvalidateAutomaticSTSSelection()
{
	// A projected aperture can temporarily cross the near plane during recoil
	// even though the equipped weapon, scope attachment, and renderer buffers
	// remain valid. Keep that transient condition separate from selection
	// invalidation: the render hook still needs the published ScopeFade
	// identity in order to recognize the authored draw on the same frame.
	ResetAutomaticSTSProjectionTracking();
	ResetAutomaticSTSEyeBoxTracking();
	if (hookIns) {
		hookIns->InvalidateAutomaticSTSGeometry();
	}
}

float UpdateAutomaticSTSTracking(
	const RE::NiAVObject* aperture,
	const Hook::D3D::ScreenSphereProjection& projection,
	bool apertureVisible,
	float deltaSeconds)
{
	if (!aperture || !projection.valid) {
		ResetAutomaticSTSProjectionTracking();
		return 0.0F;
	}

	auto& state = automaticSTSTracking;
	if (state.aperture != aperture) {
		state = {};
		state.aperture = aperture;
	}

	constexpr float kActivationDurationSeconds = 0.12F;
	const auto activationProgress = [&]() {
		const float linearProgress = std::clamp(
			state.activationSeconds / kActivationDurationSeconds,
			0.0F,
			1.0F);
		return linearProgress * linearProgress *
			(3.0F - 2.0F * linearProgress);
	};

	// ADS input arms the renderer before Fallout has finished raising the
	// weapon, but the optical blend does not begin until ScopeAiming actually
	// submits its required ScopeFade draw. Once activation has started, retain
	// its monotonic value across a transient missing previous-frame visibility
	// sample. Returning zero here caused a one-frame center and 1x pop during
	// sharp motion even though the current exact ScopeFade draw remained valid.
	if (!apertureVisible) {
		return activationProgress();
	}

	++state.adsSamples;
	state.activationSeconds +=
		std::clamp(deltaSeconds, 0.0F, 0.05F);

	// The geometry itself always follows the current draw with no positional
	// interpolation. Only optical power and effects ease in, which makes the
	// lens begin as ordinary 1x STS glass and reach the configured result over
	// a short, frame-rate-independent transition.
	const float easedProgress = activationProgress();

	if (!state.loggedReady && easedProgress >= 1.0F) {
		state.loggedReady = true;
		logger::info(
			"Automatic STS aperture transition completed after {} ADS samples: plane={}, center=({:.2f}, {:.2f}), radii=({:.2f}, {:.2f})",
			state.adsSamples,
			aperture->name.c_str(),
			projection.center.x,
			projection.center.y,
			projection.radiusX,
			projection.radiusY);
	}
	return easedProgress;
}

bool IsFinitePoint(const RE::NiPoint3& point)
{
	return std::isfinite(point.x) &&
	       std::isfinite(point.y) &&
	       std::isfinite(point.z);
}

Hook::D3D::PhysicalEyeBoxSample UpdateAutomaticSTSEyeBoxTracking(
	const void* profileIdentity,
	RE::NiAVObject* aperture,
	RE::NiAVObject* camera,
	const RE::NiPoint3& apertureWorldCenter,
	float apertureWorldRadius,
	float firstPersonFov,
	float activationProgress,
	float deltaSeconds,
	const RE::NiPoint3& apertureScreenCenter)
{
	Hook::D3D::PhysicalEyeBoxSample result{};
	auto& state = automaticSTSEyeBoxTracking;
	const auto lastValidOrCentered = [&]() {
		if (state.apertureIdentity == aperture &&
			state.profileIdentity == profileIdentity &&
			state.hasLastValidSample) {
			return state.lastValidSample;
		}
		return result;
	};
	if (!hookIns || !profileIdentity || !aperture || !camera ||
		!IsFinitePoint(apertureWorldCenter) ||
		!std::isfinite(apertureWorldRadius) ||
		apertureWorldRadius <= 0.001F ||
		!std::isfinite(aperture->world.scale)) {
		return lastValidOrCentered();
	}

	if (state.apertureIdentity != aperture ||
		state.profileIdentity != profileIdentity) {
		// ScopeFade objects are rebuilt when a weapon or attachment changes.
		// Never carry an eye position calibrated for one optical assembly into
		// another, even if the generated profile happens to share defaults.
		state = {};
		state.apertureIdentity = aperture;
		state.profileIdentity = profileIdentity;
	}

	const float worldScale = std::abs(aperture->world.scale);
	if (!std::isfinite(worldScale) ||
		worldScale <= 0.0001F ||
		worldScale >= 10000.0F) {
		return lastValidOrCentered();
	}
	const float localRadius = apertureWorldRadius / worldScale;
	if (!std::isfinite(localRadius) ||
		localRadius <= 0.001F ||
		localRadius >= 100000.0F) {
		return lastValidOrCentered();
	}

	// STS ScopeFade's standardized 48-vertex annulus lies in local X/Z and
	// uses local Y as its optical normal. Transforming the camera into that
	// authored coordinate system makes weapon inertia, sway, and recoil show
	// up as real eye-versus-optic motion without assuming screen center or
	// reading FPGunplayOverhaul internals.
	const RE::NiTransform inverseAperture = aperture->world.Invert();
	const RE::NiPoint3 apertureLocalCenter =
		inverseAperture * apertureWorldCenter;
	const RE::NiPoint3 cameraLocal =
		inverseAperture * camera->world.translate;
	const RE::NiPoint3 eyeLocal = cameraLocal - apertureLocalCenter;
	if (!IsFinitePoint(apertureLocalCenter) ||
		!IsFinitePoint(cameraLocal) ||
		!IsFinitePoint(eyeLocal)) {
		return lastValidOrCentered();
	}

	const float boundedDeltaSeconds =
		std::clamp(deltaSeconds, 0.0F, 0.05F);
	if (!state.baselineReady) {
		// The centered pupil may already fade in during aim-in, but measured
		// offsets begin only after the weapon settles. Use velocity and elapsed
		// time rather than frame count so vanilla and FPGunplay inertia produce
		// the same calibration at 30, 60, or 144 FPS.
		if (activationProgress < 0.999F) {
			state.hasCandidate = false;
			state.hasPrevious = false;
			state.stableSeconds = 0.0F;
		} else if (!state.hasCandidate || !state.hasPrevious) {
			state.candidateEyeLocal = eyeLocal;
			state.previousEyeLocal = eyeLocal;
			state.hasCandidate = true;
			state.hasPrevious = true;
			state.stableSeconds = 0.0F;
		} else {
			const float elapsed =
				std::max(boundedDeltaSeconds, 1.0F / 240.0F);
			const float normalizedVelocity =
				(eyeLocal - state.previousEyeLocal).Length() /
				(localRadius * elapsed);
			state.previousEyeLocal = eyeLocal;
			constexpr float kMaximumCalibrationVelocity = 1.50F;
			if (!std::isfinite(normalizedVelocity) ||
				normalizedVelocity > kMaximumCalibrationVelocity) {
				state.candidateEyeLocal = eyeLocal;
				state.stableSeconds = 0.0F;
			} else {
				// Time-constant EMA rejects idle micro-motion without making
				// the baseline depend on how many frames fit in the window.
				constexpr float kCalibrationTimeConstant = 0.08F;
				const float blend =
					1.0F -
					std::exp(
						-boundedDeltaSeconds /
						kCalibrationTimeConstant);
				state.candidateEyeLocal +=
					(eyeLocal - state.candidateEyeLocal) * blend;
				state.stableSeconds += boundedDeltaSeconds;
				constexpr float kRequiredStableSeconds = 0.12F;
				if (state.stableSeconds >= kRequiredStableSeconds) {
					state.baselineEyeLocal = state.candidateEyeLocal;
					state.baselineReady = true;
					state.previousEyeLocal = eyeLocal;
					state.hasPrevious = true;
					logger::info(
						"Automatic STS physical eye-box baseline ready: "
						"eyeLocal=({:.4f}, {:.4f}, {:.4f}), "
						"localRadius={:.4f}, stableSeconds={:.3f}",
						state.baselineEyeLocal.x,
						state.baselineEyeLocal.y,
						state.baselineEyeLocal.z,
						localRadius,
						state.stableSeconds);
				}
			}
		}
	}

	if (state.baselineReady) {
		// Parallax is a transient response to relative eye/optic motion, not a
		// persistent function of the direction in which the player looks.
		// Follow a settled optic with a short time constant, but freeze the
		// baseline while recoil, sway, or weapon-inertia motion is occurring.
		// This forms a bounded high-pass response: movement exposes scope
		// shadow and shifts the scene, then a stationary aim always returns to
		// the authored optical center regardless of camera pitch or yaw.
		const float normalizedDisplacement =
			(eyeLocal - state.baselineEyeLocal).Length() /
			localRadius;
		if (!std::isfinite(normalizedDisplacement)) {
			return lastValidOrCentered();
		} else {
			const float elapsed =
				std::max(boundedDeltaSeconds, 1.0F / 240.0F);
			const RE::NiPoint3 previousEye =
				state.hasPrevious ? state.previousEyeLocal : eyeLocal;
			const float normalizedVelocity =
				(eyeLocal - previousEye).Length() /
				(localRadius * elapsed);
			state.previousEyeLocal = eyeLocal;
			state.hasPrevious = true;

			if (std::isfinite(normalizedVelocity)) {
				const float recenterBlend =
					MagnaScope::EyeBoxRecentering::CalculateBlend(
						normalizedVelocity,
						boundedDeltaSeconds,
						normalizedDisplacement);
				state.baselineEyeLocal +=
					(eyeLocal - state.baselineEyeLocal) *
					recenterBlend;
			}
		}
	}

	// Before calibration completes the centered zero-offset pupil is still a
	// valid optical result. This removes the former one-frame pop after the
	// ADS transition; only live displacement waits for a settled baseline.
	const RE::NiPoint3 deltaLocal =
		state.baselineReady ?
			eyeLocal - state.baselineEyeLocal :
			RE::NiPoint3{};
	float normalizedX = deltaLocal.x / localRadius;
	float normalizedY = deltaLocal.z / localRadius;
	float normalizedRelief =
		state.baselineReady ?
			(std::abs(eyeLocal.y) -
			 std::abs(state.baselineEyeLocal.y)) /
				localRadius :
			0.0F;
	if (!std::isfinite(normalizedX) ||
		!std::isfinite(normalizedY) ||
		!std::isfinite(normalizedRelief)) {
		return lastValidOrCentered();
	}
	// A sharp but finite drag remains a valid optical sample. Clamp the
	// published pupil travel instead of invalidating it, which formerly made
	// the shader snap to its zero-offset fallback for one frame.
	const float maximumTravel = std::clamp(
		Hook::D3D::scopeEyeBoxMaxTravel.load(std::memory_order_acquire),
		0.0F,
		4.0F);
	const float planarLength =
		std::sqrt(normalizedX * normalizedX + normalizedY * normalizedY);
	if (planarLength > maximumTravel && planarLength > 0.0001F) {
		const float scale = maximumTravel / planarLength;
		normalizedX *= scale;
		normalizedY *= scale;
	}
	normalizedRelief =
		std::clamp(normalizedRelief, -maximumTravel, maximumTravel);

	// Project one physical aperture radius along each in-plane local axis.
	// The resulting pixel vectors preserve roll, perspective, off-center
	// authoring, and non-square render surfaces. A later shader can multiply
	// the normalized local displacement by these vectors without inventing a
	// screen-space orientation.
	const RE::NiPoint3 xAxisWorld =
		aperture->world *
		(apertureLocalCenter + RE::NiPoint3{ localRadius, 0.0F, 0.0F });
	const RE::NiPoint3 zAxisWorld =
		aperture->world *
		(apertureLocalCenter + RE::NiPoint3{ 0.0F, 0.0F, localRadius });
	const RE::NiPoint3 xAxisScreen =
		hookIns->WorldPointToScreen(camera, xAxisWorld, firstPersonFov);
	const RE::NiPoint3 zAxisScreen =
		hookIns->WorldPointToScreen(camera, zAxisWorld, firstPersonFov);
	if (!IsFinitePoint(apertureScreenCenter) ||
		!IsFinitePoint(xAxisScreen) ||
		!IsFinitePoint(zAxisScreen) ||
		xAxisScreen.z <= 0.001F ||
		zAxisScreen.z <= 0.001F) {
		return lastValidOrCentered();
	}

	const float basisXX = xAxisScreen.x - apertureScreenCenter.x;
	const float basisXY = xAxisScreen.y - apertureScreenCenter.y;
	const float basisZX = zAxisScreen.x - apertureScreenCenter.x;
	const float basisZY = zAxisScreen.y - apertureScreenCenter.y;
	const float xBasisLength =
		std::sqrt(basisXX * basisXX + basisXY * basisXY);
	const float zBasisLength =
		std::sqrt(basisZX * basisZX + basisZY * basisZY);
	if (!std::isfinite(xBasisLength) ||
		!std::isfinite(zBasisLength) ||
		xBasisLength <= 0.01F ||
		zBasisLength <= 0.01F ||
		xBasisLength > 100000.0F ||
		zBasisLength > 100000.0F) {
		return lastValidOrCentered();
	}

	result.eyeOffsetX = normalizedX;
	result.eyeOffsetY = normalizedY;
	result.eyeReliefDelta = normalizedRelief;
	result.lensBasisXX = basisXX;
	result.lensBasisXY = basisXY;
	result.lensBasisZX = basisZX;
	result.lensBasisZY = basisZY;
	result.blend = std::clamp(activationProgress, 0.0F, 1.0F);
	result.valid = true;
	state.lastValidSample = result;
	state.hasLastValidSample = true;
	return result;
}

bool IsDescendantOf(
	const RE::NiAVObject* object,
	const RE::NiAVObject* expectedAncestor)
{
	// A malformed scene graph must fail closed instead of letting an
	// unbounded parent walk stall the main thread.
	for (std::size_t depth = 0; object && depth < 128; ++depth) {
		if (object == expectedAncestor) {
			return true;
		}
		object = object->parent;
	}
	return false;
}

std::vector<RE::NiAVObject*> FindSTSReticleSurfaces(
	RE::NiAVObject* scopeViewParts)
{
	std::vector<RE::NiAVObject*> result;
	if (!scopeViewParts) {
		return result;
	}

	struct PendingObject
	{
		RE::NiAVObject* object{ nullptr };
		bool insideReticleSubtree{ false };
	};
	std::vector<PendingObject> pending{
		{ scopeViewParts, false }
	};
	// A damaged or unexpectedly cyclic NIF must not hold the game thread.
	constexpr std::size_t kMaximumVisitedObjects = 512U;
	for (std::size_t cursor = 0;
		cursor < pending.size() && cursor < kMaximumVisitedObjects;
		++cursor) {
		auto* object = pending[cursor].object;
		if (!object) {
			continue;
		}
		const std::string_view name{ object->name.c_str() };
		const bool insideReticleSubtree =
			pending[cursor].insideReticleSubtree ||
			name.find("Reticle") != std::string_view::npos;
		if (insideReticleSubtree) {
			if (auto* shape = object->IsTriShape();
				shape && shape->rendererData &&
				std::find(result.begin(), result.end(), object) == result.end()) {
				result.push_back(object);
			}
		}
		if (auto* node = object->IsNode()) {
			for (auto& childPointer : node->children) {
				if (auto* child = childPointer.get()) {
					pending.push_back({ child, insideReticleSubtree });
				}
			}
		}
	}
	return result;
}

STSApertureSelection FindSTSAperture(RE::NiAVObject* firstPersonRoot)
{
	if (!firstPersonRoot) {
		return {};
	}

	auto* scopeAiming =
		firstPersonRoot->GetObjectByName("ScopeAiming");
	auto* scopeViewPartsObject =
		firstPersonRoot->GetObjectByName("ScopeViewParts");
	auto* scopeViewParts =
		scopeViewPartsObject ? scopeViewPartsObject->IsNode() : nullptr;
	if (!scopeAiming || !scopeViewParts ||
		!IsDescendantOf(scopeViewParts, scopeAiming)) {
		return {};
	}

	const auto validBound = [](const RE::NiAVObject* object) {
		if (!object) {
			return false;
		}
		const auto& bound = object->worldBound;
		return IsFinitePoint(bound.center) &&
		       std::isfinite(bound.fRadius) &&
		       bound.fRadius > 0.001F &&
		       bound.fRadius < 100000.0F;
	};

	// ScopeFade is the required STS optical plane. Do not substitute optional
	// Glass, Lens, ScreenWarp, or EdgeBlur shapes here: Stage 4c proved those
	// differ between scope NIFs. ScopeFade supplies the one guaranteed authored
	// transform, depth, visibility branch, and renderer property contract.
	auto* opticalPlane = scopeViewParts->GetObjectByName("ScopeFade:0");
	if (!opticalPlane ||
		!IsDescendantOf(opticalPlane, scopeViewParts) ||
		!validBound(opticalPlane)) {
		return {};
	}

	auto* scopeFade = opticalPlane->IsTriShape();
	if (!scopeFade || scopeFade->numTriangles != 48U ||
		scopeFade->numVertices != 48U || !scopeFade->rendererData) {
		logger::error(
			"Automatic STS ScopeFade topology is unsupported: "
			"triangles={}, vertices={}, rendererData={}",
			scopeFade ? scopeFade->numTriangles : 0U,
			scopeFade ? scopeFade->numVertices : 0U,
			scopeFade ? scopeFade->rendererData != nullptr : false);
		return {};
	}
	RE::NiAVObject* renderSurface = scopeFade;
	const auto& planeBound = opticalPlane->worldBound;
	const float planeRadius = planeBound.fRadius;

	auto reticleSurfaces = FindSTSReticleSurfaces(scopeViewParts);
	RE::NiAVObject* aimReference =
		scopeViewParts->GetObjectByName("ReticleNode");
	if (!aimReference || !IsDescendantOf(aimReference, scopeViewParts) ||
		!validBound(aimReference)) {
		aimReference = scopeViewParts->GetObjectByName("Reticle:0");
	}
	if (!aimReference ||
		!IsDescendantOf(aimReference, scopeViewParts) ||
		!validBound(aimReference)) {
		aimReference = opticalPlane;
	}

	RE::NiAVObject* extentReference = nullptr;
	for (const char* extentName :
		{ "Glass:0", "ScreenWarp:0", "EdgeBlur:0" }) {
		auto* candidate = scopeViewParts->GetObjectByName(extentName);
		if (!candidate ||
			!IsDescendantOf(candidate, scopeViewParts) ||
			!validBound(candidate)) {
			continue;
		}
		// Reject tiny decorative layers. A usable glass extent must be wider
		// than the normalized ScopeFade plane in the live scene graph.
		if (candidate->worldBound.fRadius >
			opticalPlane->worldBound.fRadius * 1.35F) {
			extentReference = candidate;
			break;
		}
	}

	// Many weapon-mod STS meshes, including the test Elcan, omit Glass:0.
	// Their direct ScopeAiming child ending in _STS is the authored aiming
	// housing. Its 3D sphere includes tube depth, so use a conservative planar
	// fraction and clamp it to the normalized fade-plane convention.
	if (!extentReference) {
		if (auto* scopeAimingObject =
				firstPersonRoot->GetObjectByName("ScopeAiming")) {
			if (auto* scopeAimingNode = scopeAimingObject->IsNode()) {
				for (auto& childPointer : scopeAimingNode->children) {
					auto* child = childPointer.get();
					if (!child || !validBound(child)) {
						continue;
					}
					const std::string_view childName{
						child->name.c_str()
					};
					if (childName.find("_STS") !=
						std::string_view::npos) {
						extentReference = child;
						break;
					}
				}
			}
		}
	}

	float apertureRadius = planeRadius * 3.0F;
	if (extentReference) {
		const bool isAimingHousing =
			std::string_view{ extentReference->name.c_str() }.find("_STS") !=
			std::string_view::npos;
		apertureRadius = extentReference->worldBound.fRadius *
		                 (isAimingHousing ? 0.82F : 0.94F);
		apertureRadius = std::clamp(
			apertureRadius,
			planeRadius * 1.5F,
			planeRadius * 4.0F);
	} else {
		// ScopeFade uses the same approximately unit-radius source geometry
		// across the inspected STS corpus. Three live radii closely match the
		// usable opening while leaving the metal rim outside the mask.
		apertureRadius = planeRadius * 3.0F;
	}

	const RE::NiPoint3 localBoundCenter =
		opticalPlane->world.Invert() * planeBound.center;
	RE::NiPoint3 previousWorldCenter =
		opticalPlane->previousWorld * localBoundCenter;
	if (!IsFinitePoint(previousWorldCenter)) {
		previousWorldCenter = planeBound.center;
	}

	return {
		opticalPlane,
		renderSurface,
		aimReference,
		std::move(reticleSurfaces),
		extentReference,
		planeBound.center,
		previousWorldCenter,
		aimReference->worldBound.center,
		apertureRadius
	};
}

RE::NiNode* scopeNormalNode3rd_i;
RE::NiNode* scopeAimingNode3rd_i;

RE::NiNode* scopeNormalNode_i;
RE::NiNode* scopeAimingNode_i;
RE::TESObjectWEAP::InstanceData* weaponInstanceData;
RE::TESObjectWEAP::InstanceData* lastEquippedInstance = nullptr;
std::string lastAttachmentKey;
bool hasScopeSelectionSnapshot = false;
RE::BGSZoomData::Data originalZoomData;
RE::TESObjectWEAP::InstanceData* originalZoomInstance = nullptr;
RE::BSTSmartPointer<RE::TBO_InstanceData> originalZoomInstanceOwner;
// The authored or generated zoom object used by the selected instance.
// Keep this pointer stable because aim and animation systems may cache it.
RE::BGSZoomData* originalZoomForm = nullptr;
bool hasOriginalZoomData = false;
bool selectedZoomOverrideApplied = false;
bool selectedCameraOverrideApplied = false;
bool zoomOverrideSuspendedForSave = false;
std::uint64_t zoomSelectionRevision = 0;

struct EquippedWeaponSnapshot
{
	RE::TESObjectWEAP* weapon = nullptr;
	RE::BSTSmartPointer<RE::TBO_InstanceData> instanceOwner;

	[[nodiscard]] RE::TESObjectWEAP::InstanceData* GetInstance() const noexcept
	{
		return static_cast<RE::TESObjectWEAP::InstanceData*>(
			instanceOwner.get());
	}

	[[nodiscard]] explicit operator bool() const noexcept
	{
		return weapon != nullptr && instanceOwner != nullptr;
	}
};

// equippedItems is owned by MiddleHighProcessData and may be rebuilt during
// equip transitions. Copy the engine smart pointer while holding the documented
// lock, then inspect the instance only after the lock has been released.
[[nodiscard]] EquippedWeaponSnapshot GetEquippedWeaponSnapshot(
	const RE::PlayerCharacter* actor)
{
	EquippedWeaponSnapshot snapshot;
	if (!actor || !actor->currentProcess ||
		!actor->currentProcess->middleHigh) {
		return snapshot;
	}

	auto* middleHigh = actor->currentProcess->middleHigh;
	RE::BSAutoLock lock{ middleHigh->equippedItemsLock };
	for (const auto& equipped : middleHigh->equippedItems) {
		if (!equipped.item.object ||
			equipped.item.object->formType != RE::ENUM_FORM_ID::kWEAP ||
			!equipped.item.instanceData) {
			continue;
		}

		snapshot.weapon =
			static_cast<RE::TESObjectWEAP*>(equipped.item.object);
		snapshot.instanceOwner = equipped.item.instanceData;
		break;
	}
	return snapshot;
}

PlayerControls* pc;
HMODULE Upscaler;

NiPoint4 lastPosition;
NiPoint4 currPosition;
//float* ptr_deltaTime;
BSTimer* uiTimer;
//float deltaTime;

REL::Relocation<uintptr_t> ptr_PCUpdateMainThread{ REL::ID(633524), 0x22D };
uintptr_t PCUpdateMainThreadOrig;

BGSKeyword* ChangeAnimFlavorKeyword = nullptr;
ScopeData::FTSData* currentData;
const char* customPath = "Data\\F4SE\\Plugins\\FTS";

using namespace Hook;

ImGuiImpl::ImGuiImplClass* imgui_Impl;

bool isUpdateContext = false;

float timerA = 0;
bool bFirstTimeZoomData = false;

template <class Ty>
Ty SafeWrite64Function(uintptr_t addr, Ty data)
{
	DWORD oldProtect;
	void* _d[2];
	memcpy(_d, &data, sizeof(data));
	size_t len = sizeof(_d[0]);

	VirtualProtect((void*)addr, len, PAGE_EXECUTE_READWRITE, &oldProtect);
	Ty olddata;
	memset(&olddata, 0, sizeof(Ty));
	memcpy(&olddata, (void*)addr, len);
	memcpy((void*)addr, &_d[0], len);
	VirtualProtect((void*)addr, len, oldProtect, &oldProtect);
	return olddata;
}

TESForm* GetFormFromMod(std::string modname, uint32_t formid)
{
	if (!modname.length() || !formid)
		return nullptr;
	TESDataHandler* dh = TESDataHandler::GetSingleton();
	TESFile* modFile = nullptr;
	for (auto it = dh->files.begin(); it != dh->files.end(); ++it) {
		TESFile* f = *it;
		if (strcmp(f->filename, modname.c_str()) == 0) {
			modFile = f;
			break;
		}
	}
	if (!modFile)
		return nullptr;
	uint8_t modIndex = modFile->compileIndex;
	uint32_t id = formid;
	if (modIndex < 0xFE) {
		id |= ((uint32_t)modIndex) << 24;
	} else {
		uint16_t lightModIndex = modFile->smallFileCompileIndex;
		if (lightModIndex != 0xFFFF) {
			id |= 0xFE000000 | (uint32_t(lightModIndex) << 12);
		}
	}
	return TESForm::GetFormByID(id);
}

DWORD StartHooking(LPVOID)
{
	//Sleep(100);
	hookIns = Hook::D3D::GetSington();
	//hookIns->Hook();

	imgui_Impl = ImGuiImpl::ImGuiImplClass::GetSington();
	return 0;
}

bool IssueChangeAnim(std::monostate, BGSKeyword* keyword)
{
	bChangeAnimFlag = true;
	ChangeAnimFlavorKeyword = keyword;

	return true;
}

int testDegree = 0;

bool TestButton(std::monostate)
{
	/*NiPoint3 pos, dir;
	NiPoint3 right = NiPoint3(-1, 0, 0);
	player-> GetEyeVector(pos, dir, true);
	NiPoint3 heading = Normalize(NiPoint3(dir.x, dir.y, 0));

	testDegree += 5;

	NiMatrix3 rot = scopeNode->parent->parent->world.rotate * GetRotationMatrix33(heading, testDegree * toRad) * Transpose(scopeNode->parent->parent->world.rotate);
	scopeNode->parent->local.rotate = rot;*/
	//auto camStat = &BSGraphics::State::GetSingleton();
	//camStat->cameraState.camViewData.inv1stPersonProjMat = {};

	return true;
}

[[nodiscard]] std::uint32_t GetGunStateNibble(const Actor* actor) noexcept
{
	// The current CommonLib declares gunState as a signed four-bit enum
	// field. kFireSighted (8) therefore sign-extends unless the storage nibble
	// is normalized before comparison.
	return actor ?
	           (static_cast<std::uint32_t>(actor->gunState) & 0xFU) :
	           0U;
}

bool IsInADS(Actor* actor)
{
	if (!actor) {
		return false;
	}
	const auto gunState = GetGunStateNibble(actor);
	if (gunState ==
			static_cast<std::uint32_t>(GUN_STATE::kSighted) ||
		gunState ==
			static_cast<std::uint32_t>(GUN_STATE::kFireSighted)) {
		return true;
	}

	// Some firing animations transiently report kFire while the iron-sights
	// camera and ScopeAiming branch remain active. The camera state is the
	// stable optical lifecycle authority during recoil.
	const auto* camera = RE::PlayerCamera::GetSingleton();
	const auto cameraState =
		camera ? camera->GetCameraCurrentState() : nullptr;
	return cameraState &&
	       cameraState->id == RE::CameraStates::kIronSights;
}

bool IsADSInputHeld()
{
	// CommonLibF4 Pre-NG only forward-declares AttackBlockHandler. The full
	// verified layout exposes the right attack/ADS byte at +0x73 on OG as
	// documented in F4SE_Plugin_Development_Reference.md. This read is guarded
	// and used only as an early intent signal; ScopeFade's actual draw remains
	// the authority for when any lens pixels may be changed.
	const auto* controls = RE::PlayerControls::GetSingleton();
	if (!controls || !controls->attackHandler) {
		return false;
	}
	const auto* handlerBytes =
		reinterpret_cast<const std::uint8_t*>(controls->attackHandler);
	return handlerBytes[0x73] != 0;
}

bool IsADSIntentOrActive(Actor* actor)
{
	return IsADSInputHeld() || IsInADS(actor);
}

bool IsSideAim()
{
	static const BGSKeyword* sideAimKeywords[] = { an_45, AnimsXM2010_scopeKH45, AnimsXM2010_scopeKM,
		AnimsAX50_scopeKH45, Tull_SideAimKeyword,
		AX50_toounScope_K, AX50_toounScope_L,
		AnimsAX50_scopeK };
	return player && std::any_of(std::begin(sideAimKeywords), std::end(sideAimKeywords), [](const BGSKeyword* kw) { return kw && player->HasKeyword(kw); });
}

BGSKeyword* IsMagnifier()
{
	if (!player) {
		return nullptr;
	}

	const BGSKeyword* magnifierKeywords[] = {
		QMW_AnimsQBZ191M_on,
		QMW_AnimsQBZ191M_off,
		QMW_AnimsRU556M_off,
		QMW_AnimsRU556M_on
	};
	for (const auto* keyword : magnifierKeywords) {
		if (keyword && player->HasKeyword(keyword)) {
			return const_cast<BGSKeyword*>(keyword);
		}
	}
	return nullptr;
}

std::string GetEquippedAttachmentKey(const RE::PlayerCharacter* actor)
{
	if (!actor || !actor->biped) {
		return "Default";
	}

	const auto* weaponObject =
		actor->biped->GetBipObject(RE::BIPED_OBJECT::kWeaponGun);
	const auto* extra = weaponObject ? weaponObject->modExtra : nullptr;
	if (!extra || !extra->values) {
		return "Default";
	}

	const auto containsScopeToken = [](std::string_view value) {
		std::string lowered(value);
		std::ranges::transform(
			lowered,
			lowered.begin(),
			[](const unsigned char character) {
				return static_cast<char>(std::tolower(character));
			});
		return lowered.contains("scope") ||
		       lowered.contains("optic") ||
		       lowered.contains("sight");
	};

	std::vector<std::string> scopeIdentities;
	std::vector<std::string> fallbackIdentities;
	for (const auto& entry : extra->GetIndexData()) {
		if (entry.disabled) {
			continue;
		}
		auto* mod =
			RE::TESForm::GetFormByID<RE::BGSMod::Attachment::Mod>(entry.objectID);
		if (!mod) {
			continue;
		}
		const auto* file = mod->GetFile(0);
		const auto identity = std::format(
			"{}:{:08X}",
			file ? std::string(file->GetFilename()) : std::string("Fallout4.esm"),
			file ? mod->GetLocalFormID() : mod->GetFormID());
		fallbackIdentities.push_back(identity);

		// Attachment points are typed keywords authored by the weapon mod.
		// STS-compatible optics conventionally use names such as
		// ap_gun_Scope, *_Sight, or *_Optic. Prefer those OMODs so changing a
		// receiver, magazine, muzzle, or cosmetic does not fork the scope's
		// MagnaScope profile. The OMOD editor ID is checked as a secondary
		// signal for mods that use a generic/custom attach-point name.
		const auto* attachPoint =
			RE::detail::BGSKeywordGetTypedKeywordByIndex(
				RE::KeywordType::kAttachPoint,
				mod->attachPoint.keywordIndex);
		const std::string_view attachPointEditorID =
			attachPoint ? attachPoint->formEditorID.c_str() : "";
		const std::string_view modEditorID = mod->GetFormEditorID();
		if (containsScopeToken(attachPointEditorID) ||
			containsScopeToken(modEditorID)) {
			scopeIdentities.push_back(identity);
		}
	}

	// Some third-party weapons use opaque attach-point/editor IDs. Retain a
	// deterministic fallback to all installed OMODs rather than collapsing
	// them into "Default" and sharing settings between physically different
	// optics. The log identifies this fallback so a new naming convention can
	// be added from evidence instead of guessing.
	auto& identities =
		scopeIdentities.empty() ? fallbackIdentities : scopeIdentities;
	if (identities.empty()) {
		return "Default";
	}
	if (scopeIdentities.empty()) {
		static std::set<std::string> loggedFallbackKeys;
		std::vector<std::string> logIdentities = fallbackIdentities;
		std::ranges::sort(logIdentities);
		std::string logKey;
		for (const auto& identity : logIdentities) {
			if (!logKey.empty()) {
				logKey += ',';
			}
			logKey += identity;
		}
		if (loggedFallbackKeys.insert(logKey).second) {
			logger::warn(
				"No scope-like OMOD attach point was found; automatic profile identity falls back to all equipped OMODs [{}]",
				logKey);
		}
	}
	std::ranges::sort(identities);

	std::string key = "OMOD[";
	for (std::size_t index = 0; index < identities.size(); ++index) {
		if (index != 0) {
			key += ',';
		}
		key += identities[index];
	}
	key += ']';
	return key;
}

void ClearIsolatedZoomSession()
{
	if (originalZoomForm && hasOriginalZoomData) {
		// MagnaScope owns only these fields. Reticle overlay and image-space
		// modifier state may be changed by the game or another plugin while the
		// weapon is selected, so restoring the entire Data object is unsafe.
		// Restore even when the saved profile override was disabled because an
		// unsaved Menu Framework preview may still have changed these fields.
		originalZoomForm->zoomData.fovMult = originalZoomData.fovMult;
		if (selectedCameraOverrideApplied ||
			settings.AllowsCameraOverrides()) {
			originalZoomForm->zoomData.cameraOffset =
				originalZoomData.cameraOffset;
		}
	}
	if (imgui_Impl) {
		++zoomSelectionRevision;
		ImGuiImpl::PublishAuthoredZoomSnapshot(
			nullptr,
			zoomSelectionRevision);
		imgui_Impl->UpdateWeaponInstance(nullptr);
	}
	weaponInstanceData = nullptr;
	originalZoomInstanceOwner.reset();
	originalZoomInstance = nullptr;
	originalZoomForm = nullptr;
	hasOriginalZoomData = false;
	selectedZoomOverrideApplied = false;
	selectedCameraOverrideApplied = false;
	zoomOverrideSuspendedForSave = false;
}

[[nodiscard]] bool HasSelectedZoomSession() noexcept
{
	return originalZoomInstanceOwner &&
	       originalZoomInstance &&
	       originalZoomInstanceOwner.get() == originalZoomInstance &&
	       originalZoomForm &&
	       originalZoomInstance->zoomData == originalZoomForm &&
	       hasOriginalZoomData;
}

void WriteSelectedZoomOverride(const ScopeData::ZoomDataOverwrite& overrideData)
{
	if (!HasSelectedZoomSession()) {
		return;
	}

	originalZoomForm->zoomData.fovMult = overrideData.fovMul;
	if (settings.AllowsCameraOverrides()) {
		originalZoomForm->zoomData.cameraOffset = {
			overrideData.x,
			overrideData.y,
			overrideData.z
		};
		selectedCameraOverrideApplied = true;
	}
	selectedZoomOverrideApplied = true;
}

void ApplySelectedZoomOverride(const ScopeData::FTSData* profile)
{
	if (!settings.AllowsOverrides() ||
		!profile || !HasSelectedZoomSession()) {
		return;
	}

	// Match original FTS by changing the selected form's values before aim
	// starts. Never replace the pointer cached by Fallout systems.
	originalZoomForm->zoomData.fovMult = originalZoomData.fovMult;
	if (selectedCameraOverrideApplied ||
		settings.AllowsCameraOverrides()) {
		originalZoomForm->zoomData.cameraOffset =
			originalZoomData.cameraOffset;
	}
	selectedZoomOverrideApplied = false;
	selectedCameraOverrideApplied = false;
	if (profile->zoomDataOverwrite.enableZoomDateOverwrite) {
		WriteSelectedZoomOverride(profile->zoomDataOverwrite);
	}
	logger::info(
		"Verification stage {} applied selected-profile override lifecycle (enabled={}, fovMult={:.3f})",
		settings.verificationStage,
		profile->zoomDataOverwrite.enableZoomDateOverwrite,
		originalZoomForm->zoomData.fovMult);
}

void ApplySelectedEditorPreview(
	const ScopeData::ZoomDataOverwrite& preview)
{
	if (!settings.AllowsOverrides() || !HasSelectedZoomSession()) {
		return;
	}

	// Every editor frame starts from the authored baseline. Turning the
	// checkbox off therefore previews a real revert instead of leaving the
	// last enabled value latched on the shared zoom form.
	originalZoomForm->zoomData.fovMult = originalZoomData.fovMult;
	if (selectedCameraOverrideApplied ||
		settings.AllowsCameraOverrides()) {
		originalZoomForm->zoomData.cameraOffset =
			originalZoomData.cameraOffset;
	}
	selectedZoomOverrideApplied = false;
	selectedCameraOverrideApplied = false;
	if (preview.enableZoomDateOverwrite) {
		WriteSelectedZoomOverride(preview);
	}
}

[[nodiscard]] bool IsSameProfileIdentity(
	const ScopeData::FTSData& left,
	const ScopeData::FTSData& right)
{
	// Automatic profiles share one file per weapon, so path alone is not
	// enough. The attachment key keeps a delayed save from crossing into a
	// different scope OMOD after an equip change.
	return left.path == right.path &&
	       left.keywordName == right.keywordName &&
	       left.omodKey == right.omodKey &&
	       left.sourcePlugin == right.sourcePlugin &&
	       left.sourceFormID == right.sourceFormID;
}

void DetachIsolatedZoomForSave()
{
	if (!HasSelectedZoomSession()) {
		return;
	}
	originalZoomForm->zoomData.fovMult = originalZoomData.fovMult;
	if (selectedCameraOverrideApplied ||
		settings.AllowsCameraOverrides()) {
		originalZoomForm->zoomData.cameraOffset =
			originalZoomData.cameraOffset;
	}
	selectedZoomOverrideApplied = false;
	selectedCameraOverrideApplied = false;
	zoomOverrideSuspendedForSave = true;
	logger::debug(
		"Restored authored zoom fields before save serialization");
}

void ReattachIsolatedZoomAfterSave()
{
	if (!zoomOverrideSuspendedForSave) {
		return;
	}
	ApplySelectedZoomOverride(currentData);
	zoomOverrideSuspendedForSave = false;
	logger::debug("Restored zoom override after save serialization");
}

inline void InitCurrentScopeData()
{
	const auto selectProfile = [](ScopeData::FTSData* profile, bool containsAllAdditionalKeywords = true) {
		// A profile selection can replace the first-person NIF and its pooled
		// D3D suballocations. Clear the old identity before exposing the new
		// profile to the render thread.
		InvalidateAutomaticSTSSelection();
		sdh->SetCurrentFTSData(profile, containsAllAdditionalKeywords);
		currentData = profile;

		// Apply the profile before Fallout begins an aim transition without
		// replacing the BGSZoomData pointer cached by the engine.
		ApplySelectedZoomOverride(
			containsAllAdditionalKeywords ? profile : nullptr);
		bFirstTimeZoomData =
			settings.AllowsOverrides() &&
			profile != nullptr && containsAllAdditionalKeywords;
	};

	const auto clearSelection = [] {
		InvalidateAutomaticSTSSelection();
		ClearIsolatedZoomSession();
		sdh->SetCurrentFTSData(nullptr);
		currentData = nullptr;
		weaponInstanceData = nullptr;
		bFirstTimeZoomData = false;
	};

	if (!player || !player->currentProcess || !player->currentProcess->middleHigh) {
		clearSelection();
		lastEquippedInstance = nullptr;
		lastAttachmentKey.clear();
		hasScopeSelectionSnapshot = true;
		return;
	}

	const auto equipped = GetEquippedWeaponSnapshot(player);
	if (!equipped) {
		clearSelection();
		lastEquippedInstance = nullptr;
		lastAttachmentKey.clear();
		hasScopeSelectionSnapshot = true;
		return;
	}

	auto* weapon = equipped.weapon;
	auto* instance = equipped.GetInstance();
	if (!weapon || !instance || instance->type != WEAPON_TYPE::kGun) {
		clearSelection();
		lastEquippedInstance = nullptr;
		lastAttachmentKey.clear();
		hasScopeSelectionSnapshot = true;
		return;
	}

	// The strong instance reference makes restoration safe even if an equip
	// change removed the old instance from the process arrays already.
	ClearIsolatedZoomSession();

	weaponInstanceData = instance;
	lastEquippedInstance = instance;
	lastAttachmentKey = GetEquippedAttachmentKey(player);
	hasScopeSelectionSnapshot = true;
	bFirstTimeZoomData = false;
	originalZoomInstance = instance;
	originalZoomInstanceOwner = equipped.instanceOwner;
	originalZoomForm = instance->zoomData;
	hasOriginalZoomData = instance->zoomData != nullptr;
	if (hasOriginalZoomData) {
		originalZoomData = instance->zoomData->zoomData;
		++zoomSelectionRevision;
		ScopeData::ZoomDataOverwrite authoredZoom{};
		authoredZoom.enableZoomDateOverwrite = true;
		authoredZoom.fovMul = originalZoomData.fovMult;
		authoredZoom.x = originalZoomData.cameraOffset.x;
		authoredZoom.y = originalZoomData.cameraOffset.y;
		authoredZoom.z = originalZoomData.cameraOffset.z;
		ImGuiImpl::PublishAuthoredZoomSnapshot(
			&authoredZoom,
			zoomSelectionRevision);
		imgui_Impl->UpdateWeaponInstance(instance);
	} else {
		++zoomSelectionRevision;
		ImGuiImpl::PublishAuthoredZoomSnapshot(
			nullptr,
			zoomSelectionRevision);
		imgui_Impl->UpdateWeaponInstance(nullptr);
	}

	BSScrapArray<const BGSKeyword*> weaponKeywords;
	if (instance->keywords) {
		instance->keywords->CollectAllKeywords(weaponKeywords, nullptr);
	}

	constexpr std::string_view ftsPrefix = "FTS_";
	const auto explicitKeyword = std::find_if(
		weaponKeywords.begin(),
		weaponKeywords.end(),
		[](const BGSKeyword* keyword) {
			return keyword && keyword->formEditorID.size() >= ftsPrefix.size() &&
		           std::strncmp(keyword->formEditorID.c_str(), ftsPrefix.data(), ftsPrefix.size()) == 0;
		});

	if (explicitKeyword != weaponKeywords.end()) {
		auto* scopeDataMap = sdh->GetScopeDataMap();
		const std::string explicitKey((*explicitKeyword)->formEditorID.c_str());
		const auto [first, last] = scopeDataMap->equal_range(explicitKey);
		const auto* magnifierKeyword = IsMagnifier();

		ScopeData::FTSData* bestProfile = nullptr;
		std::size_t bestSpecificity = 0;
		std::size_t candidateCount = 0;
		for (auto profileIt = first; profileIt != last; ++profileIt) {
			++candidateCount;
			auto* profile = profileIt->second;
			if (!profile) {
				continue;
			}

			const bool hasAdditionalKeywords = std::ranges::all_of(
				profile->additionalKeywords,
				[instance](const std::string& keyword) {
					return instance->keywords &&
				           instance->keywords->HasKeywordString(keyword);
				});
			if (!hasAdditionalKeywords) {
				continue;
			}

			const std::string animationFlavor = magnifierKeyword ?
			                                        std::string(magnifierKeyword->formEditorID.c_str()) :
			                                        profile->animFlavorEditorID;
			const bool hasAnimationFlavor =
				animationFlavor.empty() ||
				animationFlavor == "FTS_NONE" ||
				[&animationFlavor] {
					const auto* requiredKeyword =
						TESForm::GetFormByEditorID<BGSKeyword>(animationFlavor.c_str());
					return requiredKeyword && player->HasKeyword(requiredKeyword);
				}();
			if (!hasAnimationFlavor) {
				continue;
			}

			// Multiple shipped FTS patches may share one FTS_ keyword and
			// distinguish variants with attachment or animation keywords.
			// Prefer the most constrained matching entry; stable file/load
			// order breaks ties so existing single-profile patches are
			// unchanged.
			const std::size_t specificity =
				profile->additionalKeywords.size() +
				((!profile->animFlavorEditorID.empty() &&
					 profile->animFlavorEditorID != "FTS_NONE") ?
						1U :
						0U);
			if (!bestProfile || specificity > bestSpecificity) {
				bestProfile = profile;
				bestSpecificity = specificity;
			}
		}

		if (bestProfile) {
			selectProfile(bestProfile);
			logger::info(
				"Selected explicit FTS profile {} from {} candidate(s), specificity {}",
				bestProfile->keywordName,
				candidateCount,
				bestSpecificity);
			return;
		}
		if (candidateCount > 0) {
			logger::warn(
				"No explicit FTS profile variant matched {} ({} candidate(s)); "
				"trying automatic STS discovery",
				explicitKey,
				candidateCount);
		}
	}

	// ScopeViewParts contains STS's authored aperture geometry. The geometry
	// can be deliberately off-center because weapon authors and users align it
	// to the optic. MagnaScope must preserve that transform rather than assume
	// the viewport center.
	auto* firstPersonRoot = player->firstPerson3D.get();
	const bool hasSTSAnchor = firstPersonRoot &&
	                          (firstPersonRoot->GetObjectByName("ScopeViewParts") ||
								  firstPersonRoot->GetObjectByName("ScopeAiming"));
	if (settings.autoSTS && hasOriginalZoomData && hasSTSAnchor) {
		auto* profile = sdh->GetOrCreateAutoProfile(
			weapon,
			originalZoomData,
			lastAttachmentKey,
			settings.defaultMaskDiameter,
			settings.defaultMagnification,
			settings.zoomSpread);
		selectProfile(profile);
		logger::info("Selected automatic STS profile {}", profile->keywordName);
		return;
	}

	clearSelection();
}

/// <summary>
/// The hook method is from Bingle
/// </summary>
class InputEventReceiverOverride : public BSInputEventReceiver
{
public:
	typedef void (InputEventReceiverOverride::*FnPerformInputProcessing)(const InputEvent* a_queueHead);

	//using Virtual-Key Codes
	void ProcessButtonEvent(ButtonEvent* evn)
	{
		if (!evn || evn->eventType != INPUT_EVENT_TYPE::kButton) {
			return;
		}

		uint32_t id = evn->idCode;
		if (evn->device == INPUT_DEVICE::kMouse) {
			if (hookIns && hookIns->GetRenderState() && evn->QJustPressed() &&
				!F4SEMenuFramework::IsAnyBlockingWindowOpened()) {
				// Fallout exposes wheel up and wheel down as mouse button
				// IDs 8 and 9 in the input event stream.
				if (id == 8) {
					hookIns->AdjustZoomDelta(0.1F);
				} else if (id == 9) {
					hookIns->AdjustZoomDelta(-0.1F);
				}
			}
			id += 0x100;
		}
		if (evn->device == INPUT_DEVICE::kGamepad)
			id += 0x10000;

		//if (evn->device == INPUT_DEVICE::kKeyboard && id == VK_OEM_PERIOD && evn->QJustPressed()) {
		//	std::monostate mono;
		//	//TestButton(mono);
		//}

		if (evn->device == INPUT_DEVICE::kKeyboard) {
			if (currentData) {
				if (sdh->comboNVKey == -1) {
					if (id == (uint32_t)sdh->nvKey && evn->QJustPressed()) {
						nvgFlag = !nvgFlag;
						hookIns->SetNVG((int)nvgFlag);
					}
				} else {
					if (id == (uint32_t)(sdh->comboNVKey) && evn->heldDownSecs > 0 && evn->value == 1) {
						hasCombo = true;
					}

					if (id == (uint32_t)(sdh->comboNVKey) && evn->value == 0) {
						hasCombo = false;
					}

					if (hasCombo && id == (uint32_t)sdh->nvKey && evn->QJustPressed()) {
						nvgFlag = !nvgFlag;
						hookIns->SetNVG((int)nvgFlag);
					}
				}
			}
		}
	}

	void HookedPerformInputProcessing(const InputEvent* a_queueHead)
	{
		const auto* ui = UI::GetSingleton();
		if (ui && !ui->menuMode && !ui->GetMenuOpen("LooksMenu") && !ui->GetMenuOpen("ScopeMenu") && a_queueHead) {
			for (auto* event = a_queueHead; event; event = event->next) {
				if (event->eventType == INPUT_EVENT_TYPE::kButton) {
					ProcessButtonEvent((ButtonEvent*)event);
				}
			}
		}

		if (const auto found = fnHash.find(*(uint64_t*)this); found != fnHash.end() && found->second) {
			const auto original = found->second;
			(this->*original)(a_queueHead);
		}
	}

	void HookSink()
	{
		uint64_t vtable = *(uint64_t*)this;
		auto it = fnHash.find(vtable);
		if (it == fnHash.end()) {
			FnPerformInputProcessing fn = SafeWrite64Function(vtable, &InputEventReceiverOverride::HookedPerformInputProcessing);
			fnHash.insert(std::pair<uint64_t, FnPerformInputProcessing>(vtable, fn));
		}
	}

	void UnHookSink()
	{
		uint64_t vtable = *(uint64_t*)this;
		auto it = fnHash.find(vtable);
		if (it == fnHash.end())
			return;
		SafeWrite64Function(vtable, it->second);
		fnHash.erase(it);
	}

protected:
	static unordered_map<uint64_t, FnPerformInputProcessing> fnHash;
};
unordered_map<uint64_t, InputEventReceiverOverride::FnPerformInputProcessing> InputEventReceiverOverride::fnHash;

bool IsNeedToBeCull(int indexCount = 0, int StrideCount = 0)
{
	if (!ScopeData::ScopeDataHandler::GetSingleton())
		return false;
	if (!ScopeData::ScopeDataHandler::GetSingleton()->GetCurrentFTSData())
		return false;
	if (ScopeData::ScopeDataHandler::GetSingleton()->GetCurrentFTSData()->UsingSTS)
		return false;
	if (hasUpdateSighted)
		return false;

	return true;
}

void SetNodeVisibility(NiNode* normal, NiNode* aiming, bool isScopeActive)
{
	if (normal && aiming) {
		normal->SetAppCulled(!isScopeActive);
		aiming->SetAppCulled(isScopeActive);
	}
}

void HandleScopeNode()
{
	if (!currentData || currentData->UsingSTS)
		return;

	if (player) {
		if (player->Get3D(false)) {
			scopeNormalNode3rd_i = (RE::NiNode*)RE::PlayerCharacter::GetSingleton()->Get3D(false)->GetObjectByName("ScopeNormal");
			scopeAimingNode3rd_i = (RE::NiNode*)RE::PlayerCharacter::GetSingleton()->Get3D(false)->GetObjectByName("ScopeAiming");

			if (RE::PlayerCharacter::GetSingleton()->IsInThirdPerson()) {
				SetNodeVisibility(scopeNormalNode3rd_i, scopeAimingNode3rd_i, true);
				return;
			}
		}

		if (player->firstPerson3D) {
			scopeNormalNode_i = (RE::NiNode*)RE::PlayerCharacter::GetSingleton()->firstPerson3D->GetObjectByName("ScopeNormal");
			scopeAimingNode_i = (RE::NiNode*)RE::PlayerCharacter::GetSingleton()->firstPerson3D->GetObjectByName("ScopeAiming");

			if (bEnableScope) {
				if (IsNeedToBeCull())
					SetNodeVisibility(scopeNormalNode_i, scopeAimingNode_i, false);
				else
					SetNodeVisibility(scopeNormalNode_i, scopeAimingNode_i, true);
			} else
				SetNodeVisibility(scopeNormalNode_i, scopeAimingNode_i, true);
		}
	}
}

void HookedUpdate()
{
	typedef void (*FnUpdate)();
	FnUpdate fn = (FnUpdate)PCUpdateMainThreadOrig;
	if (!fn)
		return;
	bool originalCalled = false;
	const auto callOriginal = [&] {
		if (!originalCalled) {
			(*fn)();
			originalCalled = true;
		}
	};
	if (InGameFlag && player && player->Get3D(true)) {
		// Forced aim requested by the customization menu (which renders on
		// the D3D thread); applied here on the game thread. Mirrors the
		// original FTS PlayerAim: block game keyboard/mouse processing (so
		// the engine's "aim button not held" check cannot cancel the sighted
		// state next frame), set the sighted state, and play the vanilla
		// sighted enter/exit idle so the weapon actually raises or lowers.
		const bool forceAimRequested =
			ImGuiImpl::pendingForcedAim.load(std::memory_order_relaxed) != 0;
		static bool ownsForcedAim = false;
		if (forceAimRequested && player->currentProcess) {
			// F4SE Menu Framework already owns the gameplay-input block for a
			// blocking editor window. MagnaScope only owns the sighted state.
			// Reassert it if Fallout lowers the weapon while the editor remains
			// open, instead of relying on a one-shot transition.
			if (!IsInADS(player)) {
				player->SetInIronSightsImpl(true);
				if (auto* idle =
						RE::TESForm::GetFormByID<RE::TESIdleForm>(0x0004D32u)) {
					player->currentProcess->PlayIdle(*player, idle, nullptr);
				}
				ownsForcedAim = true;
			}
		} else if (ownsForcedAim && player->currentProcess) {
			player->SetInIronSightsImpl(false);
			if (auto* idle =
					RE::TESForm::GetFormByID<RE::TESIdleForm>(0x0004AD9u)) {
				player->currentProcess->PlayIdle(*player, idle, nullptr);
			}
			ownsForcedAim = false;
		}

		if (!bHasStartedScope &&
			(!imgui_Impl ||
				!imgui_Impl->bIsSaving.load(std::memory_order_acquire))) {
			const auto equipped = GetEquippedWeaponSnapshot(player);
			auto* equippedInstance = equipped.GetInstance();

			// While the customization menu is in edit mode the profile
			// selection stays frozen: a re-init would re-baseline the
			// weapon's zoom data and stomp the menu's live preview. The one
			// exception is the equipped instance being replaced.
			const bool editing =
				hookIns &&
				hookIns->bEnableEditMode.load(std::memory_order_acquire);
			if (!editing || equippedInstance != lastEquippedInstance) {
				if (editing) {
					// The weapon changed mid-edit (possible now that the
					// editor popout stays open during gameplay). The old
					// instance may already be freed, so end the session
					// without writing to it; the re-init below rebinds the
					// editor to the new weapon.
					hookIns->bEnableEditMode.store(
						false,
						std::memory_order_release);
					ImGuiImpl::AbandonZoomPreview();
				}

				//需要优化，尝试haskeyword可不可行
				const std::string attachmentKey =
					equippedInstance ? GetEquippedAttachmentKey(player) : std::string{};
				if (!hasScopeSelectionSnapshot ||
					equippedInstance != lastEquippedInstance ||
					attachmentKey != lastAttachmentKey) {
					InitCurrentScopeData();
				}
			}
		}

		// Menu Framework callbacks only publish copied requests. Apply a save
		// only when its attachment-aware identity still matches the selected
		// profile, then serialize and reselect entirely on the game thread.
		if (auto pendingSave = ImGuiImpl::ConsumeProfileSave()) {
			if (currentData &&
				IsSameProfileIdentity(*currentData, *pendingSave)) {
				*currentData = *pendingSave;
				sdh->SetCurrentFTSData(currentData);
				sdh->WriteCurrentFTSData();
				InitCurrentScopeData();
				hookIns->bRefreshChar.store(
					true,
					std::memory_order_release);
				logger::info(
					"Saved and reapplied profile for attachment identity {}",
					currentData ? currentData->omodKey : std::string{});
			} else {
				logger::warn(
					"Ignored a stale editor save because the equipped "
					"weapon or scope attachment changed");
			}
		}

		// Reload/reselection requests likewise execute here so profile
		// mutation, instance rebinding, and BGSZoomData restoration never run
		// from the D3D callback.
		const auto profileRequest = ImGuiImpl::ConsumeProfileAction();
		if (profileRequest != ImGuiImpl::ProfileRequest::kNone) {
			if (profileRequest == ImGuiImpl::ProfileRequest::kReload &&
				currentData &&
				(!currentData->autoProfile ||
					std::filesystem::exists(currentData->path))) {
				sdh->ReloadFTSData(currentData);
			}
			InitCurrentScopeData();
			hookIns->bRefreshChar.store(true, std::memory_order_release);
		}

		static bool editorPreviewApplied = false;
		if (currentData) {
			const bool editing =
				hookIns->bEnableEditMode.load(std::memory_order_acquire);
			const auto editorPreview =
				ImGuiImpl::GetEditorPreviewSnapshot();
			if (editing &&
				editorPreview.active &&
				editorPreview.selectionRevision == zoomSelectionRevision) {
				ApplySelectedEditorPreview(editorPreview.zoomOverride);
				Hook::D3D::scopeFadeMagnification.store(
					std::clamp(
						editorPreview.magnification,
						1.0F,
						15.0F),
					std::memory_order_release);
				Hook::D3D::scopeImageDenoise.store(
					editorPreview.imageDenoise,
					std::memory_order_release);
				Hook::D3D::scopeImageSharpen.store(
					editorPreview.imageSharpen,
					std::memory_order_release);
				Hook::D3D::scopeFishEyeStrength.store(
					editorPreview.fishEyeStrength,
					std::memory_order_release);
				Hook::D3D::scopeFishEyePower.store(
					editorPreview.fishEyePower,
					std::memory_order_release);
				Hook::D3D::scopeEdgeRefractionStrength.store(
					editorPreview.edgeRefractionStrength,
					std::memory_order_release);
				Hook::D3D::scopeEdgeRefractionWidth.store(
					editorPreview.edgeRefractionWidth,
					std::memory_order_release);
				Hook::D3D::scopeEdgeChromaticAberration.store(
					editorPreview.edgeChromaticAberration,
					std::memory_order_release);
				Hook::D3D::scopeReticleMagnification.store(
					editorPreview.reticleMagnification,
					std::memory_order_release);
				Hook::D3D::scopeReticleSize.store(
					editorPreview.reticleSize,
					std::memory_order_release);
				Hook::D3D::scopeReticleOffsetX.store(
					editorPreview.reticleOffsetX,
					std::memory_order_release);
				Hook::D3D::scopeReticleOffsetY.store(
					editorPreview.reticleOffsetY,
					std::memory_order_release);
				Hook::D3D::scopeEyeBoxRadius.store(
					editorPreview.eyeBoxRadius,
					std::memory_order_release);
				Hook::D3D::scopeVignetteReach.store(
					editorPreview.vignetteReach,
					std::memory_order_release);
				Hook::D3D::scopeVignetteSharpness.store(
					editorPreview.vignetteSharpness,
					std::memory_order_release);
				Hook::D3D::scopeEyeBoxMaxTravel.store(
					editorPreview.eyeBoxMaxTravel,
					std::memory_order_release);
				Hook::D3D::scopeSceneParallaxStrength.store(
					editorPreview.sceneParallaxStrength,
					std::memory_order_release);
				Hook::D3D::scopeOpticalLagStrength.store(
					editorPreview.opticalLagStrength,
					std::memory_order_release);
				editorPreviewApplied = true;
			} else {
				if (editorPreviewApplied) {
					// Leaving edit mode without saving restores the selected
					// profile. A saved profile was already updated before its
					// game-thread reselection request reached this point.
					ApplySelectedZoomOverride(currentData);
					editorPreviewApplied = false;
				}
				Hook::D3D::scopeFadeMagnification.store(
					std::clamp(
						currentData->shaderData.minZoom,
						1.0F,
						15.0F),
					std::memory_order_release);
				Hook::D3D::scopeImageDenoise.store(
					std::clamp(
						currentData->shaderData.imageDenoise,
						0.0F,
						1.0F),
					std::memory_order_release);
				Hook::D3D::scopeImageSharpen.store(
					std::clamp(
						currentData->shaderData.imageSharpen,
						0.0F,
						1.0F),
					std::memory_order_release);
				Hook::D3D::scopeFishEyeStrength.store(
					std::clamp(
						currentData->shaderData.fishEyeStrength,
						0.0F,
						2.0F),
					std::memory_order_release);
				Hook::D3D::scopeFishEyePower.store(
					std::clamp(
						currentData->shaderData.fishEyePower,
						0.5F,
						6.0F),
					std::memory_order_release);
				Hook::D3D::scopeEdgeRefractionStrength.store(
					std::clamp(
						currentData->shaderData.edgeRefractionStrength,
						0.0F,
						0.25F),
					std::memory_order_release);
				Hook::D3D::scopeEdgeRefractionWidth.store(
					std::clamp(
						currentData->shaderData.edgeRefractionWidth,
						0.02F,
						0.5F),
					std::memory_order_release);
				Hook::D3D::scopeEdgeChromaticAberration.store(
					std::clamp(
						currentData->shaderData.edgeChromaticAberration,
						0.0F,
						2.0F),
					std::memory_order_release);
				Hook::D3D::scopeReticleMagnification.store(
					std::clamp(
						currentData->shaderData.reticleMagnification,
						0.25F,
						8.0F),
					std::memory_order_release);
				Hook::D3D::scopeReticleSize.store(
					std::clamp(
						currentData->shaderData.ReticleSize,
						0.01F,
						128.0F),
					std::memory_order_release);
				Hook::D3D::scopeReticleOffsetX.store(
					std::clamp(
						currentData->shaderData.reticle_Offset[0],
						-1000.0F,
						1000.0F),
					std::memory_order_release);
				Hook::D3D::scopeReticleOffsetY.store(
					std::clamp(
						currentData->shaderData.reticle_Offset[1],
						-1000.0F,
						1000.0F),
					std::memory_order_release);
				Hook::D3D::scopeEyeBoxRadius.store(
					std::clamp(
						currentData->shaderData.parallax.radius,
						0.01F,
						20.0F),
					std::memory_order_release);
				Hook::D3D::scopeVignetteReach.store(
					std::clamp(
						currentData->shaderData.parallax.relativeFogRadius,
						1.01F,
						20.0F),
					std::memory_order_release);
				Hook::D3D::scopeVignetteSharpness.store(
					std::clamp(
						currentData->shaderData.parallax.scopeSwayAmount,
						0.1F,
						20.0F),
					std::memory_order_release);
				Hook::D3D::scopeEyeBoxMaxTravel.store(
					std::clamp(
						currentData->shaderData.parallax.maxTravel,
						0.0F,
						4.0F),
					std::memory_order_release);
				Hook::D3D::scopeSceneParallaxStrength.store(
					std::clamp(
						currentData->shaderData.sceneParallaxStrength,
						0.0F,
						2.0F),
					std::memory_order_release);
				Hook::D3D::scopeOpticalLagStrength.store(
					std::clamp(
						currentData->shaderData.opticalLagStrength,
						0.0F,
						4.0F),
					std::memory_order_release);
			}

			if (!settings.AllowsProjection()) {
				hookIns->EnableRender(false);
				hookIns->QueryRender(false);
				InvalidateAutomaticSTSSelection();
				callOriginal();
				return;
			}

			if (!bFirstTimeZoomData) {
				if (settings.AllowsOverrides() && !editing) {
					auto tempZDO = currentData->zoomDataOverwrite;
					if (tempZDO.enableZoomDateOverwrite) {
						WriteSelectedZoomOverride(tempZDO);
					}
				}
				bFirstTimeZoomData = settings.AllowsOverrides();
			}

			// The inherited FTS hook read first-person world transforms before
			// invoking this original update call. On Fallout 4's flattened
			// first-person rig those world fields still describe the previous
			// frame, which produced the observed one-frame aperture trail.
			// Keep profile and zoom-lifecycle work before the call, but perform
			// every optical transform read after the game refreshes the rig.
			callOriginal();
			if (currentData->autoProfile) {
				hookIns->PublishAutomaticSTSGunState(
					GetGunStateNibble(player));
			}

			auto* firstPersonRoot = player->Get3D(true);
			if (!firstPersonRoot) {
				hookIns->EnableRender(false);
				hookIns->QueryRender(false);
				InvalidateAutomaticSTSSelection();
				return;
			}
			scopeNode = firstPersonRoot->GetObjectByName("FTS:CenterPoint");
			RE::NiPoint3 scopeProjectionPoint{};
			RE::NiPoint3 previousScopeProjectionPoint{};
			RE::NiPoint3 aimProjectionPoint{};
			float scopeWorldRadius = 0.0F;
			if (scopeNode) {
				scopeProjectionPoint = scopeNode->world.translate;
				previousScopeProjectionPoint =
					scopeNode->previousWorld.translate;
				aimProjectionPoint = scopeProjectionPoint;
			} else if (currentData->autoProfile) {
				const auto aperture = FindSTSAperture(firstPersonRoot);
				scopeNode = aperture.opticalPlane;
				hookIns->PublishAutomaticSTSGeometry(
					aperture.renderSurface,
					aperture.reticleSurfaces,
					aperture.extentReference);
				scopeProjectionPoint = aperture.worldCenter;
				previousScopeProjectionPoint =
					aperture.previousWorldCenter;
				aimProjectionPoint = aperture.aimWorldCenter;
				scopeWorldRadius = aperture.worldRadius;

				if (scopeNode && IsInADS(player)) {
					static RE::NiAVObject* lastLoggedAperture = nullptr;
					if (lastLoggedAperture != scopeNode) {
						lastLoggedAperture = scopeNode;
						logger::info(
							"Automatic STS aperture selected: plane={}, renderSurface={}, extent={}, aim={}, worldCenter=({:.4f}, {:.4f}, {:.4f}), worldRadius={:.4f}; ScopeFade depth and authored reticle offset are preserved separately",
							scopeNode->name.c_str(),
							aperture.renderSurface ?
								aperture.renderSurface->name.c_str() :
								scopeNode->name.c_str(),
							aperture.extentReference ?
								aperture.extentReference->name.c_str() :
								"normalized ScopeFade",
							aperture.aimReference ?
								aperture.aimReference->name.c_str() :
								scopeNode->name.c_str(),
							scopeProjectionPoint.x,
							scopeProjectionPoint.y,
							scopeProjectionPoint.z,
							scopeWorldRadius);
					}
				}
			}
			camNode = firstPersonRoot->GetObjectByName("Camera");
			pc = PlayerControls::GetSingleton();
			NiPoint3 tempOut;

			pcam = PlayerCamera::GetSingleton();

			if (scopeNode && camNode) {
				const float firstPersonFov =
					pcam ? pcam->firstPersonFOV : 90.0F;
				D3D::ScreenSphereProjection apertureProjection{};
				if (currentData->autoProfile && scopeWorldRadius > 0.0F) {
					apertureProjection =
						hookIns->ProjectWorldSphereToScreen(
							camNode,
							scopeProjectionPoint,
							scopeWorldRadius,
							firstPersonFov);
					tempOut = apertureProjection.center;
				} else {
					tempOut = hookIns->WorldPointToScreen(
						camNode,
						scopeProjectionPoint,
						firstPersonFov);
				}
				// Player position is public CommonLibF4 state. The inherited
				// implementation followed two unverified Havok offsets
				// (+0x470 and +0x40) merely to estimate translation delta.
				// Those offsets are not required for optical alignment and are
				// unsafe in a staged crash-isolation build.
				const NiPoint3 playerPosition = player->GetPosition();
				currPosition = {
					playerPosition.x,
					playerPosition.y,
					playerPosition.z,
					0.0F
				};
				NiPoint4 VirTransLerp = {
					currPosition.x - lastPosition.x,
					currPosition.y - lastPosition.y,
					currPosition.z - lastPosition.z,
					currPosition.w - lastPosition.w
				};
				lastPosition = currPosition;

				NiPoint3 virDir = scopeProjectionPoint - camNode->world.translate;
				NiPoint3 lastVirDir =
					previousScopeProjectionPoint -
					camNode->previousWorld.translate;
				NiPoint3 VirDirLerp = NiPoint3(virDir - lastVirDir);

				NiPoint3 weaponPos = scopeProjectionPoint;
				NiPoint3 rootPos = camNode->world.translate;

				D3D::GameConstBuffer gcb;

				gcb.virDir = virDir;

				gcb.lastVirDir = lastVirDir;
				gcb.VirDirLerp = VirDirLerp;
				gcb.VirTransLerp = { VirTransLerp.x, VirTransLerp.y, VirTransLerp.z };
				gcb.weaponPos = weaponPos;
				gcb.rootPos = rootPos;

				gcb.camMat = camNode->local.rotate;
				gcb.ftsLocalMat = scopeNode->local.rotate;
				gcb.ftsWorldMat = scopeNode->world.rotate;
				gcb.ftsScreenPos = tempOut;
				const bool automaticADS =
					currentData->autoProfile &&
					IsADSIntentOrActive(player);
				if (automaticADS) {
					if (apertureProjection.valid) {
						const RE::NiPoint3 aimScreenPoint =
							IsFinitePoint(aimProjectionPoint) ?
								hookIns->WorldPointToScreen(
									camNode,
									aimProjectionPoint,
									firstPersonFov) :
								tempOut;
						const float activationProgress =
							UpdateAutomaticSTSTracking(
								scopeNode,
								apertureProjection,
								Hook::D3D::
									automaticSTSScopeFadeVisibleLastFrame.load(
										std::memory_order_acquire),
								uiTimer ? uiTimer->delta : 0.0F);
						const Hook::D3D::PhysicalEyeBoxSample physicalEyeBox =
							UpdateAutomaticSTSEyeBoxTracking(
								currentData,
								scopeNode,
								camNode,
								scopeProjectionPoint,
								scopeWorldRadius,
								firstPersonFov,
								activationProgress,
								uiTimer ? uiTimer->delta : 0.0F,
								tempOut);
						hookIns->PublishLensProjection(
							tempOut.x,
							tempOut.y,
							aimScreenPoint.x,
							aimScreenPoint.y,
							apertureProjection.radiusX,
							apertureProjection.radiusY,
							true,
							activationProgress,
							physicalEyeBox);
					} else if (automaticSTSTracking.aperture != scopeNode) {
						// An invalid first sample or a rebuilt ScopeFade is a
						// real identity boundary. Reset before accepting a new
						// stable projection.
						ResetAutomaticSTSProjectionTracking();
					}
					// During recoil the CPU sphere can cross the near plane for
					// a frame while the exact ScopeFade draw remains valid.
					// Preserve the last publication and monotonic activation;
					// draw-time geometry supplies the current aperture pose.
				} else {
					ResetAutomaticSTSProjectionTracking();
					// A new ADS session establishes its own neutral pupil. Do
					// not inherit a baseline calibrated at another pitch/yaw;
					// transient same-ADS projection loss is handled above and
					// deliberately preserves this state.
					ResetAutomaticSTSEyeBoxTracking();
				}

				if (settings.AllowsRenderer() && bHasStartedScope) {
					scopeTimer += uiTimer->delta * 1000;
					if (!bHasStartedScope) {
						scopeTimer = 0;
						return;
					}
					if (scopeTimer >= sdh->GetCurrentFTSData()->scopeFrame) {
						bEnableScope = true;
						hookIns->SetScopeEffect(true);
						scopeTimer = 0;
						bHasStartedScope = false;
					}
				} else {
					scopeTimer = 0;
				}

				hookIns->SetGameConstData(gcb);
				if (settings.AllowsRenderer()) {
					HandleScopeNode();
				}

				//PauseMenu
				//WorkshopMenu
				//CursorMenu

				const auto* ui = RE::UI::GetSingleton();
				if (IsSideAim() || (ui && (ui->GetMenuOpen("PauseMenu") ||
											  ui->GetMenuOpen("WorkshopMenu") ||
											  ui->GetMenuOpen("CursorMenu")))) {
					hookIns->EnableRender(false);
					hookIns->QueryRender(false);
				} else {
					if (IsADSIntentOrActive(player)) {
						// One-shot diagnostic: reports whether this weapon aims
						// through the vanilla ScopeMenu path or STS sighted ADS,
						// and the live FOV values while the overwrite is active.
						static bool loggedAimDiagnostics = false;
						if (!loggedAimDiagnostics) {
							loggedAimDiagnostics = true;
							logger::info(
								"ADS diagnostics: ScopeMenu={}, firstPersonFOV={:.1f}, fovAdjust={:.2f}, autoProfile={}, fovMult={:.3f}",
								ui && ui->GetMenuOpen("ScopeMenu"),
								pcam ? pcam->firstPersonFOV : -1.0F,
								pcam ? pcam->fovAdjustCurrent : -1.0F,
								currentData->autoProfile,
								(weaponInstanceData && weaponInstanceData->zoomData) ?
									weaponInstanceData->zoomData->zoomData.fovMult :
									-1.0F);
						}
						if (settings.AllowsRenderer() && currentData->autoProfile) {
							// Auto profiles must not depend on weapon-specific
							// animation tags that an STS-only weapon may not emit.
							bEnableScope = true;
							hookIns->SetScopeEffect(true);
						}
						if (settings.AllowsOverrides() &&
							!hookIns->bEnableEditMode.load(
								std::memory_order_acquire)) {
							auto tempZDO = currentData->zoomDataOverwrite;
							if (tempZDO.enableZoomDateOverwrite) {
								// The original FTS reasserts these fields while
								// sighted. Keep that timing, but only through the
								// validated selected-instance transaction.
								WriteSelectedZoomOverride(tempZDO);
							}
						}

						const bool rendererAllowed = settings.AllowsRenderer();
						hookIns->EnableRender(rendererAllowed);
						hookIns->QueryRender(rendererAllowed);
					}
				}

				if (!IsADSIntentOrActive(player)) {
					hookIns->EnableRender(false);
					// Each ADS entry receives a fresh settled-eye baseline.
					// This prevents a prior stance, shoulder transition, or
					// manually aligned optic position from biasing the next
					// physical parallax session.
					ResetAutomaticSTSEyeBoxTracking();
					// No zoom restore here: the game samples fovMult when the
					// aim-in transition starts, so the override has to stay on
					// the form while at the hip. The original values return
					// when the profile is deselected (see InitCurrentScopeData),
					// exactly like the original FTS behaved.
					if (settings.AllowsRenderer() && currentData->autoProfile) {
						hookIns->SetScopeEffect(false);
						bEnableScope = false;
					}
				}
			} else {
				hookIns->EnableRender(false);
				hookIns->QueryRender(false);
				InvalidateAutomaticSTSSelection();
			}
		} else {
			// A removed or unsupported scope must not leave editor state or a
			// previous attachment's magnification latched for the next
			// selection. The renderer is disabled below, but resetting the
			// copied controls here also makes a later selection deterministic.
			if (editorPreviewApplied) {
				editorPreviewApplied = false;
			}
			ImGuiImpl::ClearEditorPreview();
			Hook::D3D::scopeFadeMagnification.store(
				1.0F,
				std::memory_order_release);
			Hook::D3D::scopeImageDenoise.store(
				0.0F,
				std::memory_order_release);
			Hook::D3D::scopeImageSharpen.store(
				0.0F,
				std::memory_order_release);
			Hook::D3D::scopeFishEyeStrength.store(
				0.0F,
				std::memory_order_release);
			Hook::D3D::scopeFishEyePower.store(
				2.0F,
				std::memory_order_release);
			Hook::D3D::scopeReticleMagnification.store(
				1.0F,
				std::memory_order_release);
			Hook::D3D::scopeReticleSize.store(
				4.0F,
				std::memory_order_release);
			Hook::D3D::scopeReticleOffsetX.store(
				0.0F,
				std::memory_order_release);
			Hook::D3D::scopeReticleOffsetY.store(
				0.0F,
				std::memory_order_release);
			Hook::D3D::scopeSceneParallaxStrength.store(
				0.0F,
				std::memory_order_release);
			Hook::D3D::scopeOpticalLagStrength.store(
				1.0F,
				std::memory_order_release);
			hookIns->EnableRender(false);
			hookIns->QueryRender(false);
			InvalidateAutomaticSTSSelection();
		}
	}

	callOriginal();
}

class EquipWatcher : public BSTEventSink<TESEquipEvent>
{
public:
	virtual BSEventNotifyControl ProcessEvent(const TESEquipEvent& evn, BSTEventSource<TESEquipEvent>* a_source)
	{
		if (!evn.actor) {
			return BSEventNotifyControl::kContinue;
		}
		Actor* a = evn.actor->As<Actor>();

		if (a == player) {
			TESForm* item = TESForm::GetFormByID(evn.baseObject);
			/*reshade::log_message(4, "" + evn.formId);
			reshade::log_message(4, "Player!");*/

			if (evn.equipped) {
			}

			if (evn.equipped && item && item->GetFormType() == ENUM_FORM_ID::kWEAP) {
				hookIns->QueryRender(false);
				sdh->SetCurrentFTSData(nullptr);
				InitCurrentScopeData();
				hookIns->SetInterfaceTextRefresh(true);
				bChangeAnimFlag = false;
				hookIns->QueryChangeReticleTexture();
				hookIns->ResetZoomDelta();
			}
		}

		return BSEventNotifyControl::kContinue;
	}
	F4_HEAP_REDEFINE_NEW(EquipWatcher);
};

using std::unordered_map;
class AnimationGraphEventWatcher
{
public:
	typedef BSEventNotifyControl (AnimationGraphEventWatcher::*FnProcessEvent)(BSAnimationGraphEvent& evn, BSTEventSource<BSAnimationGraphEvent>* dispatcher);

	BSEventNotifyControl HookedProcessEvent(BSAnimationGraphEvent& evn, BSTEventSource<BSAnimationGraphEvent>* src)
	{
		const auto foundHook = fnHash.find(*(uint64_t*)this);
		FnProcessEvent fn = foundHook != fnHash.end() ? foundHook->second : nullptr;
		string prefix = "";

		//	_MESSAGE("evn.animEvent: %s; evn.argument: %s", evn.animEvent.c_str(), evn.argument.c_str());

		if (IsInADS(player)) {
			if (!IsSideAim() && !player->IsInThirdPerson() && bNeedToUpdateFTSData) {
				if (bChangeAnimFlag) {
					InitCurrentScopeData();
				}

				currentData = sdh->GetCurrentFTSData();

				if (currentData && !bHasStartedScope) {
					hookIns->StartScope(true);
					hookIns->SetFinishAimAnim(true);
					bHasStartedScope = true;
				}

				bNeedToUpdateFTSData = false;
			}

			if (sdh->GetCurrentFTSData() && sdh->GetCurrentFTSData()->shaderData.bBoltDisable) {
				if (hasEjectShellCasing) {
					hookIns->SetScopeEffect(true);
					hasEjectShellCasing = false;
					hasUpdateSighted = false;
				}

				if (strcmp(evn.tag.c_str(), "UpdateSighted") == 0) {
					hookIns->SetScopeEffect(false);
					hasUpdateSighted = true;
				}

				if (strcmp(evn.tag.c_str(), "initiateBoltStart") == 0) {
					hookIns->SetScopeEffect(false);
					hasUpdateSighted = true;
				}

				if (hasUpdateSighted && strcmp(evn.tag.c_str(), "initiateStart") == 0) {
					hasUpdateSighted = false;
					hasEjectShellCasing = true;
				}
			}
		} else {
			hasUpdateSighted = false;
			hasEjectShellCasing = false;
			hookIns->StartScope(false);
			bNeedToUpdateFTSData = true;
			hookIns->SetScopeEffect(false);
			bHasStartedScope = false;
			bEnableScope = false;
		}

		return fn ? (this->*fn)(evn, src) : BSEventNotifyControl::kContinue;
	}

	void HookSink()
	{
		uint64_t vtable = *(uint64_t*)this;
		auto it = fnHash.find(vtable);
		if (it == fnHash.end()) {
			FnProcessEvent fn = SafeWrite64Function(vtable + 0x8, &AnimationGraphEventWatcher::HookedProcessEvent);
			fnHash.insert(std::pair<uint64_t, FnProcessEvent>(vtable, fn));
		}
	}

	void UnHookSink()
	{
		uint64_t vtable = *(uint64_t*)this;
		auto it = fnHash.find(vtable);
		if (it == fnHash.end())
			return;
		SafeWrite64Function(vtable + 0x8, it->second);
		fnHash.erase(it);
	}

protected:
	static unordered_map<uint64_t, FnProcessEvent> fnHash;
};
unordered_map<uint64_t, AnimationGraphEventWatcher::FnProcessEvent> AnimationGraphEventWatcher::fnHash;

bool RegisterFuncs(BSScript::IVirtualMachine* vm)
{
	// Keep the legacy Papyrus script name so existing FTS patches continue to
	// bind without requiring authors to rebuild their script assets.
	constexpr std::string_view fileName = "FakeThroughScope";
#ifdef _DEBUG
	vm->BindNativeMethod(fileName, "TestButton", TestButton);
#endif  // _DEBUG
	vm->BindNativeMethod(fileName, "OnChangeAnimFlavor", IssueChangeAnim);

	return true;
}

DWORD WINAPI MainThread(LPVOID module)
{
	logger::warn("MainThread");
	const auto hModule = static_cast<HMODULE>(module);

	RE::BSGraphics::RendererWindow* renderWindow = nullptr;
	RE::BSGraphics::RendererData* rendererData = nullptr;
	while (!(renderWindow = BSGraphics::GetCurrentRendererWindow()) ||
		   !(rendererData = BSGraphics::GetRendererData()) ||
		   !renderWindow->hwnd ||
		   !renderWindow->swapChain ||
		   !rendererData->device ||
		   !rendererData->context) {
		Sleep(10);
	}

	hookIns->ImplHookDX11_Init(hModule, renderWindow->hwnd);
	//ImplHookDX11_Init(hModule, BSGraphics::RendererData::GetSingleton()->renderWindow->hwnd);

	return S_OK;
}

void TestingThread()
{
	while (true) {
	}
}

void InitializePlugin()
{
	if (settings.AllowsPrivateRenderHooks()) {
		// Stage 3 deliberately uses F4SE Menu Framework's already verified
		// before-render callback. The original FTS Present, ResizeBuffers,
		// DrawIndexed, and TAA hooks are not installed until Stage 4 because
		// none of them is required to prove a read-only back-buffer copy.
		HANDLE hThread = CreateThread(
			nullptr,
			0,
			MainThread,
			REX::W32::GetCurrentModule(),
			0,
			nullptr);
		if (hThread) {
			CloseHandle(hThread);
		} else {
			logger::error("Unable to start the renderer initialization thread");
		}
	} else {
		logger::info(
			"Verification stage {}: MagnaScope swap-chain and context hooks are disabled",
			settings.verificationStage);
		if (settings.AllowsTAACapture() &&
			!hookIns->InstallVerificationTAAHook()) {
			logger::warn(
				"Stage 3b TAA callback was not installed; "
				"framework Present capture remains active");
		}
	}
#ifdef _DEBUG
	HANDLE hThread1 = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)TestingThread, (HMODULE)REX::W32::GetCurrentModule(), 0, NULL);
	if (hThread1) {
		CloseHandle(hThread1);
	}
#endif
	auto* ui = UI::GetSingleton();
	if (!ui) {
		logger::error("UI singleton is unavailable; MagnaScope initialization deferred");
		return;
	}
	uiTimer = &ui->uiTimer;

	// Rendering remains disabled until a verified profile enters ADS and the
	// configured rollout stage explicitly allows GPU work.
	hookIns->EnableRender(false);

	player = PlayerCharacter::GetSingleton();
	pcam = PlayerCamera::GetSingleton();
	if (!player || !pcam) {
		logger::error("Player singletons are unavailable; MagnaScope initialization aborted");
		return;
	}
	((InputEventReceiverOverride*)((uint64_t)pcam + 0x38))->HookSink();

	an_45 = (RE::BGSKeyword*)RE::TESForm::GetFormByEditorID("an_45d");
	AnimsXM2010_scopeKH45 = (RE::BGSKeyword*)RE::TESForm::GetFormByEditorID("AnimsXM2010_scopeKH45");
	AX50_toounScope_K = (RE::BGSKeyword*)RE::TESForm::GetFormByEditorID("AX50_toounScope_K");
	AnimsXM2010_scopeKM = (RE::BGSKeyword*)RE::TESForm::GetFormByEditorID("AnimsXM2010_scopeKM");
	AnimsAX50_scopeKH45 = (RE::BGSKeyword*)RE::TESForm::GetFormByEditorID("AnimsAX50_scopeKH45");
	AX50_toounScope_L = (RE::BGSKeyword*)RE::TESForm::GetFormByEditorID("AX50_toounScope_L");
	AnimsAX50_scopeK = (RE::BGSKeyword*)RE::TESForm::GetFormByEditorID("AnimsAX50_scopeK");
	Tull_SideAimKeyword = (BGSKeyword*)GetFormFromMod("Tull_Framework.esp", 0x804);
	Tull_SupportKeyword = (BGSKeyword*)GetFormFromMod("Tull_Framework.esp", 0x80F);

	QMW_AnimsQBZ191M_on = (RE::BGSKeyword*)RE::TESForm::GetFormByEditorID("QMW_AnimsQBZ191M_on");
	QMW_AnimsQBZ191M_off = (RE::BGSKeyword*)RE::TESForm::GetFormByEditorID("QMW_AnimsQBZ191M_off");
	QMW_AnimsRU556M_on = (RE::BGSKeyword*)RE::TESForm::GetFormByEditorID("QMW_AnimsRU556M_on");
	QMW_AnimsRU556M_off = (RE::BGSKeyword*)RE::TESForm::GetFormByEditorID("QMW_AnimsRU556M_off");

	sdh = ScopeData::ScopeDataHandler::GetSingleton();

	((AnimationGraphEventWatcher*)((uint64_t)PlayerCharacter::GetSingleton() + 0x38))->HookSink();

	// Equipped-instance identity is checked in HookedUpdate. This replaces the
	// removed TESEquipEvent singleton accessor in multi-runtime CommonLibF4.
	hookIns->QueryChangeReticleTexture();
	sdh->ReadCustomScopeDataFiles(customPath);
	sdh->ReadDefaultScopeDataFile();
}

void ResetScopeStatus()
{
	if (auto* controls = RE::ControlMap::GetSingleton()) {
		controls->ignoreKeyboardMouse = false;
	}

	hookIns->InitPlayerData(player, pcam);
	InitCurrentScopeData();
	hookIns->SetScopeEffect(false);
	hookIns->QueryChangeReticleTexture();

	if (sdh->GetCurrentFTSData()) {
		gameDeltaZoom = sdh->GetCurrentFTSData()->shaderData.minZoom;
	}

	InGameFlag = true;
	hookIns->SetIsInGame(InGameFlag);
	hookIns->SetInterfaceTextRefresh(true);
}

F4SE_EXPORT bool F4SEAPI F4SEPlugin_Query(const F4SE::QueryInterface* a_f4se, F4SE::PluginInfo* a_info)
{
	a_info->infoVersion = F4SE::PluginInfo::kVersion;
	a_info->name = Plugin::NAME.data();
	a_info->version = Plugin::VERSION[0];

	if (a_f4se->IsEditor()) {
		return false;
	}

	const auto ver = a_f4se->RuntimeVersion();
	if (ver < F4SE::RUNTIME_1_10_163) {
		return false;
	}

	return true;
}

F4SE_PLUGIN_LOAD(const F4SE::LoadInterface* a_f4se)
{
#ifdef _DEBUG
	while (!IsDebuggerPresent()) {
	}
	Sleep(1000);
#endif

	F4SE::Init(a_f4se, {
						   .log = true,
						   .logName = Plugin::NAME.data(),
						   .trampoline = true,
						   .trampolineSize = 64,
					   });
	logger::info(
		"{} v{}.{}.{} loading on runtime {}",
		Plugin::NAME,
		Plugin::VERSION[0],
		Plugin::VERSION[1],
		Plugin::VERSION[2],
		a_f4se->RuntimeVersion().string());

	// All measured hooks in the inherited implementation are OG addresses.
	// NG and AE deliberately load as no-ops until their sites are re-derived.
	if (!REX::FModule::IsRuntimeOG()) {
		logger::warn(
			"Runtime {} is not supported yet; MagnaScope is disabled safely",
			a_f4se->RuntimeVersion().string());
		return true;
	}

	settings.Load();
	if (settings.verificationStage == 5) {
		// The Stage 5 auxiliary renderer experiment hooked broad world-render
		// entry points and was implicated in an external culling crash. It is
		// intentionally absent from the production DLL. Preserve old INI
		// compatibility by failing closed instead of silently starting normal
		// gameplay hooks with a retired diagnostic configuration.
		logger::critical(
			"Stage 5 auxiliary-renderer diagnostics are retired. MagnaScope "
			"failed closed; use the Stage 5d production fixture instead.");
		return true;
	}

	hookIns = Hook::D3D::GetSington();
	imgui_Impl = ImGuiImpl::ImGuiImplClass::GetSington();
#ifdef _DEBUG
	hookIns->InitRenderDoc();
#endif  // _DEBUG

	REL::Trampoline& trampoline = REL::GetTrampoline();
	const auto updateCallSite = ptr_PCUpdateMainThread.address();
	if (*reinterpret_cast<const std::uint8_t*>(updateCallSite) != 0xE8) {
		logger::critical(
			"PCUpdateMainThread hook guard failed at {:X}; MagnaScope is disabled",
			updateCallSite);
		return true;
	}
	PCUpdateMainThreadOrig = trampoline.write_call<5>(updateCallSite, &HookedUpdate);

	const F4SE::PapyrusInterface* papyrus = F4SE::GetPapyrusInterface();
	if (!papyrus) {
		logger::critical("Papyrus interface is unavailable");
		return false;
	}
	bool succ = papyrus->Register(RegisterFuncs);
	if (succ) {
		logger::warn("succ.");
	}

	const F4SE::MessagingInterface* message = F4SE::GetMessagingInterface();
	if (!message) {
		logger::critical("Messaging interface is unavailable");
		return false;
	}
	if (!message->RegisterListener([](F4SE::MessagingInterface::Message* msg) -> void {
			if (!msg) {
				return;
			}
			if (msg->type == F4SE::MessagingInterface::kPostLoad) {
				ImGuiImpl::RegisterMenu();
			} else if (msg->type == F4SE::MessagingInterface::kGameDataReady) {
				InitializePlugin();

			} else if (msg->type == F4SE::MessagingInterface::kPostLoadGame) {
				ResetScopeStatus();
			} else if (msg->type == F4SE::MessagingInterface::kPreLoadGame) {
				ClearIsolatedZoomSession();
				sdh->SetCurrentFTSData(nullptr);
				currentData = nullptr;
				weaponInstanceData = nullptr;
				lastEquippedInstance = nullptr;
				lastAttachmentKey.clear();
				hasScopeSelectionSnapshot = false;
			} else if (msg->type == F4SE::MessagingInterface::kPreSaveGame) {
				DetachIsolatedZoomForSave();
			} else if (msg->type == F4SE::MessagingInterface::kNewGame) {
				ResetScopeStatus();
			} else if (msg->type == F4SE::MessagingInterface::kPostSaveGame) {
				ReattachIsolatedZoomAfterSave();
			} else if (msg->type == F4SE::MessagingInterface::kGameLoaded) {
				//reshadeImpl->SetRenderEffect(false);
			}
		})) {
		logger::critical("Unable to register the F4SE messaging listener");
		return false;
	}

	return true;
}
