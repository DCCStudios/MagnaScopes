#include "ScopeProfile.h"
#include "EyeBoxRecentering.h"
#include "ImGuiImpl.h"
#include "Raycast.h"
#include "ScopeResolver.h"
#include "ScopeCoSave.h"
#define MAGNASCOPE_INTERNAL
#include "MagnaScopeAPI.h"
#include "Settings.h"
#include "WorldOnlyScopeRenderer.h"
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
		{
			// Experimental: graph events fired on a secondary-sight zoom
			// pointer swap, so the re-sample trigger can be hunted from the
			// INI without rebuilding. Unknown names are no-ops to the graph.
			wchar_t wide[256]{};
			GetPrivateProfileStringW(
				L"Sights",
				L"SightSwapGraphEvents",
				L"GunUp",
				wide,
				static_cast<DWORD>(std::size(wide)),
				path.c_str());
			sightSwapGraphEvents.clear();
			for (const wchar_t* cursor = wide; *cursor != L'\0'; ++cursor) {
				sightSwapGraphEvents.push_back(
					static_cast<char>(*cursor & 0x7F));
			}
		}
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
		synthesizedAperture =
			GetPrivateProfileIntW(
				L"AutoSTS",
				L"SynthesizedAperture",
				1,
				path.c_str()) != 0;
		synthesizedApertureProbe =
			GetPrivateProfileIntW(
				L"AutoSTS",
				L"SynthesizedApertureProbe",
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
			L"AutoSTS",
			L"SynthesizedAperture",
			synthesizedAperture ? L"1" : L"0",
			path.c_str());
		WritePrivateProfileStringW(
			L"AutoSTS",
			L"SynthesizedApertureProbe",
			synthesizedApertureProbe ? L"1" : L"0",
			path.c_str());
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

bool bNeedToUpdateScopeProfile = true;
bool bChangeAnimFlag = false;
bool nvgFlag = false;
bool hasCombo = false;
bool hasNvgCommit = false;
bool thermalFlag = false;
// The profile whose default-on state was last applied. Selection runs every
// frame while equipped, so default-on is seeded only when this changes -- that
// way the live hotkey can still toggle a mode within a scope's session without
// being re-forced on the next frame.
ScopeData::ScopeProfile* lastVisionSeededProfile = nullptr;
bool hasThermalCombo = false;
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
	// True only for a 48-vertex ScopeFade annulus. The fill shader assigns lens
	// coordinates from primitive order across 24 segments, so on any other mesh
	// they would be meaningless -- the draw would still match and still be
	// replaced, producing a confidently wrong lens. Everything else the
	// aperture supplies, projection and eye box and mask, works on any shape.
	bool supportsExactReplay{ false };
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

	// Measured from the shape's own vertices rather than inferred from names
	// and bounding spheres. `worldRadius` above is the heuristic
	// (`planeRadius * 3.0` and friends) and stays as it is: the magnify shader
	// solves its own projected radius from the replayed geometry, so nothing
	// downstream is broken by that number being three times the physical rim,
	// and the eye box that normalizes against it reads correctly today.
	//
	// These fields exist for the synthesized aperture path, which has no
	// authored mesh to solve against and therefore needs the true size.
	bool measurementValid{ false };
	// Outer rim in world units, already multiplied by the object's world
	// scale. On every ScopeFade measured so far this equals worldBound.fRadius
	// exactly, and unlike that field it does not transiently collapse to 1.0
	// because model-space extents are static data.
	float measuredWorldRadius{ 0.0F };
	// Inner rim over outer rim. ScopeFade reads ~0.497; a solid lens reads
	// ~0.000; a wire ring such as specter_reticle reads ~0.994. An aperture is
	// the middle band, which is why this is a ratio and not a bool.
	float measuredInnerRatio{ 0.0F };
	// 0, 1 or 2 for local X, Y or Z. Every genuine aperture measured so far is
	// local Y, matching the X/Z plane the projection code already assumes.
	int measuredOpticalAxis{ 1 };
	// The vertex centroid in the shape's local space, before its own scale.
	// Transforming this through the shape's live `world` gives the optical
	// centre without consulting worldBound.center -- the same bound whose
	// fRadius is observed collapsing to exactly 1.0 for every shape at once
	// while the scene graph updates. Model-space extents are static data and
	// have no such failure mode.
	RE::NiPoint3 measuredLocalCentroid{};
	// The shape whose measurement produced the fields above and whose live
	// draw supplies placement for the synthesized aperture. Usually the
	// selected plane; when the selection is not disc-shaped -- the user
	// picked the scope body, or ranking landed on a housing -- this is the
	// most lens-like candidate instead, so the optic stays correct no matter
	// what drives everything else.
	RE::NiAVObject* synthesisSource{ nullptr };
	// Outer rim in the synthesis source's model space, pre-scale. This is
	// the radius the captured-placement path builds the ring at, because the
	// captured transform constants apply the node's scale exactly as they
	// did to the mesh's own vertices.
	float measuredLocalRadius{ 0.0F };
	// The OPTICAL PLANE's own model-space outer rim, from its vertices. Zero
	// when that shape did not measure.
	//
	// Distinct from measuredLocalRadius above, which belongs to the synthesis
	// source and is a different shape whenever the plane is not disc-like.
	// This one is the radius ScopeGeometryFill_GS calls lens coordinate 1.0,
	// so it is the unit anything the magnify shader consumes as a lens
	// coordinate has to be expressed in -- notably the authored aim offset.
	float lensLocalRadius{ 0.0F };
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
	float baselineEyeLocalX{ 0.0F };
	float baselineEyeLocalZ{ 0.0F };
	float baselineEyeReliefDistance{ 0.0F };
	Hook::D3D::PhysicalEyeBoxSample lastValidSample{};
	// Published eye-box offsets are smoothed independently of the calibration
	// baseline. Keeping the previous output makes a transient projection or
	// baseline change converge continuously instead of snapping to zero or to a
	// newly measured target.
	float smoothedNormalizedX{ 0.0F };
	float smoothedNormalizedY{ 0.0F };
	float smoothedNormalizedRelief{ 0.0F };
	RE::NiPoint3 previousCameraTranslation{};
	RE::NiMatrix3 previousCameraRotation{};
	// Previous world position of the player, for the translation component of
	// the lag.
	//
	// Neither the camera node nor the aperture can supply this, and measurement
	// settled it rather than reasoning: during a hard strafe the camera node
	// reported three to twelve thousandths of a unit per frame and the aperture
	// eight to twenty-four, against the unit or so actually covered. The
	// aperture's residual was also dominated by its vertical component, which
	// is walk bob rather than travel.
	//
	// Both live in Fallout's first-person scene graph, which is not positioned
	// in world space -- the viewmodel is rendered camera-relative, so nothing
	// in it registers the player crossing the map. Only the player reference
	// carries a genuine world position.
	RE::NiPoint3 previousPlayerWorld{};
	bool previousPlayerWorldReady{ false };
	float angularLagX{ 0.0F };
	float angularLagY{ 0.0F };
	// Settled screen position of the aperture itself. The camera-versus-optic
	// residual above is near zero in Fallout because the weapon is rigidly
	// parented to the camera in ADS, so it cannot drive the optics on its own.
	// What the player actually sees move is the aperture, and its excursion
	// from this settled position is the usable optical-motion signal.
	float smoothedApertureRadius{ 0.0F };
	float smoothedApertureScreenX{ 0.0F };
	float smoothedApertureScreenY{ 0.0F };
	bool apertureScreenReady{ false };
	bool baselineReady{ false };
	bool hasLastValidSample{ false };
	bool hasSmoothedOffset{ false };
	bool previousCameraPoseReady{ false };
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
	// A stale placement identity is a set of dead buffer pointers compared as
	// integers. Never dereferenced, but a reused allocation could match a
	// foreign draw and capture the wrong transforms, so clear it whenever the
	// selection itself is torn down.
	Hook::D3D::InvalidateSynthesisPlacement();
	Hook::D3D::InvalidateSynthesizedApertureRing();
}

float UpdateAutomaticSTSTracking(
	const RE::NiAVObject* aperture,
	const Hook::D3D::ScreenSphereProjection& projection,
	bool apertureVisible,
	float deltaSeconds)
{
	if (!aperture) {
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
	if (!projection.valid) {
		// A CPU sphere can be invalid for a frame while the exact ScopeFade
		// draw and the previous coherent projection remain usable. Do not
		// invalidate the output or force the shader back to its center.
		return activationProgress();
	}

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

// Names which guard rejected the physical eye-box sample. Zero means the
// sample was produced normally. Every consumer of eye travel -- the
// exit-pupil crescent, image stillness, and axial breathing -- reads the
// same published value, so one rejected sample disables all three at once.
std::atomic<int> g_eyeBoxBailReason{ -1 };

// Optical smoothing needs a real elapsed time. UI::uiTimer is the menu timer
// and does not advance during ordinary gameplay, so every response alpha
// derived from it evaluated to exactly zero:
//
//   smoothed += (target - smoothed) * CalculateResponseAlpha(0, tau)   // * 0
//
// The smoothed eye-box output could therefore never leave its initial centred
// value. Published travel was permanently (0, 0) while the sample still
// reported valid, which silently disabled the exit-pupil crescent, image
// stillness, and axial response at once without tripping any guard.
//
// Measure the frame interval directly. The bound is the same one the tracker
// already applied, so a long stall, a load screen, or a paused game cannot
// deliver a single huge step to the filters.
[[nodiscard]] float AcquireOpticalFrameDelta() noexcept
{
	using clock = std::chrono::steady_clock;
	static clock::time_point previous{};
	static bool hasPrevious = false;
	const auto now = clock::now();
	if (!hasPrevious) {
		previous = now;
		hasPrevious = true;
		return 0.0F;
	}
	const float seconds =
		std::chrono::duration<float>(now - previous).count();
	previous = now;
	return std::isfinite(seconds) ? std::clamp(seconds, 0.0F, 0.05F) : 0.0F;
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
	const RE::NiPoint3& apertureScreenCenter,
	const RE::NiPoint3& aimWorldCenter,
	float apertureLensLocalRadius)
{
	Hook::D3D::PhysicalEyeBoxSample result{};
	auto& state = automaticSTSEyeBoxTracking;
	const auto lastValidOrCentered = [&](int reason) {
		g_eyeBoxBailReason.store(reason, std::memory_order_relaxed);
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
		return lastValidOrCentered(1);
	}

	if (state.apertureIdentity != aperture ||
		state.profileIdentity != profileIdentity) {
		// ScopeFade objects are rebuilt when a weapon or attachment changes.
		// Never carry a calibration baseline into another optical assembly, but
		// preserve the already-published output while the replacement node is
		// settling. This avoids a visible center snap during a scene-graph
		// rebuild without reusing stale calibration data.
		const bool preserveOutput =
			state.profileIdentity == profileIdentity &&
			state.hasSmoothedOffset;
		const float savedX = state.smoothedNormalizedX;
		const float savedY = state.smoothedNormalizedY;
		const float savedRelief = state.smoothedNormalizedRelief;
		const float savedAngularX = state.angularLagX;
		const float savedAngularY = state.angularLagY;
		const RE::NiPoint3 savedCameraTranslation =
			state.previousCameraTranslation;
		const RE::NiMatrix3 savedCameraRotation =
			state.previousCameraRotation;
		const bool savedCameraPoseReady = state.previousCameraPoseReady;
		const RE::NiPoint3 savedPlayerWorld = state.previousPlayerWorld;
		const bool savedPlayerWorldReady = state.previousPlayerWorldReady;
		state = {};
		state.apertureIdentity = aperture;
		state.profileIdentity = profileIdentity;
		if (preserveOutput) {
			state.smoothedNormalizedX = savedX;
			state.smoothedNormalizedY = savedY;
			state.smoothedNormalizedRelief = savedRelief;
			state.angularLagX = savedAngularX;
			state.angularLagY = savedAngularY;
			state.previousCameraTranslation = savedCameraTranslation;
			state.previousCameraRotation = savedCameraRotation;
			state.previousCameraPoseReady = savedCameraPoseReady;
			state.previousPlayerWorld = savedPlayerWorld;
			state.previousPlayerWorldReady = savedPlayerWorldReady;
			state.hasSmoothedOffset = true;
		}
	}

	const float worldScale = std::abs(aperture->world.scale);
	if (!std::isfinite(worldScale) ||
		worldScale <= 0.0001F ||
		worldScale >= 10000.0F) {
		return lastValidOrCentered(2);
	}
	const float localRadius = apertureWorldRadius / worldScale;
	if (!std::isfinite(localRadius) ||
		localRadius <= 0.001F ||
		localRadius >= 100000.0F) {
		return lastValidOrCentered(3);
	}

	// STS ScopeFade's standardized 48-vertex annulus lies in local X/Z. Build
	// its current projected basis, but measure camera-versus-optic displacement
	// in that authored local plane. Screen-center drift is not a usable motion
	// signal: a correctly aligned ADS optic remains centered while the camera
	// turns, which reduced the prior scope-shadow input to approximately zero.
	const RE::NiTransform inverseAperture = aperture->world.Invert();
	const RE::NiPoint3 apertureLocalCenter =
		inverseAperture * apertureWorldCenter;
	if (!IsFinitePoint(apertureLocalCenter) ||
		!IsFinitePoint(apertureScreenCenter)) {
		return lastValidOrCentered(4);
	}
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
	if (!IsFinitePoint(xAxisScreen) || !IsFinitePoint(zAxisScreen) ||
		xAxisScreen.z <= 0.001F || zAxisScreen.z <= 0.001F) {
		return lastValidOrCentered(5);
	}

	const float basisXX = xAxisScreen.x - apertureScreenCenter.x;
	const float basisXY = xAxisScreen.y - apertureScreenCenter.y;
	const float basisZX = zAxisScreen.x - apertureScreenCenter.x;
	const float basisZY = zAxisScreen.y - apertureScreenCenter.y;
	const float xBasisLength =
		std::sqrt(basisXX * basisXX + basisXY * basisXY);
	const float zBasisLength =
		std::sqrt(basisZX * basisZX + basisZY * basisZY);
	const float averageBasisLength = 0.5F * (xBasisLength + zBasisLength);
	// A circular aperture projects to an ellipse, so its true projected radius
	// is the major semi-axis; the minor one is pure foreshortening. Runtime
	// telemetry showed |basisX| collapsing from 452 to 141 pixels and even
	// changing sign while |basisZ| held at 460 on an optic whose apparent size
	// was barely moving. Averaging the two turned that into a 35% swing in the
	// normalizing denominator, which drowned the fore/aft apparent-size signal
	// and injected spurious lateral travel. The maximum is stable under the
	// same foreshortening.
	const float apertureProjectedRadius =
		std::max(xBasisLength, zBasisLength);
	if (!std::isfinite(xBasisLength) || !std::isfinite(zBasisLength) ||
		!std::isfinite(averageBasisLength) ||
		xBasisLength <= 0.01F || zBasisLength <= 0.01F ||
		xBasisLength > 100000.0F || zBasisLength > 100000.0F) {
		return lastValidOrCentered(6);
	}

	const float boundedDeltaSeconds =
		std::clamp(deltaSeconds, 0.0F, 0.05F);
	// How quickly the optic returns to its settled state. Higher is faster.
	// Every response below divides its time constant by this, so one control
	// governs pupil recentering, image recentering, and angular-lag decay
	// together rather than leaving them as three fixed constants.
	const float recenterSpeed = std::clamp(
		Hook::D3D::scopeRecenterSpeed.load(std::memory_order_acquire),
		0.1F,
		10.0F);
	// Player translation gets its own gain. A pan and a strafe reach the lag
	// accumulator through completely different measurements -- a distant
	// point's screen displacement against a few game units of camera motion --
	// so a single control could only ever suit one of them.
	const float strafeLag = std::clamp(
		Hook::D3D::scopeStrafeLag.load(std::memory_order_acquire),
		0.0F,
		4.0F);
	// ScopeFade lies in local X/Z, making local Y its authored optical normal.
	// Measure the camera against that plane in ScopeFade-local coordinates.
	// Unlike camera-view Z, this distance cannot change merely because the
	// player pans or pitches while the camera and optic remain a rigid pair.
	const RE::NiPoint3 cameraApertureLocal =
		inverseAperture * camera->world.translate;
	const float currentEyeReliefDistance =
		MagnaScope::EyeBoxRecentering::CalculateEyeReliefDistance(
			cameraApertureLocal.y,
			apertureLocalCenter.y);
	if (!IsFinitePoint(cameraApertureLocal) ||
		!std::isfinite(currentEyeReliefDistance) ||
		currentEyeReliefDistance <= 0.001F) {
		return lastValidOrCentered(7);
	}

	// The authored aim point in ScopeFade lens coordinates, solved here in the
	// aperture's own local frame.
	//
	// The render thread used to recover this by inverting the projected lens
	// basis assembled above -- the aperture's local X and Z axes projected to
	// screen. That basis is not conditioned for inversion: its columns
	// foreshorten by different amounts as the optic turns (the note on
	// apertureProjectedRadius above records |basisX| falling from 452 to 141
	// pixels and changing sign while |basisZ| held at 460), so its determinant
	// passes through zero, and around that zero the recovered offset explodes
	// and flips sign. Two consecutive frames measuring (1.6, -38.6) and
	// (0.7, -39.3) pixels reported 1.246 and 6.625 aperture radii for a true
	// value near 0.11.
	//
	// The radius is the authored ScopeFade rim from the mesh's own vertices,
	// not `apertureWorldRadius`. Those differ by a deliberate factor of three
	// (see STSApertureSelection::worldRadius), and lens coordinate 1.0 is the
	// rim: ScopeGeometryFill_GS places the annulus's outer ring there.
	float aimLensX = 0.0F;
	float aimLensY = 0.0F;
	bool aimLensValid = false;
	if (apertureLensLocalRadius > 0.0001F && IsFinitePoint(aimWorldCenter)) {
		const RE::NiPoint3 aimLocal = inverseAperture * aimWorldCenter;
		if (IsFinitePoint(aimLocal)) {
			const float solvedX =
				(aimLocal.x - apertureLocalCenter.x) / apertureLensLocalRadius;
			const float solvedY =
				(aimLocal.z - apertureLocalCenter.z) / apertureLensLocalRadius;
			if (std::isfinite(solvedX) && std::isfinite(solvedY)) {
				aimLensX = solvedX;
				aimLensY = solvedY;
				aimLensValid = true;
			}
		}
	}

	if (!state.baselineReady || activationProgress <= 0.001F) {
		// Start exactly centered. A stationary optic must never inherit a
		// heading-dependent calibration error from a previous pose.
		state.baselineEyeLocalX = cameraApertureLocal.x;
		state.baselineEyeLocalZ = cameraApertureLocal.z;
		state.baselineEyeReliefDistance = currentEyeReliefDistance;
		state.baselineReady = true;
	} else {
		// The settled eye point follows the current camera position continuously
		// in ScopeFade-local space.  The difference is a bounded high-pass motion
		// signal: rigid camera/weapon rotation cancels, relative weapon inertia
		// remains visible, and a stationary pose returns asymptotically to center
		// without thresholds or a final snap.
		constexpr float kOpticalFollowerTimeConstant = 0.090F;
		const float followerBlend =
			MagnaScope::EyeBoxRecentering::CalculateResponseAlpha(
				boundedDeltaSeconds,
				kOpticalFollowerTimeConstant / recenterSpeed);
		state.baselineEyeLocalX +=
			(cameraApertureLocal.x - state.baselineEyeLocalX) * followerBlend;
		state.baselineEyeLocalZ +=
			(cameraApertureLocal.z - state.baselineEyeLocalZ) * followerBlend;
		// Axial depth is independent from the planar follower even though both
		// use a smooth response. A lateral pan therefore cannot make the exit
		// pupil uniformly shrink or grow.
		state.baselineEyeReliefDistance +=
			(currentEyeReliefDistance - state.baselineEyeReliefDistance) *
			followerBlend;
	}

	// Convert the local eye displacement through the *current* projected basis.
	// The shaders consume display X/Y and invert that same basis at draw time,
	// recovering a direction-independent optic-local pupil displacement.
	const auto screenEyeOffset =
		MagnaScope::EyeBoxRecentering::ProjectLocalEyeOffsetToScreen(
			cameraApertureLocal.x,
			cameraApertureLocal.z,
			state.baselineEyeLocalX,
			state.baselineEyeLocalZ,
			localRadius,
			basisXX,
			basisXY,
			basisZX,
			basisZY,
			averageBasisLength);
	if (!screenEyeOffset.valid) {
		return lastValidOrCentered(8);
	}
	const float maximumTravel = std::clamp(
		Hook::D3D::scopeEyeBoxMaxTravel.load(std::memory_order_acquire),
		0.0F,
		4.0F);

	// Weapon-relative camera displacement above is the physically meaningful
	// signal for first-person inertia. A rigid camera/weapon rotation cancels in
	// that local frame, however, so it cannot by itself produce the familiar
	// transient eye-box shadow during a quick pan. Measure that second signal by
	// projecting the previous and current camera forward rays through the
	// *current* camera. This is display-local rather than world-heading-local:
	// right is always right and up is always up, regardless of compass heading
	// or pitch. prev-current makes the pupil lag opposite the camera movement.
	const RE::NiPoint3 currentCameraTranslation = camera->world.translate;
	const RE::NiMatrix3 currentCameraRotation = camera->world.rotate;
	// Strafe-path telemetry. Every stage between the camera moving and the
	// opening shifting, so a dead effect can be traced to the stage that
	// zeroed rather than guessed at from the shader backwards.
	float diagCameraStepLength = 0.0F;
	float diagLateralStep = 0.0F;
	float diagVerticalStep = 0.0F;
	float diagTranslationImpulseX = 0.0F;
	float diagAngularImpulseX = 0.0F;
	bool diagRotationBranchRan = false;
	bool diagTranslationApplied = false;
	// Capture fore/aft camera travel before the block below overwrites the
	// previous pose. This is the only axial signal used; see the eye-relief
	// note further down for why the aperture-local measurement cannot be.
	float axialTravelAlongView = 0.0F;
	if (state.previousCameraPoseReady && activationProgress > 0.001F) {
		const RE::NiPoint3 viewForward =
			currentCameraRotation.Transpose() *
			RE::NiPoint3{ 0.0F, 0.0F, -1.0F };
		const RE::NiPoint3 cameraStep =
			currentCameraTranslation - state.previousCameraTranslation;
		axialTravelAlongView =
			cameraStep.x * viewForward.x +
			cameraStep.y * viewForward.y +
			cameraStep.z * viewForward.z;
	}
	if (!state.previousCameraPoseReady || activationProgress <= 0.001F) {
		state.previousCameraTranslation = currentCameraTranslation;
		state.previousCameraRotation = currentCameraRotation;
		state.previousCameraPoseReady = true;
		if (const auto* const playerNow = RE::PlayerCharacter::GetSingleton()) {
			state.previousPlayerWorld = playerNow->GetPosition();
			state.previousPlayerWorldReady = true;
		}
		state.angularLagX = 0.0F;
		state.angularLagY = 0.0F;
	} else {
		constexpr float kForwardDistance = 1000.0F;
		const RE::NiPoint3 localForward{ 0.0F, 0.0F, -kForwardDistance };
		const RE::NiPoint3 previousForwardWorld =
			state.previousCameraTranslation +
			state.previousCameraRotation.Transpose() * localForward;
		const RE::NiPoint3 currentForwardWorld =
			currentCameraTranslation +
			currentCameraRotation.Transpose() * localForward;
		const RE::NiPoint3 previousForwardScreen = hookIns->WorldPointToScreen(
			camera,
			previousForwardWorld,
			firstPersonFov);
		const RE::NiPoint3 currentForwardScreen = hookIns->WorldPointToScreen(
			camera,
			currentForwardWorld,
			firstPersonFov);

		constexpr float kAngularLagDecaySeconds = 0.055F;
		const float decay = std::exp(
			-boundedDeltaSeconds *
			recenterSpeed / kAngularLagDecaySeconds);
		state.angularLagX *= decay;
		state.angularLagY *= decay;
		if (IsFinitePoint(previousForwardScreen) &&
			IsFinitePoint(currentForwardScreen) &&
			previousForwardScreen.z > 0.001F &&
			currentForwardScreen.z > 0.001F) {
			float impulseX =
				(previousForwardScreen.x - currentForwardScreen.x) /
				averageBasisLength *
				MagnaScope::EyeBoxRecentering::kAngularLagGain;
			float impulseY =
				(previousForwardScreen.y - currentForwardScreen.y) /
				averageBasisLength *
				MagnaScope::EyeBoxRecentering::kAngularLagGain;
			// Strafing and walking contribute nothing above: the camera and
			// the reference point translate together, so a rotation measured
			// from a distant point's screen displacement reads zero however
			// fast the player moves sideways. Add the translation directly.
			//
			// Measured along the camera's own right and up axes and expressed
			// in aperture radii, which is the unit the whole eye-box path
			// already speaks. The signs match the rotation impulse: moving
			// right sweeps the world left exactly as looking right does, and
			// rising sweeps it down exactly as looking up does, so both must
			// push the opening the same way a pan would.
			diagRotationBranchRan = true;
			diagAngularImpulseX = impulseX;
			// Measured from the aperture, not the camera.
			//
			// The camera node's world.translate does not move with the player:
			// during a hard strafe it reported three to twelve thousandths of a
			// unit per frame against the unit or so actually covered, so it is
			// expressed in a frame that travels along. Everything downstream
			// was correct and working on a signal that was not there.
			//
			// The aperture was no better: it reported eight to twenty-four
			// thousandths per frame, dominated by its vertical component, which
			// is walk bob rather than travel. Both live in Fallout's
			// first-person scene graph, and that graph is not positioned in
			// world space -- the viewmodel is rendered camera-relative, so
			// nothing inside it registers the player crossing the map. Only the
			// player reference carries a genuine world position.
			const auto* const playerReference =
				RE::PlayerCharacter::GetSingleton();
			if (apertureWorldRadius > 0.001F && strafeLag > 0.0F &&
				playerReference && state.previousPlayerWorldReady) {
				const RE::NiPoint3 cameraRight =
					currentCameraRotation.Transpose() *
					RE::NiPoint3{ 1.0F, 0.0F, 0.0F };
				const RE::NiPoint3 cameraUp =
					currentCameraRotation.Transpose() *
					RE::NiPoint3{ 0.0F, 1.0F, 0.0F };
				const RE::NiPoint3 playerStep =
					playerReference->GetPosition() -
					state.previousPlayerWorld;
				const float lateralStep =
					playerStep.x * cameraRight.x +
					playerStep.y * cameraRight.y +
					playerStep.z * cameraRight.z;
				const float verticalStep =
					playerStep.x * cameraUp.x +
					playerStep.y * cameraUp.y +
					playerStep.z * cameraUp.z;
				const float translationScale =
					MagnaScope::EyeBoxRecentering::kTranslationLagGain *
					strafeLag /
					apertureWorldRadius;
				impulseX += lateralStep * translationScale;
				impulseY -= verticalStep * translationScale;
				diagCameraStepLength = std::sqrt(
					playerStep.x * playerStep.x +
					playerStep.y * playerStep.y +
					playerStep.z * playerStep.z);
				diagLateralStep = lateralStep;
				diagVerticalStep = verticalStep;
				diagTranslationImpulseX = lateralStep * translationScale;
				diagTranslationApplied = true;
			}
			// Bound the accumulated lag, not the individual impulse. Clamping
			// each impulse still let the decaying sum settle far past the
			// configured travel, and it put a derivative discontinuity in the
			// middle of a fast pan.
			MagnaScope::EyeBoxRecentering::SoftLimitVector(
				impulseX,
				impulseY,
				maximumTravel);
			if (std::isfinite(impulseX) && std::isfinite(impulseY)) {
				state.angularLagX += impulseX;
				state.angularLagY += impulseY;
				MagnaScope::EyeBoxRecentering::SoftLimitVector(
					state.angularLagX,
					state.angularLagY,
					maximumTravel);
			}
		}
		state.previousCameraTranslation = currentCameraTranslation;
		state.previousCameraRotation = currentCameraRotation;
		if (const auto* const playerNow = RE::PlayerCharacter::GetSingleton()) {
			state.previousPlayerWorld = playerNow->GetPosition();
			state.previousPlayerWorldReady = true;
		}
	}

	// Follow the aperture's own projected screen position and keep the
	// excursion from it. Recoil, sway, and weapon lag during a fast pan all
	// move the optic by hundreds of pixels, which is the motion the optics are
	// meant to respond to; the camera-relative residual alone measured only a
	// few percent of an aperture radius and was far too small to see.
	float apertureExcursionX = 0.0F;
	float apertureExcursionY = 0.0F;
	float apertureScaleRatio = 1.0F;
	if (!state.apertureScreenReady || activationProgress <= 0.001F) {
		state.smoothedApertureScreenX = apertureScreenCenter.x;
		state.smoothedApertureScreenY = apertureScreenCenter.y;
		state.smoothedApertureRadius = apertureProjectedRadius;
		state.apertureScreenReady = true;
	} else {
		constexpr float kApertureFollowerTimeConstant = 0.090F;
		const float apertureBlend =
			MagnaScope::EyeBoxRecentering::CalculateResponseAlpha(
				boundedDeltaSeconds,
				kApertureFollowerTimeConstant / recenterSpeed);
		state.smoothedApertureScreenX +=
			(apertureScreenCenter.x - state.smoothedApertureScreenX) *
			apertureBlend;
		state.smoothedApertureScreenY +=
			(apertureScreenCenter.y - state.smoothedApertureScreenY) *
			apertureBlend;
		// Negated to match CalculateScreenEyeOffset's convention: the eye moves
		// opposite the optic, so an aperture that swings right is an eye that
		// has moved left of the optical axis.
		apertureExcursionX =
			-(apertureScreenCenter.x - state.smoothedApertureScreenX) /
			apertureProjectedRadius;
		apertureExcursionY =
			-(apertureScreenCenter.y - state.smoothedApertureScreenY) /
			apertureProjectedRadius;
		if (!std::isfinite(apertureExcursionX) ||
			!std::isfinite(apertureExcursionY)) {
			apertureExcursionX = 0.0F;
			apertureExcursionY = 0.0F;
		}

		// Apparent-size change dominates fore/aft motion: walking toward a
		// target swings the projected radius by well over ten percent while the
		// centre barely moves. Holding the image still against that needs the
		// ratio, not the excursion.
		state.smoothedApertureRadius +=
			(apertureProjectedRadius - state.smoothedApertureRadius) *
			apertureBlend;
		if (state.smoothedApertureRadius > 0.01F) {
			apertureScaleRatio =
				apertureProjectedRadius / state.smoothedApertureRadius;
		}
		if (!std::isfinite(apertureScaleRatio)) {
			apertureScaleRatio = 1.0F;
		}
		apertureScaleRatio = std::clamp(apertureScaleRatio, 0.25F, 4.0F);
	}

	float normalizedX =
		screenEyeOffset.x + state.angularLagX + apertureExcursionX;
	float normalizedY =
		screenEyeOffset.y + state.angularLagY + apertureExcursionY;

	// Once a second while the optic is live. Reports the strafe path end to
	// end: how far the camera actually moved, how much of that was lateral,
	// what impulse it produced against the rotation impulse beside it, what
	// the accumulator holds, and what finally reaches the shader. Whichever
	// column reads zero is where the effect dies.
	{
		static float diagnosticSeconds = 0.0F;
		diagnosticSeconds += boundedDeltaSeconds;
		if (diagnosticSeconds >= 1.0F && activationProgress > 0.001F) {
			diagnosticSeconds = 0.0F;
			logger::info(
				"Eye-box strafe path: activation={:.2f}, rotationBranch={}, "
				"translationApplied={}, strafeLag={:.2f}, "
				"apertureWorldRadius={:.3f}, playerStep={:.3f} "
				"(lateral={:.3f}, vertical={:.3f}), impulse=(angular {:.4f}, "
				"translation {:.4f}), angularLag=({:.3f}, {:.3f}), "
				"published=({:.3f}, {:.3f})",
				activationProgress,
				diagRotationBranchRan,
				diagTranslationApplied,
				strafeLag,
				apertureWorldRadius,
				diagCameraStepLength,
				diagLateralStep,
				diagVerticalStep,
				diagAngularImpulseX,
				diagTranslationImpulseX,
				state.angularLagX,
				state.angularLagY,
				normalizedX,
				normalizedY);
		}
	}
	// Fore/aft eye motion is measured against the settled eye-to-aperture
	// distance, but that distance is taken along the aperture's own local
	// normal, and the aperture's frame rotates with the weapon. During a yaw
	// the weapon lags the camera slightly, so part of that lateral swing
	// projects onto the local normal and a pure left/right pan reported real
	// fore/aft motion -- panning read as the image moving closer and farther.
	//
	// Reject that by keeping only the component the camera actually
	// translated along its own view axis. A rotation moves the camera's
	// position very little, so yaw and pitch contribute almost nothing, while
	// walking forward or backward is captured in full.
	auto axialEyeRelief =
		MagnaScope::EyeBoxRecentering::CalculateAxialEyeRelief(
			currentEyeReliefDistance,
			state.baselineEyeReliefDistance);
	if (axialEyeRelief.valid &&
		std::isfinite(axialTravelAlongView) &&
		currentEyeReliefDistance > 0.001F) {
		// Moving forward closes the eye-to-optic gap, so a positive travel
		// along the view axis is a negative relief delta.
		axialEyeRelief.normalizedDelta = std::clamp(
			-axialTravelAlongView / currentEyeReliefDistance,
			-0.10F,
			0.10F);
	}
	// Axial relief is optional. A malformed or temporarily unavailable depth
	// sample must not discard valid lateral X/Y travel, because that invalidates
	// the complete physical-eye-box sample and disables the stationary rim.
	float normalizedRelief =
		axialEyeRelief.valid ? axialEyeRelief.normalizedDelta : 0.0F;
	if (!std::isfinite(normalizedX) ||
		!std::isfinite(normalizedY) ||
		!std::isfinite(normalizedRelief)) {
		return lastValidOrCentered(9);
	}
	// A sharp but finite drag remains a valid optical sample. Clamp the
	// published pupil travel instead of invalidating it, which formerly made
	// the shader snap to its zero-offset fallback for one frame.
	const float planarLength =
		std::sqrt(normalizedX * normalizedX + normalizedY * normalizedY);
	if (planarLength > maximumTravel && planarLength > 0.0001F) {
		const float scale = maximumTravel / planarLength;
		normalizedX *= scale;
		normalizedY *= scale;
	}
	normalizedRelief =
		std::clamp(normalizedRelief, -maximumTravel, maximumTravel);

	// The follower above already provides continuous motion. Keep only a very
	// short output filter to reject one-frame scene-graph noise without adding a
	// second visible lag or a delayed final snap.
	const float responseAlpha =
		MagnaScope::EyeBoxRecentering::CalculateResponseAlpha(
			boundedDeltaSeconds,
			0.010F / recenterSpeed);
	if (!state.hasSmoothedOffset) {
		state.smoothedNormalizedX = 0.0F;
		state.smoothedNormalizedY = 0.0F;
		state.smoothedNormalizedRelief = 0.0F;
		state.hasSmoothedOffset = true;
	}
	state.smoothedNormalizedX +=
		(normalizedX - state.smoothedNormalizedX) * responseAlpha;
	state.smoothedNormalizedY +=
		(normalizedY - state.smoothedNormalizedY) * responseAlpha;
	state.smoothedNormalizedRelief +=
		(normalizedRelief - state.smoothedNormalizedRelief) * responseAlpha;
	state.smoothedNormalizedX = std::clamp(
		state.smoothedNormalizedX,
		-maximumTravel,
		maximumTravel);
	state.smoothedNormalizedY = std::clamp(
		state.smoothedNormalizedY,
		-maximumTravel,
		maximumTravel);
	state.smoothedNormalizedRelief = std::clamp(
		state.smoothedNormalizedRelief,
		-maximumTravel,
		maximumTravel);

	result.apertureScaleRatio = apertureScaleRatio;
	result.apertureProjectedRadius = apertureProjectedRadius;
	result.apertureExcursion = std::sqrt(
		apertureExcursionX * apertureExcursionX +
		apertureExcursionY * apertureExcursionY);
	result.eyeOffsetX = state.smoothedNormalizedX;
	result.eyeOffsetY = state.smoothedNormalizedY;
	result.eyeReliefDelta = state.smoothedNormalizedRelief;
	result.lensBasisXX = basisXX;
	result.lensBasisXY = basisXY;
	result.lensBasisZX = basisZX;
	result.lensBasisZY = basisZY;
	result.aimLensX = aimLensX;
	result.aimLensY = aimLensY;
	result.aimLensValid = aimLensValid;
	result.blend = std::clamp(activationProgress, 0.0F, 1.0F);
	result.valid = true;
	g_eyeBoxBailReason.store(0, std::memory_order_relaxed);
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

// Names that mark authored aiming marks, matched without regard to case.
//
// "Reticle" marks a whole subtree, because authors group etched marks, glow
// and recoil variants under a ReticleNode and every leaf belongs to the group.
// The rest match individual shapes only. An illuminated dot is commonly a
// sibling of Reticle:0 rather than a child of it, so a subtree rule never
// reaches it, but promoting a whole subtree on a token as short as "Dot" would
// sweep in anything incidentally named for one -- and a housing part pulled
// into the reticle set renders unmagnified over the sight picture.
constexpr std::string_view kReticleSubtreeToken = "Reticle";
constexpr std::string_view kReticleShapeTokens[] = { "Reticle", "Dot" };

[[nodiscard]] bool NameContainsTokenNoCase(
	std::string_view name,
	std::string_view token) noexcept
{
	if (token.empty() || name.size() < token.size()) {
		return false;
	}
	const auto equalNoCase = [](char left, char right) noexcept {
		return std::tolower(static_cast<unsigned char>(left)) ==
		       std::tolower(static_cast<unsigned char>(right));
	};
	return std::search(
			   name.begin(),
			   name.end(),
			   token.begin(),
			   token.end(),
			   equalNoCase) != name.end();
}

[[nodiscard]] bool NameHasPrefixNoCase(
	std::string_view name,
	std::string_view prefix) noexcept
{
	if (prefix.empty() || name.size() < prefix.size()) {
		return false;
	}
	for (std::size_t index = 0; index < prefix.size(); ++index) {
		if (std::tolower(static_cast<unsigned char>(name[index])) !=
			std::tolower(static_cast<unsigned char>(prefix[index]))) {
			return false;
		}
	}
	return true;
}

// Shapes that can serve as the aperture, in preference order.
//
// Authored names carry arbitrary numeric suffixes -- ScopeViewParts:378,
// ScopeAiming:78 -- so these are prefixes, matched without regard to case, and
// the discovered name is what the editor pins and the profile stores.
constexpr std::string_view kAperturePrefixes[] = {
	"ScopeFade",
	"ScopeViewParts",
	"ScopeAiming"
};

struct STSApertureCandidate
{
	RE::NiAVObject* shape{ nullptr };
	std::string name;
	// Index into kAperturePrefixes. Lower wins when choosing automatically.
	std::size_t rank{ 0U };
	// A 48-vertex, 48-triangle annulus, which is the only topology the exact
	// geometry replay can drive: its fill shader derives lens coordinates from
	// primitive order on that specific mesh. Anything else still supplies the
	// aperture's projection, eye box and mask.
	bool annulus{ false };
	// Named like an optical surface rather than structure. Third-party optics
	// that ship no ScopeFade still name their glass: specter_lens_rear,
	// hamr_lens_rear, LenseRearSTS. Without this the automatic choice takes
	// whichever shape came first, which on those meshes is the scope body.
	bool lensNamed{ false };
	// The eye-side element among them. That is the one the player looks
	// through, and it is where the aperture belongs.
	bool rearNamed{ false };
	float radius{ 0.0F };
};

// The first object under a root whose name begins with the given prefix.
//
// GetObjectByName matches the whole name, which misses every authored
// suffix -- ScopeAiming:78 is not "ScopeAiming" -- and it was the reason a
// scope built that way was rejected before any aperture search began.
RE::NiAVObject* FindObjectByPrefixNoCase(
	RE::NiAVObject* searchRoot,
	std::string_view prefix)
{
	if (!searchRoot) {
		return nullptr;
	}
	constexpr std::size_t kMaximumVisitedObjects = 512U;
	std::vector<RE::NiAVObject*> pending{ searchRoot };
	for (std::size_t cursor = 0;
		cursor < pending.size() && cursor < kMaximumVisitedObjects;
		++cursor) {
		auto* object = pending[cursor];
		if (!object) {
			continue;
		}
		if (NameHasPrefixNoCase(
				std::string_view{ object->name.c_str() },
				prefix)) {
			return object;
		}
		if (auto* node = object->IsNode()) {
			for (auto& childPointer : node->children) {
				if (auto* child = childPointer.get()) {
					pending.push_back(child);
				}
			}
		}
	}
	return nullptr;
}

// The aperture name the player pinned, or empty for automatic.
//
// The live editor wins while a preview is active so the dropdown takes effect
// without saving first; otherwise the selected profile is authoritative.
[[nodiscard]] const std::string& GetSelectedApertureSurfaceName()
{
	static std::string selected;
	// An API consumer's override outranks everything, including the editor.
	// It is the mechanism a double-pip optic uses to say "this frame, the
	// aperture is that other shape", and a scope-authoring UI has no business
	// contradicting it while it is set.
	if (auto apiOverride = MagnaScopeAPI::GetApertureOverride();
		!apiOverride.empty()) {
		selected = std::move(apiOverride);
		return selected;
	}
	const auto preview = ImGuiImpl::GetEditorPreviewSnapshot();
	if (preview.active) {
		selected = preview.apertureSurface;
		return selected;
	}
	const auto* current =
		ScopeData::ScopeDataHandler::GetSingleton()->GetCurrentScopeProfile();
	selected = current ? current->shaderData.apertureSurface : std::string{};
	return selected;
}

// Same contract for the aiming mark.
[[nodiscard]] const std::string& GetSelectedReticleSurfaceName()
{
	static std::string selected;
	if (auto apiOverride = MagnaScopeAPI::GetReticleOverride();
		!apiOverride.empty()) {
		selected = std::move(apiOverride);
		return selected;
	}
	const auto preview = ImGuiImpl::GetEditorPreviewSnapshot();
	if (preview.active) {
		selected = preview.reticleSurface;
		return selected;
	}
	const auto* current =
		ScopeData::ScopeDataHandler::GetSingleton()->GetCurrentScopeProfile();
	selected = current ? current->shaderData.reticleSurface : std::string{};
	return selected;
}

// Every usable aperture shape under the given root, best first.
//
// Only BSTriShapes qualify. An NiNode carries a transform and a bound but no
// geometry, so it can neither be replayed nor identified at draw time -- which
// is why matching the container named ScopeViewParts is not enough on its own.
std::vector<STSApertureCandidate> FindSTSApertureCandidates(
	RE::NiAVObject* scopeAiming)
{
	std::vector<STSApertureCandidate> candidates;
	if (!scopeAiming) {
		return candidates;
	}

	// Anchored to ScopeAiming, and every renderable shape beneath it counts.
	//
	// Two earlier attempts were wrong in opposite directions. Matching only
	// shape names missed scopes that hang their geometry under plain
	// ScopeAiming and ScopeViewParts nodes; searching the whole first-person
	// root instead swept in other attachments entirely -- receiver meshes from
	// unrelated weapons appeared in the list and were selected as the aperture,
	// and because that tree changes as attachments stream the choice flapped
	// between frames.
	//
	// ScopeAiming is the scope, so its subtree is the correct bound, and taking
	// descendants rather than direct children reaches shapes nested a level or
	// two down, which is where at least one weapon keeps them.
	constexpr std::size_t kMaximumVisitedObjects = 1024U;
	constexpr std::size_t kMaximumCandidates = 24U;

	struct PendingObject
	{
		RE::NiAVObject* object{ nullptr };
		bool insideViewParts{ false };
	};
	std::vector<PendingObject> pending{ { scopeAiming, false } };

	for (std::size_t cursor = 0;
		cursor < pending.size() && cursor < kMaximumVisitedObjects &&
			candidates.size() < kMaximumCandidates;
		++cursor) {
		auto* object = pending[cursor].object;
		if (!object) {
			continue;
		}
		const std::string_view name{ object->name.c_str() };
		const bool insideViewParts =
			pending[cursor].insideViewParts ||
			NameHasPrefixNoCase(name, "ScopeViewParts");

		if (auto* node = object->IsNode()) {
			for (auto& childPointer : node->children) {
				if (auto* child = childPointer.get()) {
					pending.push_back({ child, insideViewParts });
				}
			}
		}

		auto* shape = object->IsTriShape();
		if (!shape || !shape->rendererData) {
			continue;
		}
		const auto& bound = object->worldBound;
		if (!IsFinitePoint(bound.center) ||
			!std::isfinite(bound.fRadius) ||
			bound.fRadius <= 0.001F ||
			bound.fRadius >= 100000.0F) {
			continue;
		}

		// ScopeFade first, then anything under ScopeViewParts, then the rest of
		// the scope. The dropdown reorders by hand when this guesses wrong.
		const std::size_t rank =
			NameHasPrefixNoCase(name, "ScopeFade") ? 0U :
			insideViewParts                       ? 1U :
													2U;
		const bool lensNamed =
			NameContainsTokenNoCase(name, "lens") ||
			NameContainsTokenNoCase(name, "glass");
		candidates.push_back({
			object,
			std::string{ name },
			rank,
			shape->numTriangles == 48U && shape->numVertices == 48U,
			lensNamed,
			lensNamed && NameContainsTokenNoCase(name, "rear"),
			bound.fRadius
		});
	}

	// Structural rank first, as asked: ScopeFade, then ScopeViewParts, then
	// the rest of ScopeAiming. Within a rank, pick the shape most likely to be
	// the glass rather than whichever the tree happened to yield first -- on an
	// optic with no ScopeFade that was the scope body, which is why those
	// scopes magnified nothing usable.
	std::stable_sort(
		candidates.begin(),
		candidates.end(),
		[](const STSApertureCandidate& left,
			const STSApertureCandidate& right) {
			if (left.rank != right.rank) {
				return left.rank < right.rank;
			}
			// A real annulus is the only topology that can drive the exact
			// geometry replay, so it wins outright.
			if (left.annulus != right.annulus) {
				return left.annulus;
			}
			if (left.lensNamed != right.lensNamed) {
				return left.lensNamed;
			}
			if (left.rearNamed != right.rearNamed) {
				return left.rearNamed;
			}
			// Last resort: a lens is small next to the body it sits in.
			return left.radius < right.radius;
		});
	return candidates;
}

// Half-precision decode for packed vertex positions.
//
// Fallout stores positions as four 16-bit floats unless the shape carries
// VF_FULLPREC. Written out rather than pulled from DirectXMath so the same
// routine can be replayed by a host-side test with no D3D dependency.
float DecodeHalfFloat(std::uint16_t encoded)
{
	const std::uint32_t sign = static_cast<std::uint32_t>(encoded & 0x8000U)
	                           << 16U;
	std::int32_t exponent = static_cast<std::int32_t>((encoded >> 10) & 0x1FU);
	std::uint32_t mantissa = static_cast<std::uint32_t>(encoded & 0x3FFU);
	std::uint32_t bits = 0U;
	if (exponent == 0) {
		if (mantissa == 0U) {
			bits = sign;
		} else {
			// Subnormal. Shift the implied bit into place and pay for it in
			// the exponent, which is what makes the result a normal float.
			exponent = 1;
			while ((mantissa & 0x400U) == 0U) {
				mantissa <<= 1U;
				--exponent;
			}
			mantissa &= 0x3FFU;
			bits = sign |
			       (static_cast<std::uint32_t>(exponent + 112) << 23U) |
			       (mantissa << 13U);
		}
	} else if (exponent == 31) {
		bits = sign | 0x7F800000U | (mantissa << 13U);
	} else {
		bits = sign |
		       (static_cast<std::uint32_t>(exponent + 112) << 23U) |
		       (mantissa << 13U);
	}
	float result = 0.0F;
	std::memcpy(&result, &bits, sizeof(result));
	return result;
}

// What the aperture's own vertices say about its size, shape and orientation.
//
// Everything the optical path currently knows about aperture size is inferred
// from names and bounding spheres: a shape called Glass wins if it is 1.35x
// wider than the fade plane, the aiming housing is taken at 0.82 of its sphere,
// and the whole result is clamped between 1.5x and 4x. Those constants were
// fitted to ScopeFade and have no meaning on an arbitrary lens element.
//
// The vertices are the measurement those constants approximate. This reads
// them and reports; nothing consumes the result yet. The open question is
// whether Fallout keeps the CPU shadow copy alive for first-person weapon
// meshes at all -- BSGraphics::Buffer carries invalidCpuData precisely because
// it does not always -- and that is what this probe exists to answer.
struct ApertureVertexMeasurement
{
	bool measured{ false };
	// Why the measurement was not taken. Empty on success.
	const char* bailReason{ "" };

	bool dataPointerPresent{ false };
	bool cpuDataInvalid{ false };
	bool fullPrecision{ false };
	std::uint32_t vertexCount{ 0U };
	std::uint32_t stride{ 0U };
	std::uint32_t dataOffset{ 0U };
	std::uint32_t dataSize{ 0U };
	std::uint32_t maxDataSize{ 0U };
	std::uint64_t vertexDescriptor{ 0U };

	// Local-space, before the object's own scale. The mean of the vertex
	// positions, so it is weighted by tessellation density: a mesh with a
	// dense hub or asymmetric detail pulls it away from the geometric centre.
	// On specter_lever the pull is about 0.87 units, which is why this is
	// reported but not used to place anything.
	RE::NiPoint3 centroid{};
	// Midpoint of the vertex extents. For a circular opening this is the
	// centre regardless of how the ring is tessellated, which the mean is not,
	// so this is what the optical radii are measured from and what the
	// synthesized aperture is drawn around.
	RE::NiPoint3 opticalCenter{};
	// Half-extent about the centroid on each local axis. The smallest names
	// the optical axis, and the other two give the lens radius directly.
	RE::NiPoint3 halfExtents{};
	// In the plane normal to the thinnest axis.
	float outerRadius{ 0.0F };
	// Nonzero means a hole, which is the annulus question answered by
	// measurement instead of by a vertex count.
	float innerRadius{ 0.0F };
	// 0, 1 or 2 for local X, Y or Z.
	int thinnestAxis{ 1 };
	float worldScale{ 1.0F };
	// For comparison against the sphere the heuristics use today.
	float boundRadius{ 0.0F };
};

ApertureVertexMeasurement MeasureApertureVertices(RE::NiAVObject* object)
{
	ApertureVertexMeasurement measurement;
	auto* const shape = object ? object->IsTriShape() : nullptr;
	if (!shape) {
		measurement.bailReason = "not a BSTriShape";
		return measurement;
	}
	measurement.worldScale = object->world.scale;
	measurement.boundRadius = object->worldBound.fRadius;
	measurement.vertexCount = shape->numVertices;
	measurement.vertexDescriptor = shape->vertexDesc.desc;
	measurement.stride = shape->vertexDesc.GetSize();
	measurement.fullPrecision =
		shape->vertexDesc.HasFlag(RE::BSGraphics::Vertex::Flags::VF_FULLPREC);

	auto* const rendererShape = shape->rendererData ?
		static_cast<RE::BSGraphics::TriShape*>(shape->rendererData) :
		nullptr;
	auto* const vertexBuffer =
		rendererShape ? rendererShape->vertexBuffer : nullptr;
	if (!vertexBuffer) {
		measurement.bailReason = "no renderer vertex buffer";
		return measurement;
	}
	measurement.dataPointerPresent = vertexBuffer->data != nullptr;
	measurement.cpuDataInvalid = vertexBuffer->invalidCpuData;
	measurement.dataOffset = vertexBuffer->dataOffset;
	measurement.dataSize = vertexBuffer->dataSize;
	measurement.maxDataSize = vertexBuffer->maxDataSize;

	if (!measurement.dataPointerPresent) {
		measurement.bailReason = "CPU shadow copy absent";
		return measurement;
	}
	if (measurement.cpuDataInvalid) {
		measurement.bailReason = "CPU shadow copy marked invalid";
		return measurement;
	}
	if (measurement.vertexCount == 0U || measurement.stride < 8U) {
		measurement.bailReason = "degenerate vertex count or stride";
		return measurement;
	}

	// Never walk past the allocation.
	//
	// dataOffset is a byte offset into the pooled *GPU* buffer -- it is what
	// the draw classifier matches against IASetVertexBuffers -- and must not
	// be applied to the CPU pointer. data addresses this shape's own copy,
	// which the first probe run confirmed across every candidate on four
	// weapons: dataSize equalled numVertices * stride exactly every time,
	// with maxDataSize the padded allocation. Folding dataOffset in here
	// compared tens of millions against about a kilobyte and rejected every
	// shape, which is the only reason this check reported anything at all.
	const std::uint64_t span =
		static_cast<std::uint64_t>(measurement.vertexCount) *
		static_cast<std::uint64_t>(measurement.stride);
	const std::uint64_t limit = std::max(
		static_cast<std::uint64_t>(measurement.maxDataSize),
		static_cast<std::uint64_t>(measurement.dataSize));
	if (limit == 0U || span > limit) {
		measurement.bailReason = "vertex span exceeds reported buffer size";
		return measurement;
	}

	const auto* const base =
		static_cast<const std::uint8_t*>(vertexBuffer->data);

	const auto readPosition = [&](std::uint32_t index) {
		const auto* const vertex = base +
			static_cast<std::size_t>(index) *
				static_cast<std::size_t>(measurement.stride);
		RE::NiPoint3 position{};
		if (measurement.fullPrecision) {
			float components[3]{};
			std::memcpy(components, vertex, sizeof(components));
			position = { components[0], components[1], components[2] };
		} else {
			std::uint16_t components[3]{};
			std::memcpy(components, vertex, sizeof(components));
			position = {
				DecodeHalfFloat(components[0]),
				DecodeHalfFloat(components[1]),
				DecodeHalfFloat(components[2])
			};
		}
		return position;
	};

	RE::NiPoint3 minimum{
		std::numeric_limits<float>::max(),
		std::numeric_limits<float>::max(),
		std::numeric_limits<float>::max()
	};
	RE::NiPoint3 maximum{
		std::numeric_limits<float>::lowest(),
		std::numeric_limits<float>::lowest(),
		std::numeric_limits<float>::lowest()
	};
	RE::NiPoint3 sum{};
	std::uint32_t finiteCount = 0U;
	for (std::uint32_t index = 0U; index < measurement.vertexCount; ++index) {
		const RE::NiPoint3 position = readPosition(index);
		if (!IsFinitePoint(position)) {
			continue;
		}
		minimum.x = std::min(minimum.x, position.x);
		minimum.y = std::min(minimum.y, position.y);
		minimum.z = std::min(minimum.z, position.z);
		maximum.x = std::max(maximum.x, position.x);
		maximum.y = std::max(maximum.y, position.y);
		maximum.z = std::max(maximum.z, position.z);
		sum += position;
		++finiteCount;
	}
	if (finiteCount == 0U) {
		measurement.bailReason = "no finite positions decoded";
		return measurement;
	}

	const float inverseCount = 1.0F / static_cast<float>(finiteCount);
	measurement.centroid = {
		sum.x * inverseCount,
		sum.y * inverseCount,
		sum.z * inverseCount
	};
	measurement.halfExtents = {
		0.5F * (maximum.x - minimum.x),
		0.5F * (maximum.y - minimum.y),
		0.5F * (maximum.z - minimum.z)
	};
	measurement.opticalCenter = {
		0.5F * (minimum.x + maximum.x),
		0.5F * (minimum.y + maximum.y),
		0.5F * (minimum.z + maximum.z)
	};

	// The thinnest axis is the optical axis. A lens is a disc, so two extents
	// agree and the third collapses; picking the smallest identifies it
	// without assuming the local X/Z convention ScopeFade happens to use.
	const float extents[3]{
		measurement.halfExtents.x,
		measurement.halfExtents.y,
		measurement.halfExtents.z
	};
	measurement.thinnestAxis = 0;
	for (int axis = 1; axis < 3; ++axis) {
		if (extents[axis] < extents[measurement.thinnestAxis]) {
			measurement.thinnestAxis = axis;
		}
	}

	float outerRadius = 0.0F;
	float innerRadius = std::numeric_limits<float>::max();
	for (std::uint32_t index = 0U; index < measurement.vertexCount; ++index) {
		const RE::NiPoint3 position = readPosition(index);
		if (!IsFinitePoint(position)) {
			continue;
		}
		// Measured from the extent midpoint, not the mean, so the radius and
		// the centre the aperture is drawn at describe the same circle. Using
		// the mean here is what let outerRadius exceed the largest distance
		// the bounding box permits -- 3.5936 against 2.724 on specter_lever.
		const RE::NiPoint3 offset = position - measurement.opticalCenter;
		const float components[3]{ offset.x, offset.y, offset.z };
		float squared = 0.0F;
		for (int axis = 0; axis < 3; ++axis) {
			if (axis == measurement.thinnestAxis) {
				continue;
			}
			squared += components[axis] * components[axis];
		}
		const float radius = std::sqrt(squared);
		outerRadius = std::max(outerRadius, radius);
		innerRadius = std::min(innerRadius, radius);
	}
	measurement.outerRadius = outerRadius;
	measurement.innerRadius =
		innerRadius == std::numeric_limits<float>::max() ? 0.0F : innerRadius;
	measurement.measured = true;
	return measurement;
}

// Decoding a shape's whole vertex buffer is not free: acogSTS:1 is 3,924
// vertices, the candidate sweep touches eight shapes at once, and both callers
// run every frame the optic is live.
//
// That became a game-thread stall rather than a cost, because the candidate
// list reorders whenever worldBound.fRadius transiently collapses to exactly
// 1.0 for every shape simultaneously. The reorder reads as "the list changed",
// which re-probes all of it and writes a line per shape. During a save-game
// load, where bounds are rebuilt continuously, that repeats every frame and
// the game appears to hang with an STS scope drawn.
//
// Model-space vertex extents are static for a given shape, so measure once.
// The key carries the vertex identity as well as the address: a freed shape's
// pointer can be reused, and any mismatch re-measures rather than trusting a
// stale entry. Failed measurements are not cached -- they bail before the
// vertex loop, so retrying them is cheap, and a transient failure during a
// load must not be remembered as permanent.
struct ApertureMeasurementCacheEntry
{
	const void* shape{ nullptr };
	const void* rendererData{ nullptr };
	std::uint32_t vertexCount{ 0U };
	std::uint64_t vertexDescriptor{ 0U };
	ApertureVertexMeasurement measurement;
};

std::vector<ApertureMeasurementCacheEntry> g_apertureMeasurementCache;

ApertureVertexMeasurement MeasureApertureVerticesCached(RE::NiAVObject* object)
{
	auto* const shape = object ? object->IsTriShape() : nullptr;
	if (!shape) {
		return MeasureApertureVertices(object);
	}
	const void* const rendererData = shape->rendererData;
	const std::uint32_t vertexCount = shape->numVertices;
	const std::uint64_t descriptor = shape->vertexDesc.desc;
	for (const auto& entry : g_apertureMeasurementCache) {
		if (entry.shape == shape &&
			entry.rendererData == rendererData &&
			entry.vertexCount == vertexCount &&
			entry.vertexDescriptor == descriptor) {
			return entry.measurement;
		}
	}
	auto measurement = MeasureApertureVertices(object);
	if (!measurement.measured) {
		return measurement;
	}
	// A session switches weapons and attachments many times and each one would
	// otherwise leave an entry behind for good.
	if (g_apertureMeasurementCache.size() >= 64U) {
		g_apertureMeasurementCache.clear();
	}
	g_apertureMeasurementCache.push_back(
		{ shape, rendererData, vertexCount, descriptor, measurement });
	return measurement;
}

// Whether a measured shape is plausibly a lens or aperture disc: flat along
// one axis, roughly circular in the other two, not a wire ring, and not a
// tiny decoration. The thresholds are generous on purpose -- this only
// decides which mesh sizes and places the synthesized optic, and the log
// shows real lenses passing comfortably (specter_lens_rear thinness 0.06)
// while bodies and levers fail by an order of magnitude.
bool IsDiscLikeMeasurement(const ApertureVertexMeasurement& measurement)
{
	if (!measurement.measured) {
		return false;
	}
	const float extents[3]{
		measurement.halfExtents.x,
		measurement.halfExtents.y,
		measurement.halfExtents.z
	};
	const float thin = extents[measurement.thinnestAxis];
	float planeA = 0.0F;
	float planeB = 0.0F;
	int seen = 0;
	for (int axis = 0; axis < 3; ++axis) {
		if (axis == measurement.thinnestAxis) {
			continue;
		}
		(seen++ == 0 ? planeA : planeB) = extents[axis];
	}
	const float minPlane = std::min(planeA, planeB);
	const float maxPlane = std::max(planeA, planeB);
	if (minPlane <= 0.001F || thin > 0.4F * minPlane) {
		return false;
	}
	if (minPlane < 0.7F * maxPlane) {
		return false;
	}
	if (measurement.outerRadius < 0.4F) {
		// Red-dot markers and similar decorations are perfect little discs.
		// A usable optical opening on the inspected corpus is never under
		// 0.4 model units (the smallest genuine one is acogPiece at 0.66).
		return false;
	}
	const float innerRatio =
		measurement.outerRadius > 0.0001F ?
			measurement.innerRadius / measurement.outerRadius :
			0.0F;
	// A wire ring is a hole with no glass -- specter_reticle reads 0.994.
	return innerRatio < 0.92F;
}

// Copy the discovered names to the editor so its dropdown lists what this
// weapon actually has. Republished only on change: this runs every frame the
// optic is live, and the list is stable for a given scope.
void PublishApertureCandidates(
	const std::vector<STSApertureCandidate>& candidates)
{
	static std::vector<std::string> lastPublished;
	std::vector<std::string> names;
	names.reserve(candidates.size());
	for (const auto& candidate : candidates) {
		names.push_back(candidate.name);
	}
	// Compare as a set, not a sequence. The ranking's last tie-break is the
	// bounding-sphere radius, and that field collapses to exactly 1.0 for
	// every shape at once in bursts, which ties the comparison and lets
	// stable_sort fall back to tree-walk order. The membership is identical
	// either way, so treating a reorder as a new scope re-probed every mesh
	// and logged the whole list, every frame, for as long as the burst lasted.
	std::vector<std::string> sortedNames = names;
	std::sort(sortedNames.begin(), sortedNames.end());
	if (sortedNames == lastPublished) {
		return;
	}
	lastPublished = std::move(sortedNames);

	// The list changed, so this is a different scope. Record what it offers:
	// when a weapon shows nothing, the difference between "no candidates" and
	// "candidates found but the wrong one chosen" is the whole diagnosis.
	std::string summary;
	for (const auto& candidate : candidates) {
		if (!summary.empty()) {
			summary += ", ";
		}
		summary += candidate.name;
		summary += candidate.annulus ? " (annulus)" : " (plain)";
	}
	logger::verbose("Aperture candidates: {}", summary);

	// Read-only probe. Whether the CPU vertex shadow survives for first-person
	// weapon meshes decides whether the aperture can be measured directly or
	// has to be read back from the GPU, so report every candidate rather than
	// only the one that won -- a scope where the chosen shape has no shadow
	// copy but a sibling does is a different problem than none of them having
	// it. Bounded by kMaximumCandidates and fires only on a scope change.
	for (const auto& candidate : candidates) {
		const auto measurement = MeasureApertureVerticesCached(candidate.shape);
		if (!measurement.measured) {
			logger::verbose(
				"Aperture vertex probe '{}': UNAVAILABLE ({}); "
				"dataPointer={}, invalidCpuData={}, vertices={}, stride={}, "
				"fullPrecision={}, dataOffset={}, dataSize={}, maxDataSize={}, "
				"vertexDesc=0x{:016X}, boundRadius={:.4f}",
				candidate.name,
				measurement.bailReason,
				measurement.dataPointerPresent,
				measurement.cpuDataInvalid,
				measurement.vertexCount,
				measurement.stride,
				measurement.fullPrecision,
				measurement.dataOffset,
				measurement.dataSize,
				measurement.maxDataSize,
				measurement.vertexDescriptor,
				measurement.boundRadius);
			continue;
		}
		constexpr const char* kAxisNames[3]{ "X", "Y", "Z" };
		logger::verbose(
			"Aperture vertex probe '{}': vertices={}, stride={}, "
			"fullPrecision={}, centroid=({:.4f}, {:.4f}, {:.4f}), "
			"halfExtents=({:.4f}, {:.4f}, {:.4f}), opticalAxis=local{}, "
			"outerRadius={:.4f}, innerRadius={:.4f}, hole={}, "
			"worldScale={:.4f}, scaledOuterRadius={:.4f}, "
			"boundRadius={:.4f}, boundOverMeasured={:.3f}",
			candidate.name,
			measurement.vertexCount,
			measurement.stride,
			measurement.fullPrecision,
			measurement.centroid.x,
			measurement.centroid.y,
			measurement.centroid.z,
			measurement.halfExtents.x,
			measurement.halfExtents.y,
			measurement.halfExtents.z,
			kAxisNames[measurement.thinnestAxis],
			measurement.outerRadius,
			measurement.innerRadius,
			measurement.outerRadius > 0.0F &&
				measurement.innerRadius > 0.25F * measurement.outerRadius,
			measurement.worldScale,
			measurement.outerRadius * measurement.worldScale,
			measurement.boundRadius,
			measurement.outerRadius > 0.0001F ?
				measurement.boundRadius /
					(measurement.outerRadius * measurement.worldScale) :
				0.0F);
	}

	std::vector<ImGuiImpl::ApertureCandidateInfo> published;
	published.reserve(candidates.size());
	for (const auto& candidate : candidates) {
		published.push_back({ candidate.name, candidate.annulus });
	}
	ImGuiImpl::PublishApertureCandidates(published);
}

// The single renderable shape with this exact name, or null. Exact rather than
// prefix or token matching: the name came from the candidate dropdown, which is
// populated from these same objects, so anything less than an exact match would
// resolve to a shape the user never picked.
[[nodiscard]] RE::NiAVObject* FindShapeByExactName(
	RE::NiAVObject* root,
	std::string_view target)
{
	if (!root || target.empty()) {
		return nullptr;
	}
	std::vector<RE::NiAVObject*> pending{ root };
	constexpr std::size_t kMaximumVisitedObjects = 512U;
	for (std::size_t cursor = 0;
		 cursor < pending.size() && cursor < kMaximumVisitedObjects;
		 ++cursor) {
		auto* object = pending[cursor];
		if (!object) {
			continue;
		}
		if (std::string_view{ object->name.c_str() } == target) {
			if (auto* shape = object->IsTriShape();
				shape && shape->rendererData) {
				return object;
			}
		}
		if (auto* node = object->IsNode()) {
			for (auto& childPointer : node->children) {
				if (auto* child = childPointer.get()) {
					pending.push_back(child);
				}
			}
		}
	}
	return nullptr;
}

std::vector<RE::NiAVObject*> FindSTSReticleSurfaces(
	RE::NiAVObject* scopeViewParts)
{
	std::vector<RE::NiAVObject*> result;
	if (!scopeViewParts) {
		return result;
	}

	// A pinned name replaces the naming rules outright rather than adding to
	// them. Pinning exists precisely because the automatic rules found the
	// wrong shape or no shape, so letting them keep contributing would leave
	// the mistake in place alongside the correction. An unmatched name falls
	// through to automatic below instead of leaving the scope with no reticle.
	const auto& pinnedReticle = GetSelectedReticleSurfaceName();
	if (!pinnedReticle.empty()) {
		if (auto* pinned = FindShapeByExactName(scopeViewParts, pinnedReticle)) {
			result.push_back(pinned);
			return result;
		}
		static std::string lastMissingReticlePin;
		if (lastMissingReticlePin != pinnedReticle) {
			lastMissingReticlePin = pinnedReticle;
			logger::warn(
				"Pinned reticle surface '{}' is not present on this scope; "
				"falling back to automatic detection",
				pinnedReticle);
		}
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
		// Prefix, not substring. "Dot" and "Reticle" appear inside plenty of
		// incidental names in weapon-mod meshes, and a housing part swept into
		// the reticle set renders unmagnified over the sight picture. Automatic
		// detection is deliberately narrow now that the surface can also be
		// chosen by hand; a mesh that names its aiming mark something else is a
		// case for the dropdown, not for a looser rule that mis-fires on
		// everything else.
		const bool insideReticleSubtree =
			pending[cursor].insideReticleSubtree ||
			NameHasPrefixNoCase(name, kReticleSubtreeToken);
		bool namedAimingMark = false;
		for (const auto token : kReticleShapeTokens) {
			if (NameHasPrefixNoCase(name, token)) {
				namedAimingMark = true;
				break;
			}
		}
		if (insideReticleSubtree || namedAimingMark) {
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

	// Matched by prefix, because authored names carry arbitrary suffixes.
	// Exact-name lookups rejected ScopeAiming:78 outright, and requiring
	// ScopeViewParts to be a node rejected every scope that authors it as a
	// shape -- both before any aperture search could run, which is why those
	// weapons showed no scope at all rather than falling back.
	auto* scopeAiming = FindObjectByPrefixNoCase(firstPersonRoot, "ScopeAiming");
	if (!scopeAiming) {
		return {};
	}
	auto* scopeViewPartsObject =
		FindObjectByPrefixNoCase(scopeAiming, "ScopeViewParts");

	// Reticle, aim and extent lookups need a container to search. Prefer
	// ScopeViewParts when it is one, otherwise fall back to ScopeAiming: a
	// ScopeViewParts authored as a shape has no children to search, but its
	// siblings under ScopeAiming still hold the reticle.
	RE::NiAVObject* partsRoot =
		scopeViewPartsObject && scopeViewPartsObject->IsNode() ?
			scopeViewPartsObject :
			scopeAiming;

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

	// ScopeFade is the preferred optical plane, but not every scope ships one.
	// Search ScopeAiming's whole subtree for any usable aperture shape and take
	// the best available, unless the profile pins one by name.
	//
	// Do not substitute optional Glass, Lens, ScreenWarp or EdgeBlur here:
	// Stage 4c proved those differ between scope NIFs. The candidates are
	// limited to the three structural names STS itself defines.
	auto apertureCandidates = FindSTSApertureCandidates(scopeAiming);
	if (apertureCandidates.empty()) {
		// A scope with no usable aperture shape produces nothing at all, which
		// is indistinguishable from the plugin being off. Name it once.
		static std::once_flag loggedNoCandidates;
		std::call_once(loggedNoCandidates, [scopeAiming] {
			logger::warn(
				"No renderable shape found anywhere under '{}'; this scope "
				"cannot supply an aperture",
				scopeAiming->name.c_str());
		});
		return {};
	}
	PublishApertureCandidates(apertureCandidates);

	const auto& pinnedName = GetSelectedApertureSurfaceName();
	const STSApertureCandidate* chosen = nullptr;
	if (!pinnedName.empty()) {
		for (const auto& candidate : apertureCandidates) {
			if (candidate.name == pinnedName) {
				chosen = &candidate;
				break;
			}
		}
		if (!chosen) {
			// A pinned name that is absent on this weapon must not disable the
			// optic. Fall through to the automatic choice and say so once.
			static std::once_flag loggedMissingPin;
			std::call_once(loggedMissingPin, [&pinnedName] {
				logger::warn(
					"Pinned aperture surface '{}' is not present on this "
					"scope; falling back to automatic selection",
					pinnedName);
			});
		}
	}
	if (!chosen) {
		chosen = &apertureCandidates.front();
	}

	RE::NiAVObject* opticalPlane = chosen->shape;
	if (!validBound(opticalPlane)) {
		return {};
	}
	RE::NiAVObject* renderSurface = opticalPlane;
	// Only the standardized annulus can drive the exact geometry replay. Say
	// which mode this scope is in rather than leaving a silently wrong lens:
	// the fill shader assigns lens coordinates from primitive order across 24
	// segments, so on any other mesh those coordinates are meaningless.
	if (!chosen->annulus) {
		static std::once_flag loggedNonAnnulusAperture;
		std::call_once(loggedNonAnnulusAperture, [chosen] {
			logger::warn(
				"Aperture '{}' is not a 48-vertex ScopeFade annulus; it "
				"supplies projection, eye box and mask, but the exact "
				"geometry replay is unavailable on this topology",
				chosen->name);
		});
	}
	const auto& planeBound = opticalPlane->worldBound;
	const float planeRadius = planeBound.fRadius;

	// Measure the chosen shape itself. This is the size the synthesized
	// aperture path will draw at, so it is taken from the vertices rather than
	// from the bounding sphere -- which is both a heuristic and, in bursts,
	// exactly 1.0 for every shape at once while the scene graph updates.
	// Cached: this runs every frame the optic is live, and re-decoding the
	// selected mesh's whole vertex buffer per frame was a stall of its own on
	// top of the candidate-sweep thrash above.
	const auto planeMeasurement = MeasureApertureVerticesCached(opticalPlane);

	// The synthesized optic is sized and placed by a disc, not by whatever
	// happens to be selected. Selecting the scope body drew a body-radius
	// black circle over half the screen; the selection keeps driving
	// projection and eye box, but the optic itself comes from the most
	// lens-like shape available.
	RE::NiAVObject* synthesisSource = opticalPlane;
	ApertureVertexMeasurement synthesisMeasurement = planeMeasurement;
	if (!IsDiscLikeMeasurement(planeMeasurement)) {
		int bestScore = std::numeric_limits<int>::min();
		for (const auto& candidate : apertureCandidates) {
			if (!candidate.shape || candidate.shape == opticalPlane) {
				continue;
			}
			const auto measurement =
				MeasureApertureVerticesCached(candidate.shape);
			if (!IsDiscLikeMeasurement(measurement)) {
				continue;
			}
			const std::string_view name{ candidate.name };
			int score = 0;
			const auto containsNoCase = [&name](std::string_view needle) {
				return std::search(
						   name.begin(),
						   name.end(),
						   needle.begin(),
						   needle.end(),
						   [](char a, char b) {
							   return std::tolower(
										  static_cast<unsigned char>(a)) ==
							          std::tolower(
										  static_cast<unsigned char>(b));
						   }) != name.end();
			};
			if (containsNoCase("lens") || containsNoCase("glass")) {
				score += 4;
			}
			if (containsNoCase("fade")) {
				score += 3;
			}
			const float innerRatio =
				measurement.outerRadius > 0.0001F ?
					measurement.innerRadius / measurement.outerRadius :
					0.0F;
			if (innerRatio < 0.2F || (innerRatio > 0.3F && innerRatio < 0.7F)) {
				// A solid glass disc or a ScopeFade-proportioned annulus.
				score += 2;
			}
			if (score > bestScore ||
				(score == bestScore &&
					measurement.outerRadius <
						synthesisMeasurement.outerRadius)) {
				bestScore = score;
				synthesisSource = candidate.shape;
				synthesisMeasurement = measurement;
			}
		}
		if (synthesisSource != opticalPlane) {
			static const void* lastSubstitutionLogged = nullptr;
			if (lastSubstitutionLogged != synthesisSource) {
				lastSubstitutionLogged = synthesisSource;
				logger::info(
					"Selected aperture '{}' is not disc-shaped; the "
					"synthesized optic will be sized and placed by '{}' "
					"(radius={:.4f})",
					chosen->name,
					synthesisSource->name.c_str(),
					synthesisMeasurement.outerRadius);
			}
		}
	}

	const float measuredWorldRadius =
		synthesisMeasurement.outerRadius *
		std::abs(synthesisMeasurement.worldScale);
	const bool measurementValid =
		synthesisMeasurement.measured && std::isfinite(measuredWorldRadius) &&
		measuredWorldRadius > 0.001F && measuredWorldRadius < 10000.0F;
	const float measuredInnerRatio =
		synthesisMeasurement.outerRadius > 0.0001F ?
			synthesisMeasurement.innerRadius /
				synthesisMeasurement.outerRadius :
			0.0F;

	// Search from the same root the aperture candidates are enumerated from.
	// ScopeViewParts is a child of ScopeAiming, and an authored Reticle:0 is
	// commonly a sibling of it rather than inside it -- so the narrower root
	// offered Reticle:0 in the dropdown and then could not resolve it, and
	// automatic detection never saw it either. The prefix rule is what keeps
	// the wider root from sweeping anything in.
	auto reticleSurfaces =
		FindSTSReticleSurfaces(scopeAiming ? scopeAiming : partsRoot);
	// Same root as the reticle surfaces, and for the same reason: an authored
	// Reticle:0 is commonly a sibling of ScopeViewParts rather than a child of
	// it, so searching the narrower root silently fell through to opticalPlane
	// and made the aim reference the aperture itself. Everything downstream
	// then treats the lens plane as the point of aim -- including the zoom
	// conversion, which centred the optic instead of the reticle. The
	// IsDescendantOf guard below still keeps the result inside ScopeAiming.
	RE::NiAVObject* const aimSearchRoot =
		scopeAiming ? scopeAiming : partsRoot;
	// A pinned reticle is the point of aim by definition. Without this the
	// dropdown would correct which shape is drawn as the reticle while the
	// conversion and eye box kept aiming at whatever the naming rules found.
	RE::NiAVObject* aimReference = nullptr;
	if (const auto& pinnedAim = GetSelectedReticleSurfaceName();
		!pinnedAim.empty()) {
		aimReference = FindShapeByExactName(aimSearchRoot, pinnedAim);
	}
	if (!aimReference) {
		aimReference = FindObjectByPrefixNoCase(aimSearchRoot, "ReticleNode");
	}
	if (!aimReference || !IsDescendantOf(aimReference, scopeAiming) ||
		!validBound(aimReference)) {
		aimReference = FindObjectByPrefixNoCase(aimSearchRoot, "Reticle");
	}
	if (!aimReference || !IsDescendantOf(aimReference, scopeAiming) ||
		!validBound(aimReference)) {
		aimReference = FindObjectByPrefixNoCase(aimSearchRoot, "Dot");
	}
	if (!aimReference ||
		!IsDescendantOf(aimReference, scopeAiming) ||
		!validBound(aimReference)) {
		aimReference = opticalPlane;
	}

	RE::NiAVObject* extentReference = nullptr;
	for (const char* extentName :
		{ "Glass", "ScreenWarp", "EdgeBlur" }) {
		auto* candidate = FindObjectByPrefixNoCase(partsRoot, extentName);
		if (!candidate ||
			!IsDescendantOf(candidate, scopeAiming) ||
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

	// Both numbers side by side. The ratio between them is what the synthesized
	// path has to justify: the heuristic is what the eye box normalizes
	// against, the measurement is the physical rim, and on ScopeFade they
	// differ by the hardcoded 3.0.
	//
	// Once per selected shape, not once per process. This was std::call_once,
	// which reported only the session's first scope and made a later scope
	// producing no optic impossible to diagnose from the log.
	static const void* lastSizedAperture = nullptr;
	if (measurementValid && lastSizedAperture != opticalPlane) {
		lastSizedAperture = opticalPlane;
		{
			const RE::NiPoint3 centreShift =
				synthesisMeasurement.opticalCenter -
				synthesisMeasurement.centroid;
			logger::verbose(
				"Aperture sizing for '{}' (optic from '{}'): "
				"heuristic={:.4f}, measured={:.4f}, "
				"heuristicOverMeasured={:.3f}, innerRatio={:.3f}, "
				"opticalAxis=local{}, meanToExtentShift={:.4f}",
				chosen->name,
				synthesisSource ? synthesisSource->name.c_str() : "<none>",
				apertureRadius,
				measuredWorldRadius,
				measuredWorldRadius > 0.0001F ?
					apertureRadius / measuredWorldRadius :
					0.0F,
				measuredInnerRatio,
				synthesisMeasurement.thinnestAxis == 0 ?
					"X" :
					(synthesisMeasurement.thinnestAxis == 1 ? "Y" : "Z"),
				std::sqrt(
					centreShift.x * centreShift.x +
					centreShift.y * centreShift.y +
					centreShift.z * centreShift.z));
		}
	}

	return {
		opticalPlane,
		renderSurface,
		chosen->annulus,
		aimReference,
		std::move(reticleSurfaces),
		extentReference,
		planeBound.center,
		previousWorldCenter,
		aimReference->worldBound.center,
		apertureRadius,
		measurementValid,
		measuredWorldRadius,
		measuredInnerRatio,
		synthesisMeasurement.thinnestAxis,
		synthesisMeasurement.opticalCenter,
		synthesisSource,
		synthesisMeasurement.outerRadius,
		planeMeasurement.measured ? planeMeasurement.outerRadius : 0.0F
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
// Defined with the runtime sight-zoom machinery below; needed by the session
// teardown above it.
void RestoreSightZoomPointer();
bool hasOriginalZoomData = false;
bool selectedZoomOverrideApplied = false;
// True between kPreSaveGame and kPostSaveGame. DetachIsolatedZoomForSave lifts
// MagnaScope's values out of the shared zoom form so they are not written into
// the save; anything that writes them back per tick -- the variant resolver
// above all -- must stand down for that window or it would undo the detach one
// frame later and serialise the override anyway.
bool savingInProgress = false;

// Defined below, next to the rest of the reticle-discovery cache; declared here
// because profile selection needs them and runs earlier in the file.
[[nodiscard]] const std::vector<std::string>& ReticlesForSelectedScope();
void RediscoverReticles();
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

uintptr_t PCUpdateMainThreadOrig;

BGSKeyword* ChangeAnimFlavorKeyword = nullptr;
ScopeData::ScopeProfile* currentData;
const char* customPath = "Data\\F4SE\\Plugins\\MagnaScope\\Auto";

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

// Whether the player is looking through their own eyes.
//
// The optic is a first-person effect built entirely out of the first-person
// rig: the aperture, the reticle and the eye box are all weapon geometry
// measured against the first-person Camera node. From a detached camera none
// of that is what fills the screen, so compositing spends the geometry replay
// and the reticle capture drawing a lens over a view it does not belong to.
//
// Only the cameras that genuinely leave the player's eyes are rejected.
// kIronSights and kFirstPerson are the optic's normal home; kPCTransition,
// kTween and kAnimated are what the camera passes through entering and
// leaving iron sights, and rejecting those would blink the optic off for the
// frames either side of every ADS. kFurniture, kMount, kDialogue and
// kBleedout all still render first person in Fallout 4 and are left alone
// deliberately.
[[nodiscard]] bool IsFirstPersonCameraView()
{
	const auto* camera = RE::PlayerCamera::GetSingleton();
	const auto state = camera ? camera->GetCameraCurrentState() : nullptr;
	if (!state) {
		// Fail open. An unreadable camera state must not be able to switch
		// the optic off; that would be a much louder bug than rendering it
		// one frame too long.
		return true;
	}
	// Compared rather than switched: id is a REX::TEnum wrapper, which has
	// equality but no implicit conversion to a switch expression.
	return state->id != RE::CameraStates::k3rdPerson &&
	       state->id != RE::CameraStates::kAutoVanity &&
	       state->id != RE::CameraStates::kFree;
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
	// The pointer swap must be undone while the instance is still known to be
	// alive; the field restores below then operate on the authored form the
	// engine is once again reading.
	RestoreSightZoomPointer();
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

// --- runtime secondary-sight zoom forms ------------------------------------
//
// One runtime BGSZoomData per sight slot, created through the engine's own
// form factory and reused across profiles (their contents are rewritten on
// every activation). While a secondary sight is up, the weapon INSTANCE's
// zoomData pointer is swapped to the slot's form -- instance-scoped, so the
// shared authored zoom form and every other user of it stay untouched. The
// pointer is restored on swap-back, unequip, and around saves so a runtime
// FormID can never leak into a save.
RE::BGSZoomData* AcquireRuntimeSightZoom(std::size_t index)
{
	static std::vector<RE::BGSZoomData*> pool;
	while (pool.size() <= index) {
		auto factories = RE::IFormFactory::GetFormFactories();
		auto* factory =
			factories[std::to_underlying(RE::ENUM_FORM_ID::kZOOM)];
		auto* created = factory ?
			static_cast<RE::BGSZoomData*>(factory->DoCreate()) :
			nullptr;
		if (!created) {
			logger::warn(
				"Runtime BGSZoomData creation failed; secondary-sight zoom "
				"pointer swaps are unavailable this session");
			return nullptr;
		}
		pool.push_back(created);
	}
	return pool[index];
}

// The runtime form currently installed on the instance, null when the
// authored form is in place. Game thread only.
RE::BGSZoomData* installedSightZoom = nullptr;

// Fires the configured animation-graph events at the player. Confirmed in
// game: the engine samples ZoomData when the graph is poked (GunUp), not
// only at aim-in, so this is what makes a pointer swap or a per-tick value
// write actually reach the eye mid-ADS. Configured via
// [Sights] SightSwapGraphEvents in MagnaScope.ini (semicolon separated);
// unknown event names are ignored by the graph.
//
// The key is re-read from the INI on EVERY firing, deliberately: it keeps
// candidate event names testable with no restart, and even the per-tick
// transition burst is a few dozen reads of a small cached file.
//
// logEachEvent quiets the per-firing log lines for the transition burst,
// which pokes the graph every tick for the blend's duration; the burst is
// bounded in the log by the transition started/settled lines instead.
void FireSightSwapGraphEvents(bool logEachEvent = true)
{
	if (!player) {
		return;
	}
	std::string events;
	{
		wchar_t wide[256]{};
		GetPrivateProfileStringW(
			L"Sights",
			L"SightSwapGraphEvents",
			L"GunUp",
			wide,
			static_cast<DWORD>(std::size(wide)),
			(GetPluginDirectory() / L"MagnaScope.ini").c_str());
		for (const wchar_t* cursor = wide; *cursor != L'\0'; ++cursor) {
			events.push_back(static_cast<char>(*cursor & 0x7F));
		}
	}
	std::size_t begin = 0U;
	while (begin < events.size()) {
		auto end = events.find(';', begin);
		if (end == std::string::npos) {
			end = events.size();
		}
		const auto token = events.substr(begin, end - begin);
		begin = end + 1U;
		if (token.empty()) {
			continue;
		}
		const bool handled = player->NotifyAnimationGraphImpl(
			RE::BSFixedString(token.c_str()));
		if (logEachEvent) {
			logger::verbose(
				"[sight] graph event '{}' fired (handled={})",
				token,
				handled);
		}
	}
}

// Keeps the instance's zoom pointer in step with the selected sight. Runs
// once per game tick; a no-op every tick nothing changed. Declarative on
// purpose: scroll swaps, co-save restores, and profile reselects all funnel
// through the same comparison, so there is no path that can miss a restore.
void ReconcileSightZoomPointer()
{
	if (!settings.AllowsOverrides() || zoomOverrideSuspendedForSave ||
		!currentData || !originalZoomInstanceOwner || !originalZoomInstance ||
		originalZoomInstanceOwner.get() != originalZoomInstance ||
		!originalZoomForm || !hasOriginalZoomData) {
		return;
	}
	// Hands off if a third party re-pointed the instance somewhere we have
	// never seen; fighting over the pointer helps nobody.
	if (originalZoomInstance->zoomData != originalZoomForm &&
		originalZoomInstance->zoomData != installedSightZoom) {
		return;
	}

	auto& state = MagnaScope::SessionStateFor(*currentData);
	// The pointer follows the BLEND, not the raw selection: while the ease
	// is still gliding back to the primary optic the runtime form must stay
	// installed, because it is the form receiving the per-tick lerped values
	// the engine keeps re-sampling (DriveSightZoomTransition). Only a
	// settled blend restores the authored pointer.
	const int blendSight = state.secondaryIndex >= 0 ?
	                           state.secondaryIndex :
	                           (state.sightBlend > 0.0001F ?
	                                state.lastSightIndex :
	                                -1);
	RE::BGSZoomData* desired = originalZoomForm;
	if (blendSight >= 0 &&
		blendSight < static_cast<int>(currentData->secondarySights.size())) {
		auto* runtime = AcquireRuntimeSightZoom(
			static_cast<std::size_t>(blendSight));
		if (runtime) {
			// Seed with what the engine is looking at RIGHT NOW -- the live
			// authored form, which already carries this tick's override
			// values -- so the first re-sample after the swap sees no
			// discontinuity. The transition writer then moves the values
			// every tick; reseeding while installed would fight it.
			if (originalZoomInstance->zoomData != runtime) {
				runtime->zoomData = originalZoomForm->zoomData;
				runtime->isMod = originalZoomForm->isMod;
			}
			desired = runtime;
		}
	}

	if (originalZoomInstance->zoomData != desired) {
		originalZoomInstance->zoomData = desired;
		installedSightZoom =
			desired == originalZoomForm ? nullptr : desired;
		logger::verbose(
			"[sight] instance zoom pointer -> {} (sight index {})",
			desired == originalZoomForm ? "authored form" : "runtime form",
			state.secondaryIndex);
		FireSightSwapGraphEvents();
	}
}

// Restores the authored pointer immediately. For paths that end the zoom
// session or serialize state and cannot wait for the next reconcile tick.
void RestoreSightZoomPointer()
{
	if (originalZoomInstance && originalZoomForm && installedSightZoom &&
		originalZoomInstance->zoomData == installedSightZoom) {
		originalZoomInstance->zoomData = originalZoomForm;
		logger::verbose(
			"[sight] instance zoom pointer restored to the authored form");
	}
	installedSightZoom = nullptr;
}

// Turns the pointer swap from a snap into a glide. The resolver eases
// sightBlend and publishes lerped zoom values every tick; this writes them
// into the installed runtime form and re-pokes the graph while the ease is
// in flight, so the engine -- which only samples ZoomData when poked --
// tracks the moving values and the eye glides between sights. Runs from
// the snapshot consumer, after the resolver has published this tick.
void DriveSightZoomTransition(const ScopeData::ZoomDataOverwrite& blended)
{
	if (!installedSightZoom || !originalZoomInstance ||
		originalZoomInstance->zoomData != installedSightZoom ||
		!currentData) {
		return;
	}
	// Both blend endpoints declining the override means the snapshot holds the
	// struct's inert defaults, not values; leave the seeded authored data in
	// place. One enabled endpoint is enough (LerpZoom keeps the flag on across
	// the blend so the glide is continuous).
	if (!blended.enableZoomDateOverwrite) {
		return;
	}
	installedSightZoom->zoomData.fovMult = blended.fovMul;
	if (settings.AllowsCameraOverrides()) {
		installedSightZoom->zoomData.cameraOffset = {
			blended.x,
			blended.y,
			blended.z
		};
	}

	auto& state = MagnaScope::SessionStateFor(*currentData);
	const float target = state.secondaryIndex >= 0 ? 1.0F : 0.0F;
	// Approach() snaps exactly onto its target when it settles, so a plain
	// equality comparison is the settled test, no epsilon needed.
	const bool inFlight = state.sightBlend != target;
	static bool wasInFlight = false;
	const bool settledThisTick = !inFlight && wasInFlight;
	if (inFlight != wasInFlight) {
		wasInFlight = inFlight;
		logger::verbose(
			"[sight] transition {} (blend={:.2f}, target={:.0f})",
			inFlight ? "started" : "settled",
			state.sightBlend,
			target);
	}
	// Poke while the ease is moving, plus once more on the settle edge so
	// the engine is guaranteed to sample the exact final values. Sighted
	// only: the sample is only observable in ADS, and the ADS-entry rising
	// edge already covers the next aim-in.
	if ((inFlight || settledThisTick) && player && IsInADS(player)) {
		FireSightSwapGraphEvents(false);
	}
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

void ApplySelectedZoomOverride(const ScopeData::ScopeProfile* profile)
{
	if (!settings.AllowsOverrides() ||
		!profile || !HasSelectedZoomSession()) {
		return;
	}

	// Match original scope-rendering by changing the selected form's values before aim
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
	const ScopeData::ScopeProfile& left,
	const ScopeData::ScopeProfile& right)
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
	// Order matters: while a runtime sight form is installed the session check
	// below reports false (the instance points away from the authored form),
	// so the pointer restore has to happen first or a runtime FormID would
	// ride the instance straight into the save.
	RestoreSightZoomPointer();
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
	const auto selectProfile = [](ScopeData::ScopeProfile* profile, bool containsAllAdditionalKeywords = true) {
		// A profile selection can replace the first-person NIF and its pooled
		// D3D suballocations. Clear the old identity before exposing the new
		// profile to the render thread.
		InvalidateAutomaticSTSSelection();
		sdh->SetCurrentScopeProfile(profile, containsAllAdditionalKeywords);
		currentData = profile;
		MagnaScope::WorldOnlyScopeRenderer::GetSingleton().RequestFrame(
			settings.AllowsWorldColorCapture() && profile != nullptr &&
			profile->autoProfile && containsAllAdditionalKeywords);

		// Apply the profile before Fallout begins an aim transition without
		// replacing the BGSZoomData pointer cached by the engine.
		ApplySelectedZoomOverride(
			containsAllAdditionalKeywords ? profile : nullptr);
		bFirstTimeZoomData =
			settings.AllowsOverrides() &&
			profile != nullptr && containsAllAdditionalKeywords;
		// Default-on is applied when actually sighted (see HookedUpdate), not
		// here: profile selection runs at equip, before the profile has been
		// configured/saved, so seeding here would latch the wrong state.
	};

	const auto clearSelection = [] {
		MagnaScope::WorldOnlyScopeRenderer::GetSingleton().RequestFrame(false);
		InvalidateAutomaticSTSSelection();
		ClearIsolatedZoomSession();
		sdh->SetCurrentScopeProfile(nullptr);
		currentData = nullptr;
		weaponInstanceData = nullptr;
		bFirstTimeZoomData = false;
		// Re-arm default-on so the next scope (even the same one re-equipped)
		// applies its default again.
		lastVisionSeededProfile = nullptr;
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
		// Publish this scope's reticle list for the editor and the hotkey
		// cycler. Reads the cache built at data load, not the disk.
		ImGuiImpl::PublishDiscoveredReticles(ReticlesForSelectedScope());
		logger::info("Selected automatic STS profile {}", profile->keywordName);
		return;
	}

	clearSelection();
}

// --- optics hotkey state -------------------------------------------------
// The key is polled on the game thread (PollOpticsKey) while the wheel arrives
// on the input thread (MagnaScopeInputCallback): the poll writes these flags,
// the wheel reads/writes them, so they are atomic. The press timestamp is only
// touched inside the poll.
std::atomic<bool> opticsKeyHeld{ false };
std::atomic<bool> opticsKeyConsumedByScroll{ false };
std::chrono::steady_clock::time_point opticsKeyPressTime{};

// Live secondary-sight eye shift, ZoomData offset space, blend-scaled by the
// resolver. Game thread only: written by the preview consumer, applied to the
// first-person weapon each frame in the scope block. The engine samples the
// zoom form's cameraOffset at aim-in only, so this is what actually moves the
// eye mid-ADS.
float sightShiftOffset[3] = { 0.0F, 0.0F, 0.0F };
// Longer than a deliberate tap ever is, shorter than a hold ever is.
constexpr float kOpticsTapSeconds = 0.25F;

// Reticle files for the selected scope, discovered once at data load and on
// explicit rescan rather than per weapon swap: a recursive directory read under
// MO2's virtual file system is not a bounded operation, and a hitch on every
// equip is exactly what gets a mod blamed for stutter.
std::map<
	std::tuple<std::string, std::uint32_t, std::string>,
	std::vector<std::string>>
	discoveredReticlesByScope;

// Walks every profile's reticles folder once and caches the result.
//
// Deliberately not per-selection: profile selection runs inside HookedUpdate on
// the main thread, and a cold-cache recursive directory read there -- under
// MO2's USVFS, which adds real latency to directory walks -- is an unbounded
// stall on every weapon swap.
void RediscoverReticles()
{
	discoveredReticlesByScope.clear();

	const std::filesystem::path root(
		"Data\\F4SE\\Plugins\\MagnaScope\\Auto");
	std::error_code error;
	if (!std::filesystem::exists(root, error)) {
		return;
	}

	std::size_t total = 0U;
	for (const auto& profile : sdh->AllAutoProfiles()) {
		if (!profile) {
			continue;
		}
		const auto directory = profile->ReticleDirectory();
		if (directory.empty() ||
			!std::filesystem::exists(directory, error)) {
			continue;
		}

		std::vector<std::string> reticles;
		for (const auto& entry :
			std::filesystem::directory_iterator(
				directory,
				std::filesystem::directory_options::skip_permission_denied,
				error)) {
			if (!entry.is_regular_file(error)) {
				continue;
			}
			auto extension = entry.path().extension().string();
			std::ranges::transform(
				extension,
				extension.begin(),
				[](unsigned char value) {
					return static_cast<char>(std::tolower(value));
				});
			if (extension != ".dds" && extension != ".png") {
				continue;
			}
			reticles.push_back(entry.path().filename().string());
		}

		if (reticles.empty()) {
			continue;
		}
		std::ranges::sort(reticles);
		total += reticles.size();
		discoveredReticlesByScope.emplace(
			MagnaScope::MakeSessionKey(*profile),
			std::move(reticles));
	}

	logger::info(
		"Discovered {} reticle textures across {} scope profiles",
		total,
		discoveredReticlesByScope.size());
}

[[nodiscard]] const std::vector<std::string>& ReticlesForSelectedScope()
{
	static const std::vector<std::string> empty;
	if (!currentData) {
		return empty;
	}
	const auto entry =
		discoveredReticlesByScope.find(MagnaScope::MakeSessionKey(*currentData));
	return entry == discoveredReticlesByScope.end() ? empty : entry->second;
}

// Input arrives on Fallout's input thread, not the game thread. The session
// state it wants to change lives in a std::map that the game thread inserts
// into and holds references into, so touching it from here would be a genuine
// data race -- an insert can rehash while the resolver is mid-read.
//
// So input only ever accumulates a delta, and the game thread applies it. Same
// discipline as the API command queue, for the same reason.
std::atomic<int> pendingVariantScroll{ 0 };
std::atomic<int> pendingSightScroll{ 0 };
std::atomic<int> pendingReticleCycle{ 0 };

// True when the wheel belongs to variant selection, so the caller must not also
// apply free zoom. Reads the profile only for the enabled flag, which the game
// thread does not mutate during play.
// +1 for wheel up, -1 for wheel down, 0 for anything else.
//
// Different local input paths expose the wheel as a raw mouse ID (8/9), an
// already-unified ID (0x108/0x109), or a BS_BUTTON_CODE. MagnaScope only ever
// tested the raw pair, so on any setup reporting the unified form the wheel was
// never seen at all -- exactly the "scrolling does nothing" symptom. Taken from
// ScrollWheelWeaponSelect, which handles all three deliberately.
[[nodiscard]] int WheelDirectionOf(std::uint32_t idCode) noexcept
{
	switch (idCode) {
	case 8U:
	case 0x108U:
	case static_cast<std::uint32_t>(RE::BS_BUTTON_CODE::kWheelUp):
		return 1;
	case 9U:
	case 0x109U:
	case static_cast<std::uint32_t>(RE::BS_BUTTON_CODE::kWheelDown):
		return -1;
	default:
		return 0;
	}
}

// Optics hotkey, registered with F4SE Menu Framework.
//
// The framework owns binding, persistence (its own PluginHotkeys.ini, so our
// updates never clobber a rebind) and conflict warnings. Critically it works in
// DIK scan codes, which is also what Fallout's ButtonEvent reports for the
// keyboard -- MagnaScope's editor was binding from a Virtual-Key table, so the
// bound code and the reported code were in different code spaces and simply
// never compared equal. No amount of gating was ever going to fix that.
constexpr const char* kOpticsHotkeyId = "MagnaScope.Optics";
// DIK_X. Only a default; the framework's persisted binding wins.
constexpr unsigned int kOpticsHotkeyDefault = 0x2D;
// The framework encodes mouse buttons as scan codes starting at 256
// (MouseLeft=256, MouseRight=257, MouseMiddle=258, Mouse4=259, Mouse5=260),
// deliberately above the 8-bit DIK range so a keyboard key and a mouse button
// can never collide. Any bound code at or above this is a mouse button. This
// is the same unified 0x100+ space Fallout's ButtonEvent also reports mouse
// ids in, which is why the runtime comparison below needs no per-code table.
constexpr unsigned int kMouseBindingBase = 256U;

[[nodiscard]] bool RouteVariantScroll(int direction)
{
	if (!currentData) {
		return false;
	}
	if (!currentData->variants.enabled ||
		currentData->variants.variants.size() < 2U) {
		// Rate-limited: this runs per wheel notch, and a silent decline here is
		// indistinguishable in game from the wheel not being read at all.
		static std::uint64_t lastReport = 0U;
		const auto now = static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::seconds>(
				std::chrono::steady_clock::now().time_since_epoch())
				.count());
		if (now != lastReport) {
			lastReport = now;
			logger::verbose(
				"Wheel not routed to variants: enabled={}, variantCount={}. "
				"Free-scroll zoom is handling it instead.",
				currentData->variants.enabled,
				currentData->variants.variants.size());
		}
		return false;
	}
	pendingVariantScroll.fetch_add(direction, std::memory_order_relaxed);
	return true;
}

void RouteSecondarySightScroll(int direction)
{
	if (!currentData || currentData->secondarySights.empty()) {
		return;
	}
	pendingSightScroll.fetch_add(direction, std::memory_order_relaxed);
}

void RouteReticleCycle(int direction)
{
	if (!currentData) {
		return;
	}
	pendingReticleCycle.fetch_add(direction, std::memory_order_relaxed);
}

// NOTE deliberately absent: there is NO re-aim dip on a sight swap. An
// earlier build dropped and re-raised the sighted state to force the engine
// to re-sample ZoomData; in game that opened a not-sighted window that
// ScrollWheelWeaponSelect acted on (weapon switch mid-swap) and could wedge
// the engine's aim state machine (no ADS, no wheel, fire still working). The
// camera offset in ZoomData applies live from the form while sighted, so the
// resolver's per-tick lerp is the whole transition.

// Drains the accumulated input on the game thread, where the session map is
// safe to mutate.
void ApplyPendingSightInput()
{
	if (!currentData) {
		// Discard rather than carry across a weapon change: a notch aimed at
		// the previous scope must not land on the next one.
		pendingVariantScroll.store(0, std::memory_order_relaxed);
		pendingSightScroll.store(0, std::memory_order_relaxed);
		pendingReticleCycle.store(0, std::memory_order_relaxed);
		return;
	}

	const int variantDelta =
		pendingVariantScroll.exchange(0, std::memory_order_relaxed);
	const int sightDelta =
		pendingSightScroll.exchange(0, std::memory_order_relaxed);
	const int reticleDelta =
		pendingReticleCycle.exchange(0, std::memory_order_relaxed);
	if (variantDelta == 0 && sightDelta == 0 && reticleDelta == 0) {
		return;
	}

	auto& state = MagnaScope::SessionStateFor(*currentData);
	if (variantDelta != 0) {
		const float before = state.variantTarget;
		const bool applied =
			MagnaScope::AdjustVariantSelection(*currentData, state, variantDelta);
		// One line per applied notch: proves the game thread saw the scroll and
		// moved the target. If this appears but the magnification does not
		// change on screen, the stall is downstream (resolver/consumer/render).
		logger::info(
			"[variant] delta={} applied={} target {:.2f}->{:.2f} pos={:.2f}",
			variantDelta,
			applied,
			before,
			state.variantTarget,
			state.variantPosition);
	}
	if (sightDelta != 0 &&
		MagnaScope::CycleSecondarySight(*currentData, state, sightDelta)) {
		// The resolver lerps the form's zoom data toward the new sight from
		// here on; the player stays sighted throughout.
		logger::verbose(
			"[sight] switched to index {} (lerp, no state change)",
			state.secondaryIndex);
	}
	if (reticleDelta != 0) {
		MagnaScope::CycleReticle(
			*currentData,
			state,
			static_cast<int>(ReticlesForSelectedScope().size()),
			reticleDelta);
	}
}

// DIK scan code -> Windows virtual key, for polling the bound key with
// GetAsyncKeyState. The reverse of the editor capture's VK->DIK. Extended keys
// (0x80+) don't round-trip through MapVirtualKeyA, hence the explicit table.
[[nodiscard]] int VirtualKeyFromDik(unsigned int dik)
{
	switch (dik) {
	case 0xC8U: return VK_UP;
	case 0xD0U: return VK_DOWN;
	case 0xCBU: return VK_LEFT;
	case 0xCDU: return VK_RIGHT;
	case 0xC7U: return VK_HOME;
	case 0xCFU: return VK_END;
	case 0xC9U: return VK_PRIOR;
	case 0xD1U: return VK_NEXT;
	case 0xD2U: return VK_INSERT;
	case 0xD3U: return VK_DELETE;
	case 0x9DU: return VK_RCONTROL;
	case 0xB8U: return VK_RMENU;
	default:
		return static_cast<int>(MapVirtualKeyA(dik, MAPVK_VSC_TO_VK));
	}
}

// Optics-key held/tap detection, polled on the game thread.
//
// History worth keeping: MagnaScope's [input] logs proved that F4SE Menu
// Framework's AddInputEvent callback delivered mouse events but NOT keyboard
// events (wheel arrived every notch, key presses never did) -- the framework
// was hooking PlayerControls' PerformInputProcessing, a mouse-only slice of the
// queue. The framework was then fixed to hook PlayerCamera's full-queue
// receiver instead, so the callback now does deliver keyboard.
//
// The poll is kept anyway. GetAsyncKeyState reads the physical key regardless
// of framework version or how the game routes input, and it sidesteps the
// DIK-vs-VK ambiguity of the raw keyboard event entirely (the binding is DIK;
// convert once to VK here). The wheel stays on the callback, which has always
// worked for mouse.
void PollOpticsKey()
{
	static bool wasDown = false;

	const auto boundCode =
		F4SEMenuFramework::Hotkeys::GetBinding(kOpticsHotkeyId);
	const int vk = boundCode != 0U ? VirtualKeyFromDik(boundCode) : 0;
	if (vk == 0) {
		wasDown = false;
		opticsKeyHeld.store(false, std::memory_order_relaxed);
		return;
	}

	const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;

	// Menu open: swallow the key but keep tracking state, so closing a menu
	// with the key held cannot read as a fresh press.
	if (F4SEMenuFramework::IsAnyBlockingWindowOpened()) {
		wasDown = down;
		opticsKeyHeld.store(false, std::memory_order_relaxed);
		opticsKeyConsumedByScroll.store(false, std::memory_order_relaxed);
		return;
	}

	if (down && !wasDown) {
		opticsKeyHeld.store(true, std::memory_order_relaxed);
		opticsKeyConsumedByScroll.store(false, std::memory_order_relaxed);
		opticsKeyPressTime = std::chrono::steady_clock::now();
		logger::verbose("[input] optics key DOWN (poll, scan 0x{:02X})", boundCode);
	} else if (!down && wasDown) {
		// Reticle cycling fires on release: that distinguishes a tap from a
		// hold without delaying the tap, and a hold that consumed a scroll
		// notch is suppressed so switching sights cannot also swap reticles.
		const auto heldSeconds =
			std::chrono::duration<float>(
				std::chrono::steady_clock::now() - opticsKeyPressTime)
				.count();
		if (opticsKeyHeld.load(std::memory_order_relaxed) &&
			!opticsKeyConsumedByScroll.load(std::memory_order_relaxed) &&
			heldSeconds < kOpticsTapSeconds) {
			RouteReticleCycle(1);
		}
		opticsKeyHeld.store(false, std::memory_order_relaxed);
		opticsKeyConsumedByScroll.store(false, std::memory_order_relaxed);
		logger::verbose("[input] optics key UP (poll, held {:.2f}s)", heldSeconds);
	}
	wasDown = down;
}

// Optics key AND mouse wheel, both handled through F4SE Menu Framework's
// AddInputEvent callback (single-path, re-enabled per request). PollOpticsKey
// and the own-hook wheel routing are disabled so this is the sole input path.
//
// Runs on the input thread. It publishes nothing directly into the session map
// (game-thread-owned); it only accumulates atomic deltas the game thread
// drains. Returns false always -- MagnaScope observes input, never consumes it.
// The optics key's press/release semantics, shared by the bound keyboard key
// and a bound mouse button so the two behave identically: a fresh press arms
// the hold state (the wheel handler reads opticsKeyHeld to route a
// secondary-sight switch), and a release that was neither consumed by a scroll
// nor held past the tap threshold cycles the reticle. `source` only labels the
// diagnostic line.
void ApplyOpticsKeyTransition(const ButtonEvent* button, const char* source)
{
	if (button->QJustPressed()) {
		opticsKeyHeld.store(true, std::memory_order_relaxed);
		opticsKeyConsumedByScroll.store(false, std::memory_order_relaxed);
		opticsKeyPressTime = std::chrono::steady_clock::now();
		logger::verbose("[input] optics key DOWN ({})", source);
	} else if (button->value == 0.0F) {
		const auto heldSeconds =
			std::chrono::duration<float>(
				std::chrono::steady_clock::now() - opticsKeyPressTime)
				.count();
		if (opticsKeyHeld.load(std::memory_order_relaxed) &&
			!opticsKeyConsumedByScroll.load(std::memory_order_relaxed) &&
			heldSeconds < kOpticsTapSeconds) {
			RouteReticleCycle(1);
		}
		opticsKeyHeld.store(false, std::memory_order_relaxed);
		opticsKeyConsumedByScroll.store(false, std::memory_order_relaxed);
		logger::verbose(
			"[input] optics key UP ({}, held {:.2f}s)", source, heldSeconds);
	}
}

bool __stdcall MagnaScopeInputCallback(RE::InputEvent* rawEvent)
{
	// This CommonLibF4 revision has no AsButtonEvent; check eventType and cast,
	// matching the receiver hook below.
	if (!rawEvent || rawEvent->eventType != INPUT_EVENT_TYPE::kButton) {
		return false;
	}
	auto* button = static_cast<ButtonEvent*>(rawEvent);
	const auto id = static_cast<std::uint32_t>(button->idCode);

	if (button->device == INPUT_DEVICE::kMouse) {
		const int direction = WheelDirectionOf(id);
		// The framework already suppresses this callback while a blocking
		// window is open; the explicit guard also covers MagnaScope's own
		// editor, so scrolling to adjust a slider never switches variants.
		if (direction != 0 && button->QJustPressed() &&
			!F4SEMenuFramework::IsAnyBlockingWindowOpened()) {
			const bool held = opticsKeyHeld.load(std::memory_order_relaxed);
			// One line that localises a "hold+scroll does nothing" report:
			// held=0 means the keyboard optics key never registered as down
			// (input never reached this callback, or the wrong code); held=1
			// with sights=0 means no secondary sight is configured on the live
			// scope; held=1 with sights>0 means the wheel routed correctly and
			// the fault is downstream in the resolver. Rate-limited to one per
			// second so a scroll burst does not flood the log.
			static std::uint64_t lastWheelLog = 0U;
			const auto nowSec = static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::seconds>(
					std::chrono::steady_clock::now().time_since_epoch())
					.count());
			if (nowSec != lastWheelLog) {
				lastWheelLog = nowSec;
				logger::verbose(
					"[input] wheel dir={} opticsHeld={} sights={}",
					direction,
					held ? 1 : 0,
					currentData ? currentData->secondarySights.size() : 0U);
			}
			if (held) {
				opticsKeyConsumedByScroll.store(
					true, std::memory_order_relaxed);
				RouteSecondarySightScroll(direction);
			} else if (!RouteVariantScroll(direction)) {
				if (hookIns && hookIns->GetRenderState()) {
					hookIns->AdjustZoomDelta(0.1F * direction);
				}
			}
			return false;
		}

		// Not the wheel. A bound MOUSE BUTTON is the optics key, behaving
		// exactly like a bound keyboard key: tap to cycle reticles, hold and
		// scroll to switch sights. The framework stores mouse buttons at
		// codes 256+, and Fallout reports mouse button ids either raw
		// (0..7 -- left/right/middle/x1/x2) or already unified (0x100+), the
		// same dual form the wheel arrives in; normalise to the unified space
		// so a single comparison covers both input paths.
		const auto boundCode =
			F4SEMenuFramework::Hotkeys::GetBinding(kOpticsHotkeyId);
		if (boundCode >= kMouseBindingBase) {
			const auto unifiedId = id >= 0x100U ? id : id + 0x100U;
			if (unifiedId == boundCode) {
				ApplyOpticsKeyTransition(button, "mouse");
			}
		}
		return false;
	}

	if (button->device != INPUT_DEVICE::kKeyboard) {
		return false;
	}

	// A mouse-button binding is handled entirely in the mouse branch above; no
	// keyboard event can match it, so skip the VK conversion (which is
	// meaningless for a 256+ code) and leave the key path for keyboard binds.
	if (F4SEMenuFramework::Hotkeys::GetBinding(kOpticsHotkeyId) >=
		kMouseBindingBase) {
		return false;
	}

	// DIAGNOSTIC: log every fresh keyboard press's idCode reaching this
	// callback, to reveal whether the dispatched keyboard code is DIK (matches
	// the 0x15 binding) or VK (e.g. 0x59 for Y) or something else. The framework
	// confirms keyboard events DO reach here (btn kbd=1); this shows their code.
	if (button->QJustPressed()) {
		logger::verbose(
			"[input] kbd press reached callback: scan 0x{:02X} (optics bound 0x{:02X})",
			id,
			F4SEMenuFramework::Hotkeys::GetBinding(kOpticsHotkeyId));
	}

	// Optics key detected HERE on the AddInputEvent callback (single-path per
	// request; PollOpticsKey is disabled in the tick). The framework dispatches
	// the raw queue with a DIK keyboard idCode, the same space the Hotkeys
	// binding uses, so it compares directly. Under investigation with the
	// framework's [InputQueueHook] diagnostic (this callback was not dispatching
	// while MagnaScope's own PlayerCamera hook was also installed).
	// PROVEN BY LOG (2026-08-14 17:23): this receiver reports VK codes, not
	// DIK -- pressing the bound Y arrived as 0x59 (VK 'Y'), Escape as 0x1B,
	// Left Alt as 0xA4, while the framework binding is DIK 0x15. Matches
	// MeleeAndThrow's documentation of this receiver ("keyboard idCode is a
	// Windows virtual-key code; its default 0xA4 is VK_LMENU"); the framework
	// guide's "idCode is a DIK scan code" is wrong for PlayerCamera's queue.
	// So convert the DIK binding to VK once and compare in VK space.
	const auto boundCode =
		F4SEMenuFramework::Hotkeys::GetBinding(kOpticsHotkeyId);
	const auto boundVk = boundCode != 0U ?
	                         static_cast<std::uint32_t>(
								 VirtualKeyFromDik(boundCode)) :
	                         0U;
	if (boundVk == 0U || id != boundVk) {
		return false;
	}

	ApplyOpticsKeyTransition(button, "keyboard");
	return false;
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
			// Wheel routing lives on the framework's AddInputEvent callback
			// (MagnaScopeInputCallback), which delivers mouse reliably. This
			// receiver hook keeps only the legacy NVG key -- confirmed with the
			// framework author that the callback and this hook compose cleanly
			// on PlayerCamera+0x38, so there is no reason to route here too.
			id += 0x100;
		}
		if (evn->device == INPUT_DEVICE::kGamepad)
			id += 0x10000;

		//if (evn->device == INPUT_DEVICE::kKeyboard && id == VK_OEM_PERIOD && evn->QJustPressed()) {
		//	std::monostate mono;
		//	//TestButton(mono);
		//}

		if (evn->device == INPUT_DEVICE::kKeyboard) {
			// The optics key is NOT handled here -- it lives in
			// MagnaScopeInputCallback, where the framework dispatches the raw
			// queue with DIK keyboard codes that match the Hotkeys binding
			// directly. Only the legacy STS night-vision key remains on this
			// receiver.

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

				// Thermal Vision toggle, mirroring the night-vision handler
				// above: either a bare key or a modifier + key combo.
				if (sdh->comboThermalKey == -1) {
					if (id == (uint32_t)sdh->thermalKey && evn->QJustPressed()) {
						thermalFlag = !thermalFlag;
						hookIns->SetThermal((int)thermalFlag);
					}
				} else {
					if (id == (uint32_t)(sdh->comboThermalKey) && evn->heldDownSecs > 0 && evn->value == 1) {
						hasThermalCombo = true;
					}

					if (id == (uint32_t)(sdh->comboThermalKey) && evn->value == 0) {
						hasThermalCombo = false;
					}

					if (hasThermalCombo && id == (uint32_t)sdh->thermalKey && evn->QJustPressed()) {
						thermalFlag = !thermalFlag;
						hookIns->SetThermal((int)thermalFlag);
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
	if (!ScopeData::ScopeDataHandler::GetSingleton()->GetCurrentScopeProfile())
		return false;
	if (ScopeData::ScopeDataHandler::GetSingleton()->GetCurrentScopeProfile()->UsingSTS)
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

// --- sphere occlusion: game-thread producer --------------------------------
//
// Builds the holed index arrays for every occludable scope mesh and hands them
// to the render side. Everything runs here, on the game thread, from the
// engine's own CPU geometry copies (the same access the aperture vertex probe
// proved safe); the render side only creates buffers and swaps them at draw
// time.
//
// Recomputed only when its inputs change. The scope's meshes are rigid
// relative to the glass, so the in-sphere triangle set is a function of the
// settings and the equipped scope, not of the frame.
std::uint64_t occlusionInputHash = 0U;

[[nodiscard]] std::uint64_t HashOcclusionInputs(
	const ScopeData::OcclusionSettings& occlusionSettings,
	bool wanted,
	const void* scopeAiming,
	const void* glassNode)
{
	std::uint64_t hash = 1469598103934665603ULL;
	const auto mix = [&hash](const void* data, std::size_t size) {
		const auto* bytes = static_cast<const std::uint8_t*>(data);
		for (std::size_t index = 0; index < size; ++index) {
			hash ^= bytes[index];
			hash *= 1099511628211ULL;
		}
	};
	mix(&wanted, sizeof(wanted));
	mix(&scopeAiming, sizeof(scopeAiming));
	mix(&glassNode, sizeof(glassNode));
	mix(&occlusionSettings.enabled, sizeof(occlusionSettings.enabled));
	mix(&occlusionSettings.sphereRadius, sizeof(occlusionSettings.sphereRadius));
	mix(occlusionSettings.sphereOffset, sizeof(occlusionSettings.sphereOffset));
	mix(&occlusionSettings.frontOnly, sizeof(occlusionSettings.frontOnly));
	mix(&occlusionSettings.flipFront, sizeof(occlusionSettings.flipFront));
	for (const auto& name : occlusionSettings.excludedShapes) {
		mix(name.data(), name.size());
		mix("|", 1);
	}
	return hash;
}

// True for shapes the sphere must never cut regardless of settings: the
// optical surfaces themselves. These never even appear in the editor's
// exclude checklist -- there is nothing to decide about them.
[[nodiscard]] bool IsOcclusionAlwaysProtected(const std::string& name)
{
	const auto containsToken = [&name](const char* token) {
		const auto it = std::search(
			name.begin(), name.end(),
			token, token + std::strlen(token),
			[](char a, char b) {
				return std::tolower(static_cast<unsigned char>(a)) ==
			           std::tolower(static_cast<unsigned char>(b));
			});
		return it != name.end();
	};
	if (containsToken("ScopeFade") || containsToken("TextureLoader") ||
		containsToken("Reticle") || containsToken("Dot:") ||
		containsToken("Glass") || containsToken("Paralax") ||
		containsToken("Parallax")) {
		return true;
	}
	if (currentData) {
		if (name == currentData->shaderData.apertureSurface ||
			name == currentData->shaderData.reticleSurface) {
			return true;
		}
	}
	return false;
}

// User opt-outs. Separate from the always-protected set because these names
// must still be listed in the editor (an excluded shape has to stay visible
// to be un-excludable).
[[nodiscard]] bool IsOcclusionUserExcluded(
	const std::string& name,
	const ScopeData::OcclusionSettings& occlusionSettings)
{
	for (const auto& excluded : occlusionSettings.excludedShapes) {
		if (_stricmp(name.c_str(), excluded.c_str()) == 0) {
			return true;
		}
	}
	return false;
}

// IEEE binary16 encode, round toward zero. The occlusion-sphere gizmo
// vertices span tens of units, where a half ULP is far below a pixel;
// denormals flush to signed zero.
[[nodiscard]] std::uint16_t FloatToHalfBits(float value)
{
	std::uint32_t bits = 0U;
	std::memcpy(&bits, &value, sizeof(bits));
	const std::uint32_t sign = (bits >> 16U) & 0x8000U;
	const std::uint32_t mantissa = bits & 0x007FFFFFU;
	const std::int32_t exponent =
		static_cast<std::int32_t>((bits >> 23U) & 0xFFU) - 127 + 15;
	if (exponent >= 31) {
		return static_cast<std::uint16_t>(sign | 0x7C00U);
	}
	if (exponent <= 0) {
		return static_cast<std::uint16_t>(sign);
	}
	return static_cast<std::uint16_t>(
		sign | (static_cast<std::uint32_t>(exponent) << 10U) |
		(mantissa >> 13U));
}

// Unit-sphere direction table for the gizmo, in exactly the topology the
// render side's index buffer expects (16 stacks x 24 slices, +Z pole
// first). The two sides must never drift apart.
[[nodiscard]] const std::vector<RE::NiPoint3>& OcclusionSphereUnitTable()
{
	static std::vector<RE::NiPoint3> table = [] {
		constexpr std::uint32_t stacks = 16U;
		constexpr std::uint32_t slices = 24U;
		std::vector<RE::NiPoint3> directions;
		directions.reserve((stacks + 1U) * (slices + 1U));
		for (std::uint32_t stack = 0U; stack <= stacks; ++stack) {
			const float phi = 3.14159265F *
				static_cast<float>(stack) / stacks;
			for (std::uint32_t slice = 0U; slice <= slices; ++slice) {
				const float theta = 6.2831853F *
					static_cast<float>(slice) / slices;
				directions.push_back(RE::NiPoint3{
					std::sin(phi) * std::cos(theta),
					std::sin(phi) * std::sin(theta),
					std::cos(phi) });
			}
		}
		return directions;
	}();
	return table;
}

// Closest point on triangle ABC to P (Ericson, Real-Time Collision
// Detection 5.1.5). Drives the sphere cull: a low-poly housing has
// triangles far larger than the sphere, and the original all-three-
// vertices-inside rule let such a triangle pass straight through the
// volume untouched -- in game that read as the sphere hiding almost
// nothing no matter where it was placed.
[[nodiscard]] RE::NiPoint3 ClosestPointOnTriangle(
	const RE::NiPoint3& p,
	const RE::NiPoint3& a,
	const RE::NiPoint3& b,
	const RE::NiPoint3& c)
{
	const RE::NiPoint3 ab = b - a;
	const RE::NiPoint3 ac = c - a;
	const RE::NiPoint3 ap = p - a;
	const float d1 = ab.Dot(ap);
	const float d2 = ac.Dot(ap);
	if (d1 <= 0.0F && d2 <= 0.0F) {
		return a;
	}
	const RE::NiPoint3 bp = p - b;
	const float d3 = ab.Dot(bp);
	const float d4 = ac.Dot(bp);
	if (d3 >= 0.0F && d4 <= d3) {
		return b;
	}
	const float vc = d1 * d4 - d3 * d2;
	if (vc <= 0.0F && d1 >= 0.0F && d3 <= 0.0F) {
		return a + ab * (d1 / (d1 - d3));
	}
	const RE::NiPoint3 cp = p - c;
	const float d5 = ab.Dot(cp);
	const float d6 = ac.Dot(cp);
	if (d6 >= 0.0F && d5 <= d6) {
		return c;
	}
	const float vb = d5 * d2 - d1 * d6;
	if (vb <= 0.0F && d2 >= 0.0F && d6 <= 0.0F) {
		return a + ac * (d2 / (d2 - d6));
	}
	const float va = d3 * d6 - d5 * d4;
	if (va <= 0.0F && (d4 - d3) >= 0.0F && (d5 - d6) >= 0.0F) {
		return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
	}
	const float denom = va + vb + vc;
	if (std::abs(denom) < 1e-12F) {
		return a;
	}
	const float v = vb / denom;
	const float w = vc / denom;
	return a + ab * v + ac * w;
}

void UpdateScopeOcclusion(RE::NiAVObject* firstPersonRoot, RE::NiAVObject* glassNode)
{
	// Editor preview wins while the editor is open, so the sphere can be
	// tuned live against unsaved values; otherwise the saved/session profile.
	ScopeData::OcclusionSettings preview;
	const bool editing =
		hookIns && hookIns->bEnableEditMode.load(std::memory_order_acquire);
	const ScopeData::OcclusionSettings* occlusionSettings = nullptr;
	if (editing && ImGuiImpl::GetOcclusionPreview(preview)) {
		occlusionSettings = &preview;
	} else if (currentData) {
		occlusionSettings = &currentData->occlusion;
	}

	auto* scopeAiming = firstPersonRoot ?
		FindObjectByPrefixNoCase(firstPersonRoot, "ScopeAiming") :
		nullptr;
	// The sphere is tuned against the primary optic's eye line; from a
	// canted or top-mounted secondary sight the same volume cuts visible
	// housing. Suspension covers the blend too, so holes never pop while
	// the eye is still travelling. This flag feeds the input hash below,
	// which is what makes selecting a sight rebuild (to empty) and
	// selecting the optic rebuild the holes again.
	bool secondarySightActive = false;
	if (currentData) {
		const auto& sightState = MagnaScope::SessionStateFor(*currentData);
		secondarySightActive = sightState.secondaryIndex >= 0 ||
			sightState.sightBlend > 0.0001F;
	}
	const bool wanted =
		occlusionSettings && occlusionSettings->enabled &&
		!(occlusionSettings->disableOnSecondarySight &&
			secondarySightActive) &&
		bEnableScope &&
		glassNode && scopeAiming && currentData && currentData->autoProfile;

	static const ScopeData::OcclusionSettings disabledSettings{};
	const auto& hashed = occlusionSettings ? *occlusionSettings : disabledSettings;
	// The node pointers are hashed even when occlusion is off, so an equip
	// change still re-runs this and republishes the editor's shape checklist.
	const auto hash = HashOcclusionInputs(
		hashed,
		wanted,
		static_cast<const void*>(scopeAiming),
		static_cast<const void*>(glassNode));
	if (hash == occlusionInputHash) {
		return;
	}
	occlusionInputHash = hash;

	if (!wanted) {
		// Names still flow to the editor when a scope is selected but the
		// cull is off; that is how the checklist has content before the user
		// first enables the feature.
		if (scopeAiming) {
			std::vector<std::string> names;
			const std::function<void(RE::NiAVObject*)> collect =
				[&](RE::NiAVObject* object) {
					if (!object) {
						return;
					}
					if (object->IsTriShape()) {
						const std::string name(
							object->name.c_str() ? object->name.c_str() : "");
						if (!IsOcclusionAlwaysProtected(name)) {
							names.push_back(name);
						}
						return;
					}
					if (auto* node = object->IsNode()) {
						for (const auto& child : node->children) {
							collect(child.get());
						}
					}
				};
			collect(scopeAiming);
			ImGuiImpl::PublishOcclusionShapes(names);
		}
		Hook::D3D::PublishScopeOcclusion({});
		return;
	}

	// Sphere and front plane in world space, from the glass node. The offset
	// is authored in the glass's local axes; Transpose(rotate) maps a local
	// direction into world, matching the camera-space idiom used elsewhere.
	const auto& glassWorld = glassNode->world;
	const RE::NiPoint3 offsetLocal{
		occlusionSettings->sphereOffset[0],
		occlusionSettings->sphereOffset[1],
		occlusionSettings->sphereOffset[2]
	};
	const RE::NiPoint3 sphereCenterWorld =
		glassWorld.translate + glassWorld.rotate.Transpose() * offsetLocal;
	RE::NiPoint3 frontNormalWorld =
		glassWorld.rotate.Transpose() *
		RE::NiPoint3{ 0.0F, occlusionSettings->flipFront ? -1.0F : 1.0F, 0.0F };
	const RE::NiPoint3 planePointWorld = glassWorld.translate;

	std::vector<Hook::D3D::OcclusionEntry> entries;
	std::size_t consideredShapes = 0U;
	// Every candidate name goes to the editor's exclude checklist, whether or
	// not it ends up culled this rebuild.
	std::vector<std::string> shapeNames;

	const std::function<void(RE::NiAVObject*)> walk =
		[&](RE::NiAVObject* object) {
			if (!object) {
				return;
			}
			if (auto* shape = object->IsTriShape()) {
				++consideredShapes;
				const std::string name(
					object->name.c_str() ? object->name.c_str() : "");
				if (IsOcclusionAlwaysProtected(name)) {
					return;
				}
				shapeNames.push_back(name);
				if (IsOcclusionUserExcluded(name, *occlusionSettings)) {
					return;
				}
				if (shape->numVertices == 0U ||
					shape->numVertices > 65535U ||
					shape->numTriangles == 0U) {
					return;
				}
				auto* rendererShape = shape->rendererData ?
					static_cast<RE::BSGraphics::TriShape*>(
						shape->rendererData) :
					nullptr;
				auto* vertexBuffer =
					rendererShape ? rendererShape->vertexBuffer : nullptr;
				auto* indexBuffer =
					rendererShape ? rendererShape->indexBuffer : nullptr;
				if (!vertexBuffer || !indexBuffer ||
					!vertexBuffer->data || vertexBuffer->invalidCpuData ||
					!indexBuffer->data || indexBuffer->invalidCpuData ||
					!indexBuffer->buffer) {
					return;
				}
				const std::uint32_t stride = shape->vertexDesc.GetSize();
				const bool fullPrecision = shape->vertexDesc.HasFlag(
					RE::BSGraphics::Vertex::Flags::VF_FULLPREC);
				const std::uint32_t indexCount = shape->numTriangles * 3U;
				const std::uint64_t vertexSpan =
					static_cast<std::uint64_t>(shape->numVertices) * stride;
				const std::uint64_t indexSpan =
					static_cast<std::uint64_t>(indexCount) *
					sizeof(std::uint16_t);
				if (stride < 8U ||
					vertexSpan > std::max(
						vertexBuffer->maxDataSize, vertexBuffer->dataSize) ||
					indexSpan > std::max(
						indexBuffer->maxDataSize, indexBuffer->dataSize)) {
					return;
				}

				// Sphere and plane in this mesh's local space; the local frame
				// is where the CPU vertex copy lives.
				const auto& meshWorld = object->world;
				const float scale =
					meshWorld.scale > 1e-6F ? meshWorld.scale : 1.0F;
				const RE::NiPoint3 centerLocal =
					(meshWorld.rotate *
						(sphereCenterWorld - meshWorld.translate)) *
					(1.0F / scale);
				const float radiusLocal = occlusionSettings->sphereRadius / scale;
				const RE::NiPoint3 planePointLocal =
					(meshWorld.rotate *
						(planePointWorld - meshWorld.translate)) *
					(1.0F / scale);
				const RE::NiPoint3 normalLocal =
					meshWorld.rotate * frontNormalWorld;

				const auto* base =
					static_cast<const std::uint8_t*>(vertexBuffer->data);
				const auto readPosition = [&](std::uint32_t index) {
					const auto* vertex = base +
						static_cast<std::size_t>(index) * stride;
					if (fullPrecision) {
						float components[3]{};
						std::memcpy(components, vertex, sizeof(components));
						return RE::NiPoint3{
							components[0], components[1], components[2]
						};
					}
					std::uint16_t components[3]{};
					std::memcpy(components, vertex, sizeof(components));
					return RE::NiPoint3{
						DecodeHalfFloat(components[0]),
						DecodeHalfFloat(components[1]),
						DecodeHalfFloat(components[2])
					};
				};
				// The sphere culls any triangle it TOUCHES (closest-point
				// distance), not only triangles wholly inside: housing
				// triangles are routinely larger than the sphere, and the
				// wholly-inside rule let them pass straight through the volume
				// uncut. The front-plane gate stays per-vertex and strict --
				// every vertex must sit on the objective side -- so a triangle
				// spanning the glass plane can never cut into the eyepiece
				// side.
				const auto frontSide = [&](const RE::NiPoint3& position) {
					if (!occlusionSettings->frontOnly) {
						return true;
					}
					const RE::NiPoint3 fromPlane = position - planePointLocal;
					return fromPlane.x * normalLocal.x +
							fromPlane.y * normalLocal.y +
							fromPlane.z * normalLocal.z >
						0.0F;
				};
				const auto triangleCulled = [&](
											 const RE::NiPoint3& p0,
											 const RE::NiPoint3& p1,
											 const RE::NiPoint3& p2) {
					if (!frontSide(p0) || !frontSide(p1) || !frontSide(p2)) {
						return false;
					}
					const RE::NiPoint3 closest =
						ClosestPointOnTriangle(centerLocal, p0, p1, p2);
					const RE::NiPoint3 toCenter = closest - centerLocal;
					return toCenter.x * toCenter.x + toCenter.y * toCenter.y +
							toCenter.z * toCenter.z <=
						radiusLocal * radiusLocal;
				};

				const auto* sourceIndices =
					static_cast<const std::uint16_t*>(indexBuffer->data);
				std::vector<std::uint16_t> indices(
					sourceIndices, sourceIndices + indexCount);
				std::uint32_t culled = 0U;
				for (std::uint32_t triangle = 0U;
					 triangle < shape->numTriangles;
					 ++triangle) {
					const std::uint32_t at = triangle * 3U;
					const std::uint16_t i0 = indices[at];
					const std::uint16_t i1 = indices[at + 1U];
					const std::uint16_t i2 = indices[at + 2U];
					if (i0 >= shape->numVertices ||
						i1 >= shape->numVertices ||
						i2 >= shape->numVertices) {
						continue;
					}
					if (triangleCulled(
							readPosition(i0),
							readPosition(i1),
							readPosition(i2))) {
						indices[at + 1U] = i0;
						indices[at + 2U] = i0;
						++culled;
					}
				}

				if (culled > 0U) {
					entries.push_back(Hook::D3D::OcclusionEntry{
						reinterpret_cast<std::uintptr_t>(indexBuffer->buffer),
						indexBuffer->dataOffset,
						indexCount,
						std::move(indices),
						culled });
				}
				return;
			}
			if (auto* node = object->IsNode()) {
				for (const auto& child : node->children) {
					walk(child.get());
				}
			}
		};
	walk(scopeAiming);

	ImGuiImpl::PublishOcclusionShapes(shapeNames);
	logger::verbose(
		"[occlusion] rebuilt: {} shape(s) considered, {} substituted, "
		"radius={:.2f} frontOnly={} excludes={}",
		consideredShapes,
		entries.size(),
		occlusionSettings->sphereRadius,
		occlusionSettings->frontOnly,
		occlusionSettings->excludedShapes.size());
	Hook::D3D::PublishScopeOcclusion(std::move(entries));
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

// Game thread: build the thermal overlay's heat list from nearby living actors
// and publish it to the render thread through the lock-free seqlock. Runs only
// while the thermal mode is toggled on. Actors are projected with the WORLD
// camera at worldFOV, so their normalized screen positions match where they
// appear in the backbuffer the scope samples; the magnify shader evaluates the
// blobs in that same source space, so magnification needs no compensation here.
// Only normalized scalars are published -- no scene-graph pointer crosses to
// the render thread.
// One light/fire candidate read from the cell under its lock, before the
// (reentrant) projection + raycast run unlocked. Kept small and POD so the
// locked section is nothing but cheap field reads.
struct PendingLightSource
{
	RE::NiPoint3 position{};
	float thermalStrength = 0.0F;
	float lightStrength = 0.0F;
	float warmth = 0.0F;
};

// Game-thread producer for both overlays. Actors feed the thermal channel;
// light/fire emitters feed the light channel (NV bloom) plus thermal when warm.
// Every candidate is projected, off-screen-culled, then occlusion-tested with a
// camera->source ray, so a source behind cover never glows. Runs on
// PCUpdateMainThread (main thread), where cell iteration and Havok picks are
// safe. Published lock-free via the heat seqlock.
// a_cameraNode / a_fov MUST be the same first-person "Camera" node and FOV the
// aperture projection uses (firstPersonRoot->GetObjectByName("Camera"),
// pcam->firstPersonFOV) -- projecting world objects through any other node
// (e.g. PlayerCamera::cameraRoot) puts them behind the camera, collapsing the
// projected radius so ProjectWorldSphereToScreen reports invalid and every
// source is dropped. That was why no body ever lit up.
static void PublishVisionSources(
	RE::PlayerCharacter* a_player,
	bool a_wantThermal,
	bool a_wantNV)
{
	auto* hook = D3D::GetSington();
	if (!hook) {
		return;
	}

	std::array<D3D::HeatSource, D3D::kMaxHeatSources> sources{};
	std::uint32_t count = 0U;

	// Verbose diagnostics: how many candidates were considered, dropped
	// off-screen, culled by occlusion, and finally published. Lets a bug report
	// (with Verbose Logging on) distinguish "no sources found" from "occlusion
	// ate them" without a debugger.
	int diagConsidered = 0;
	int diagOffscreen = 0;
	int diagOccluded = 0;

	// Project through the game's WORLD camera (worldToCam), which is what
	// actually renders the backbuffer the magnify shader samples. MagnaScope's
	// own WorldPointToScreen is FIRST-PERSON-scene-graph space and silently
	// rejects every main-world point -- that is why no actor, light, or the sun
	// ever lit up. Actors, placed lights, and the sun are all main-world space.
	auto* worldCam = RE::Main::WorldRootCamera();
	if (worldCam) {
		const auto& m = worldCam->worldToCam;
		const RE::NiPoint3 camPos = worldCam->world.translate;

		// World -> backbuffer UV [0,1] (y-down). Returns false behind camera.
		// Same worldToCam row-vector projection + perspective divide the debug
		// overlays use.
		const auto worldToUv =
			[&](const RE::NiPoint3& w, float& u, float& v) -> bool {
			const float trace =
				w.x * m[3][0] + w.y * m[3][1] + w.z * m[3][2] + m[3][3];
			if (trace <= 0.00001F) {
				return false;  // behind the camera
			}
			const float inv = 1.0F / trace;
			const float x =
				(w.x * m[0][0] + w.y * m[0][1] + w.z * m[0][2] + m[0][3]) * inv;
			const float y =
				(w.x * m[1][0] + w.y * m[1][1] + w.z * m[1][2] + m[1][3]) * inv;
			u = (x + 1.0F) * 0.5F;
			v = 1.0F - (y + 1.0F) * 0.5F;  // NDC y-up -> texture v (y-down)
			return std::isfinite(u) && std::isfinite(v);
		};

		// Project a world source, cull off-screen, occlusion-test, and append a
		// blob. Only on-screen candidates are raycast, so the per-frame cast
		// count is bounded by what is actually visible through the optic.
		const auto tryAdd = [&](const RE::NiPoint3& worldCenter,
								float worldRadius, float thermalStrength,
								float lightStrength, float warmth) {
			++diagConsidered;
			if (count >= D3D::kMaxHeatSources || !(worldRadius > 0.0F)) {
				return;
			}
			float u = 0.0F;
			float v = 0.0F;
			if (!worldToUv(worldCenter, u, v)) {
				return;  // behind camera
			}
			if (u < -0.25F || u > 1.25F || v < -0.25F || v > 1.25F) {
				++diagOffscreen;
				return;
			}
			// Blob radius: project a world-up offset and take the screen delta
			// (height-normalized, matching the shader's g.z * BUFFER_HEIGHT).
			float ru = 0.0F;
			float rv = 0.0F;
			float radius = 0.01F;
			if (worldToUv({ worldCenter.x, worldCenter.y,
							worldCenter.z + worldRadius },
					ru, rv)) {
				radius = std::max(radius, std::abs(rv - v));
			}
			// Cap well under a scope-filling disk: a bounding-sphere radius over-
			// covers the visible object, and the scope then magnifies it further.
			radius = std::clamp(radius, 0.004F, 0.09F);
			// Occlusion: a solid hit closer than the source (minus its own
			// radius, so a hit on the source's near surface does not self-cull)
			// means it sits behind cover and must not glow.
			const RE::NiPoint3 delta{ worldCenter.x - camPos.x,
									  worldCenter.y - camPos.y,
									  worldCenter.z - camPos.z };
			const float dist = std::sqrt(
				delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
			MagnaScope::Raycast::RayHit rayHit;
			if (MagnaScope::Raycast::Cast(camPos, worldCenter, rayHit) &&
				rayHit.distance < dist - worldRadius) {
				++diagOccluded;
				return;
			}
			sources[count].x = u;
			sources[count].y = v;
			sources[count].radius = radius;
			sources[count].thermalStrength = thermalStrength;
			sources[count].lightStrength = lightStrength;
			sources[count].warmth = warmth;
			++count;
		};

		// 1) Living actors -> body heat (thermal only; a person emits no light,
		// so it never blooms night vision). Skipped when thermal is off.
		if (a_wantThermal) {
			auto* processLists = RE::ProcessLists::GetSingleton();
			if (processLists) {
				for (const RE::ActorHandle& handle :
					processLists->highActorHandles) {
					if (count >= D3D::kMaxHeatSources) {
						break;
					}
					const RE::NiPointer<RE::Actor> actorPtr = handle.get();
					RE::Actor* actor = actorPtr.get();
					if (!actor || actor == a_player || actor->IsPlayerRef()) {
						continue;
					}
					if (actor->IsDead(false)) {  // corpses cool off
						continue;
					}
					RE::NiAVObject* actor3D = actor->Get3D();
					if (!actor3D) {
						continue;
					}
					// 0.5x the bounding sphere: the visible body is smaller than the
				// sphere that encloses it, so this keeps the hot spot on the
				// animal rather than a halo around it.
				tryAdd(actor3D->worldBound.center,
						actor3D->worldBound.fRadius * 0.5F, 1.5F, 0.0F, 1.0F);
				}
			}
		}

		// 2) Light / fire emitters -> NV bloom, plus thermal heat when warm.
		// Enumerated whenever either mode is on. The cell references are read
		// under the cell spin lock into locals ONLY; the projection + raycast
		// then run UNLOCKED, because cell->Pick can take that same lock and a
		// BSSpinLock is not recursive (deadlock otherwise -- the same
		// read-under-lock / process-outside discipline the OAR fix uses).
		if (a_wantThermal || a_wantNV) {
			std::array<PendingLightSource, 64> pending{};
			std::size_t pendingCount = 0U;
			if (RE::TESObjectCELL* cell =
					a_player ? a_player->parentCell : nullptr) {
				RE::BSAutoLock lock{ cell->spinLock };
				for (const RE::NiPointer<RE::TESObjectREFR>& refPtr :
					cell->references) {
					if (pendingCount >= pending.size()) {
						break;
					}
					RE::TESObjectREFR* ref = refPtr.get();
					if (!ref) {
						continue;
					}
					RE::TESBoundObject* base = ref->GetObjectReference();
					if (!base ||
						base->formType != RE::ENUM_FORM_ID::kLIGH) {
						continue;
					}
					auto* ligh = static_cast<RE::TESObjectLIGH*>(base);
					const float fade =
						std::isfinite(ligh->fade) ? ligh->fade : 1.0F;
					const float lightStrength =
						std::clamp(fade, 0.2F, 2.0F) * 1.2F;
					// Light temperature is DERIVED FROM INTENSITY: a brighter
					// light reads hotter on thermal. Flickering sources
					// (torches/fire) are pinned hot regardless, since flame is
					// hot even when dim. Flag bits (0x8 Flicker, 0x40
					// FlickerSlow, 0x80 Pulse, 0x100 PulseSlow) are the CK/xEdit
					// values (inferred, not in commonlib); the flicker
					// amplitudes are the reliable cross-check.
					const bool flickers =
						(ligh->data.flags & 0x1C8U) != 0U ||
						ligh->data.flickerIntensityAmplitude != 0.0F ||
						ligh->data.flickerMovementAmplitude != 0.0F;
					float warmth = std::clamp(lightStrength * 0.5F, 0.0F, 1.0F);
					if (flickers) {
						warmth = std::max(warmth, 0.85F);
					}
					const float thermalStrength =
						a_wantThermal ? warmth * lightStrength : 0.0F;
					const float lightOut = a_wantNV ? lightStrength : 0.0F;
					if (thermalStrength <= 0.0F && lightOut <= 0.0F) {
						continue;
					}
					pending[pendingCount].position = ref->GetPosition();
					pending[pendingCount].thermalStrength = thermalStrength;
					pending[pendingCount].lightStrength = lightOut;
					pending[pendingCount].warmth = warmth;
					++pendingCount;
				}
			}
			// Unlocked: project + occlusion-test each candidate. A fixed ~16u
			// glow sphere keeps a lamp a point glow rather than projecting its
			// whole illumination volume.
			for (std::size_t i = 0U; i < pendingCount; ++i) {
				if (count >= D3D::kMaxHeatSources) {
					break;
				}
				tryAdd(pending[i].position, 16.0F, pending[i].thermalStrength,
					pending[i].lightStrength, pending[i].warmth);
			}

			// Directional light (sun / moon): the brightest emitter outdoors.
			// The sun disk is placed far out in the sky, so its billboard node
			// gives a real world position that projects to where the sun
			// actually is. Projection rejects it when it is below the horizon
			// (behind camera) and the occlusion ray hides it behind terrain or
			// buildings, so it only shows in clear line of sight -- exactly like
			// looking up at it through the optic. Radius scales with distance to
			// a small angular disk; blinding for NV, hot for thermal.
			if (auto* sky = RE::Sky::GetSingleton()) {
				if (auto* sun = sky->sun) {
					RE::NiAVObject* sunNode =
						sun->sunGlareNode ? sun->sunGlareNode.get() :
						sun->sunBaseNode  ? sun->sunBaseNode.get() :
											nullptr;
					if (sunNode) {
						const RE::NiPoint3 sunPos = sunNode->world.translate;
						const RE::NiPoint3 d{ sunPos.x - camPos.x,
											  sunPos.y - camPos.y,
											  sunPos.z - camPos.z };
						const float sunDist = std::sqrt(
							d.x * d.x + d.y * d.y + d.z * d.z);
						if (sunDist > 1.0F) {
							const float sunRadius =
								std::max(sunDist * 0.03F, 8.0F);
							tryAdd(sunPos, sunRadius,
								a_wantThermal ? 2.0F : 0.0F,
								a_wantNV ? 3.0F : 0.0F, 1.0F);
						}
					}
				}
			}
		}
	}

	// Publishing (even an empty list) is valid and clears stale blobs.
	hook->PublishHeatSources(sources.data(), count);

	// Rate-limited so even verbose logs stay readable (~1/sec at 60fps).
	static int s_visionDiagTick = 0;
	if ((s_visionDiagTick++ % 60) == 0) {
		logger::verbose(
			"[vision] thermal={} nv={} published={} (considered={}, "
			"offscreen={}, occluded={})",
			a_wantThermal,
			a_wantNV,
			count,
			diagConsidered,
			diagOffscreen,
			diagOccluded);
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
		// original scope-rendering PlayerAim: block game keyboard/mouse processing (so
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
		bool pendingSaveWritesToDisk = true;
		if (auto pendingSave =
				ImGuiImpl::ConsumeProfileSave(pendingSaveWritesToDisk)) {
			if (currentData &&
				IsSameProfileIdentity(*currentData, *pendingSave)) {
				*currentData = *pendingSave;
				sdh->SetCurrentScopeProfile(currentData);
				// The session-only apply takes the identical path minus this
				// write. Editor values used to live only in the preview
				// snapshot, which the shader reads solely while edit mode is
				// on, so closing the editor silently reverted everything to
				// whatever was last written to disk. Putting them into the
				// in-memory profile instead makes them behave like the profile
				// they will become: they survive closing the editor,
				// re-equipping, and ADS cycles, and are lost only on exit.
				if (pendingSaveWritesToDisk) {
					sdh->WriteCurrentScopeProfile();
				}
				InitCurrentScopeData();
				// Push the snapshot's zoom onto the live weapon here rather than
				// relying on the edit-mode transition below to do it. That
				// transition is a one-shot driven by a flag the render thread
				// sets independently of this snapshot, so which of the two the
				// game thread observes first is a race. Applying it on arrival
				// makes the result the same either way, and it is idempotent:
				// the call re-baselines and reapplies from the profile, which is
				// exactly what the transition would have done.
				ApplySelectedZoomOverride(currentData);
				hookIns->bRefreshChar.store(
					true,
					std::memory_order_release);
				logger::info(
					"{} profile for attachment identity {}",
					pendingSaveWritesToDisk ?
						"Saved and reapplied" :
						"Applied for this session (not written to disk)",
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
		// Queued API work, drained on the game thread exactly like the editor
		// requests beside it. Callers may be on any thread; nothing they queue
		// touches an engine object until here.
		MagnaScopeAPI::DrainCommands();
		// Rebuild what the API getters serve, on this thread, so an external
		// caller never walks live engine state from its own.
		MagnaScopeAPI::RefreshSnapshots(
			static_cast<int>(ReticlesForSelectedScope().size()));

		const auto profileRequest = ImGuiImpl::ConsumeProfileAction();
		if (profileRequest != ImGuiImpl::ProfileRequest::kNone) {
			if (profileRequest == ImGuiImpl::ProfileRequest::kReload &&
				currentData &&
				(!currentData->autoProfile ||
					std::filesystem::exists(currentData->path))) {
				sdh->ReloadScopeProfile(currentData);
			}
			if (profileRequest ==
				ImGuiImpl::ProfileRequest::kRescanReticles) {
				RediscoverReticles();
				ImGuiImpl::PublishDiscoveredReticles(
					ReticlesForSelectedScope());
			}
			if (profileRequest ==
					ImGuiImpl::ProfileRequest::kDeletePreset &&
				currentData && currentData->autoProfile) {
				// Delete the file first. Dropping the cached profile without
				// removing it would resynthesize defaults now and then load the
				// stale preset back on the next launch, which reads as the
				// delete having silently failed.
				const std::string deletedPath = currentData->path;
				std::error_code removeError;
				const bool removed =
					std::filesystem::remove(deletedPath, removeError);
				if (removeError) {
					logger::error(
						"Could not delete preset '{}': {}",
						deletedPath,
						removeError.message());
				} else {
					// Evicting the cache entry is what makes the reset visible
					// immediately: the next selection finds nothing cached and
					// no file, so it synthesizes from defaults exactly as a
					// scope being seen for the first time does.
					sdh->ForgetAutoProfile(currentData);
					currentData = nullptr;
					scopeNode = nullptr;
					InvalidateAutomaticSTSSelection();
					logger::info(
						"Deleted preset '{}' and dropped the cached profile; "
						"defaults will be resynthesized on the next selection",
						removed ? deletedPath : std::string{ "<absent>" });
				}
			}
			InitCurrentScopeData();
			hookIns->bRefreshChar.store(true, std::memory_order_release);
		}

		static bool editorPreviewApplied = false;
		if (currentData) {
			const bool editing =
				hookIns->bEnableEditMode.load(std::memory_order_acquire);

			// The variant resolver is the second producer on the live-overlay
			// channel; the editor is the first. It publishes here, after profile
			// selection has already run this tick, because it stamps the current
			// selection revision onto the snapshot and a snapshot carrying a
			// stale revision is dropped by the consumer below -- which for the
			// resolver would mean one frame of base-profile values at the wrong
			// magnification, a visible flash on a 1x-to-8x variant set.
			//
			// While the editor is open it owns the channel: the resolver still
			// advances its eases (so a magnification step in progress does not
			// freeze) but does not publish over the unsaved edits.
			// Drain input regardless of edit mode.
			//
			// This used to sit inside the !editing branch below, which meant
			// that with the editor open -- exactly when someone has just
			// finished setting variants up and wants to try them -- every
			// queued notch accumulated and was never applied. The wheel looked
			// completely dead, and the accumulated deltas then fired all at once
			// on closing the editor.
			//
			// Draining here also keeps the queue from growing without bound
			// during a long edit session.
			//
			// PollOpticsKey maintains the optics-key held/tap state from the
			// physical key (GetAsyncKeyState): version-independent, no
			// DIK/VK ambiguity; see the function for the full history. It also
			// handles the menu-open case (clears the held flag) so a key
			// held across a menu can't leave the wheel stuck on the sight.
			/* PollOpticsKey() disabled: optics key is on the AddInputEvent callback (single-path, re-enabled per request). Resync below still clears held flags while a menu is open. */
			if (F4SEMenuFramework::IsAnyBlockingWindowOpened()) {
				opticsKeyHeld.store(false, std::memory_order_relaxed);
				opticsKeyConsumedByScroll.store(false, std::memory_order_relaxed);
			}
			ApplyPendingSightInput();
			// After input has moved the sight selection: swap or restore the
			// instance's zoom pointer to match, and fire the configured graph
			// events on a change. No-op on every tick nothing changed.
			if (!savingInProgress) {
				ReconcileSightZoomPointer();
			}
			// Also fire the experiment events on every ADS ENTRY (rising edge),
			// so a candidate event name can be tested by simply re-aiming:
			// edit the INI (re-read on every firing), aim, observe. Firing on
			// entry costs nothing when the list is empty.
			{
				static bool wasInADS = false;
				const bool inADS = player && IsInADS(player);
				if (inADS && !wasInADS) {
					FireSightSwapGraphEvents();
				}
				wasInADS = inADS;
			}

			// Default-on: apply each mode's per-scope default the first time we
			// are sighted with this profile (re-armed on scope change via
			// clearSelection, and on closing the editor so a freshly-ticked
			// default takes effect immediately). Force-on only -- the hotkey
			// still toggles a mode off within the session, and that sticks
			// because the profile is marked applied. Applied while sighted
			// rather than at selection, which runs before the profile is saved.
			{
				static bool wasEditingVision = false;
				if (wasEditingVision && !editing) {
					lastVisionSeededProfile = nullptr;
				}
				wasEditingVision = editing;

				if (!editing && player && IsInADS(player) &&
					currentData != lastVisionSeededProfile) {
					lastVisionSeededProfile = currentData;
					if (currentData->shaderData.bCanEnableNV &&
						currentData->shaderData.bDefaultEnableNV) {
						nvgFlag = true;
						if (hookIns) {
							hookIns->SetNVG(1);
						}
					}
					if (currentData->shaderData.bCanEnableThermal &&
						currentData->shaderData.bDefaultEnableThermal) {
						thermalFlag = true;
						if (hookIns) {
							hookIns->SetThermal(1);
						}
					}
				}
			}

			if (!editing) {
				static auto lastResolveTime =
					std::chrono::steady_clock::now();
				const auto resolveNow = std::chrono::steady_clock::now();
				const float deltaSeconds = std::clamp(
					std::chrono::duration<float>(
						resolveNow - lastResolveTime)
						.count(),
					0.0F,
					0.25F);
				lastResolveTime = resolveNow;

				auto& sessionState =
					MagnaScope::SessionStateFor(*currentData);
				ImGuiImpl::EditorPreviewSnapshot resolved;
				if (MagnaScope::ResolveOverlay(
						*currentData,
						sessionState,
						deltaSeconds,
						zoomSelectionRevision,
						resolved)) {
					ImGuiImpl::PublishEditorPreview(resolved);
				}
				// The editor needs to know which variant to point its sliders
				// at. Published rather than read from the session map, which
				// only this thread may touch.
				ImGuiImpl::PublishActiveVariantIndex(
					MagnaScope::ActiveVariantIndex(*currentData, sessionState));
			}

			const auto editorPreview =
				ImGuiImpl::GetEditorPreviewSnapshot();
			// Live editor values reach the shader only through this preview,
			// and it is gated on a revision match. A stale revision silently
			// drops every unsaved edit and falls back to the profile's saved
			// values -- 1x magnification on a profile that was never saved,
			// which looks exactly like the optical path doing nothing. Say so
			// rather than leaving it to be inferred from a missing effect.
			if (editing && editorPreview.active &&
				editorPreview.selectionRevision != zoomSelectionRevision) {
				static std::uint64_t lastReportedRevisionPair = 0U;
				const std::uint64_t pair =
					(static_cast<std::uint64_t>(
						 editorPreview.selectionRevision)
						<< 32U) |
					static_cast<std::uint32_t>(zoomSelectionRevision);
				if (lastReportedRevisionPair != pair) {
					lastReportedRevisionPair = pair;
					logger::warn(
						"Editor preview ignored: snapshot revision {} does not "
						"match selection revision {}. Unsaved edits including "
						"magnification are not being applied; the saved "
						"profile values are in use instead",
						editorPreview.selectionRevision,
						zoomSelectionRevision);
				}
			}
			// Both producers land here. The editor's snapshot and the resolver's
			// go through the same struct, the same Clamp(), and the same
			// revision gate, so there is exactly one path from a value to the
			// shader whether it came from a slider or from a variant.
			if (editorPreview.active &&
				editorPreview.selectionRevision == zoomSelectionRevision) {
				// Between kPreSaveGame and kPostSaveGame MagnaScope's values are
				// deliberately out of the zoom form so they are not serialised.
				// Writing them back inside that window would put them straight
				// into the save, which is the whole thing
				// DetachIsolatedZoomForSave exists to prevent.
				if (!savingInProgress) {
					ApplySelectedEditorPreview(editorPreview.zoomOverride);
					// The live sight glide: the same lerped values into the
					// installed runtime form, plus the graph poke that makes
					// the engine re-sample them mid-ADS.
					DriveSightZoomTransition(editorPreview.zoomOverride);
				}
				Hook::D3D::scopeApertureActivationScale.store(
					std::clamp(
						editorPreview.apertureActivationScale, 0.0F, 1.0F),
					std::memory_order_release);
				sightShiftOffset[0] = editorPreview.sightShiftX;
				sightShiftOffset[1] = editorPreview.sightShiftY;
				sightShiftOffset[2] = editorPreview.sightShiftZ;
				// Negative means "not pinned", which restores the free-scroll
				// bounds. Only a live variant set pins it.
				const float pinnedStore =
					(!editing && currentData->variants.enabled &&
						!currentData->variants.variants.empty()) ?
						std::clamp(editorPreview.magnification, 1.0F, 15.0F) :
						-1.0F;
				Hook::D3D::scopeVariantPinnedZoom.store(
					pinnedStore,
					std::memory_order_release);
				// Probe: log only when the stored pin changes, so the log shows
				// the exact tick the consumer handed a new magnification to the
				// render side. If [variant] delta lines appear but this never
				// moves, the resolver/publish leg is the stall; if this moves
				// and the screen does not, the stall is render-side.
				{
					static float lastLoggedPin = -999.0F;
					if (std::abs(pinnedStore - lastLoggedPin) > 0.01F) {
						lastLoggedPin = pinnedStore;
						logger::info(
							"[variant] consumer stored pinned={:.2f} "
							"(previewMag={:.2f}, editing={})",
							pinnedStore,
							editorPreview.magnification,
							editing);
					}
				}
				Hook::D3D::scopeCustomReticleIndex.store(
					editorPreview.customReticleIndex,
					std::memory_order_release);
				{
					// Resolve the index to a path here, on the game thread, and
					// hand the render thread a string rather than an index into
					// a list it cannot see. The load itself happens in
					// UpdateScene, which is the only place a device call is
					// safe.
					const auto& reticles = ReticlesForSelectedScope();
					const int reticleIndex = editorPreview.customReticleIndex;
					std::string reticlePath;
					if (reticleIndex >= 0 &&
						static_cast<std::size_t>(reticleIndex) < reticles.size() &&
						currentData) {
						const auto directory = currentData->ReticleDirectory();
						if (!directory.empty()) {
							reticlePath =
								(std::filesystem::path(directory) /
									reticles[static_cast<std::size_t>(
										reticleIndex)])
									.string();
						}
					}
					hookIns->RequestCustomReticleTexture(reticlePath);
				}
				Hook::D3D::scopeCustomReticleScale.store(
					std::clamp(editorPreview.customReticleScale, 0.05F, 8.0F),
					std::memory_order_release);
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
				Hook::D3D::scopeMagnificationFilter.store(
					static_cast<float>(
						std::clamp(editorPreview.magnificationFilter, 0, 2)),
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
				Hook::D3D::scopeReticleShadowStrength.store(
					editorPreview.reticleShadowStrength,
					std::memory_order_release);
				Hook::D3D::scopeReticleParallaxStrength.store(
					editorPreview.reticleParallaxStrength,
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
				Hook::D3D::scopeSceneDepth.store(
					editorPreview.sceneDepth,
					std::memory_order_release);
				Hook::D3D::scopeShadowDepth.store(
					editorPreview.shadowDepth,
					std::memory_order_release);
				Hook::D3D::scopeImageStillness.store(
					editorPreview.imageStillness,
					std::memory_order_release);
				Hook::D3D::scopeAxialBreathing.store(
					editorPreview.axialBreathing,
					std::memory_order_release);
				Hook::D3D::scopeRecenterSpeed.store(
					editorPreview.recenterSpeed,
					std::memory_order_release);
				Hook::D3D::scopeStrafeLag.store(
					editorPreview.strafeLag,
					std::memory_order_release);
				Hook::D3D::scopeTubeDepth.store(
					editorPreview.tubeDepth,
					std::memory_order_release);
				Hook::D3D::scopeLensOffsetX.store(
					editorPreview.lensOffsetX,
					std::memory_order_release);
				Hook::D3D::scopeLensOffsetY.store(
					editorPreview.lensOffsetY,
					std::memory_order_release);
				Hook::D3D::scopeLensScale.store(
					editorPreview.lensScale,
					std::memory_order_release);
				Hook::D3D::scopeBreathRate.store(
					editorPreview.breathing.rate,
					std::memory_order_release);
				Hook::D3D::scopeBreathSway.store(
					editorPreview.breathing.sway,
					std::memory_order_release);
				Hook::D3D::scopeBreathDrift.store(
					editorPreview.breathing.drift,
					std::memory_order_release);
				Hook::D3D::scopeBreathFigure.store(
					editorPreview.breathing.figure,
					std::memory_order_release);
				Hook::D3D::scopeBreathHold.store(
					editorPreview.breathing.hold,
					std::memory_order_release);
				Hook::D3D::scopeBreathPupilFollow.store(
					editorPreview.breathing.pupilFollow,
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
				sightShiftOffset[0] = 0.0F;
				sightShiftOffset[1] = 0.0F;
				sightShiftOffset[2] = 0.0F;
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
				Hook::D3D::scopeMagnificationFilter.store(
					static_cast<float>(
						std::clamp(
							currentData->shaderData.magnificationFilter,
							0,
							2)),
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
				Hook::D3D::scopeReticleShadowStrength.store(
					std::clamp(
						currentData->shaderData.reticleShadowStrength,
						0.0F,
						1.0F),
					std::memory_order_release);
				Hook::D3D::scopeReticleParallaxStrength.store(
					std::clamp(
						currentData->shaderData.reticleParallaxStrength,
						0.0F,
						4.0F),
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
				Hook::D3D::scopeSceneDepth.store(
					std::clamp(
						currentData->shaderData.parallax.sceneDepth,
						0.0F,
						4.0F),
					std::memory_order_release);
				Hook::D3D::scopeShadowDepth.store(
					std::clamp(
						currentData->shaderData.parallax.shadowDepth,
						0.0F,
						4.0F),
					std::memory_order_release);
				Hook::D3D::scopeImageStillness.store(
					std::clamp(
						currentData->shaderData.parallax.imageStillness,
						0.0F,
						8.0F),
					std::memory_order_release);
				Hook::D3D::scopeAxialBreathing.store(
					std::clamp(
						currentData->shaderData.parallax.axialBreathing,
						0.0F,
						4.0F),
					std::memory_order_release);
				Hook::D3D::scopeRecenterSpeed.store(
					std::clamp(
						currentData->shaderData.parallax.recenterSpeed,
						0.1F,
						10.0F),
					std::memory_order_release);
				Hook::D3D::scopeStrafeLag.store(
					std::clamp(
						currentData->shaderData.parallax.strafeLag,
						0.0F,
						4.0F),
					std::memory_order_release);
				Hook::D3D::scopeTubeDepth.store(
					std::clamp(
						currentData->shaderData.parallax.tubeDepth,
						0.0F,
						1.0F),
					std::memory_order_release);
				Hook::D3D::scopeLensOffsetX.store(
					std::clamp(
						currentData->shaderData.lensOffset[0],
						-1.0F,
						1.0F),
					std::memory_order_release);
				Hook::D3D::scopeLensOffsetY.store(
					std::clamp(
						currentData->shaderData.lensOffset[1],
						-1.0F,
						1.0F),
					std::memory_order_release);
				Hook::D3D::scopeLensScale.store(
					std::clamp(
						currentData->shaderData.lensScale,
						0.25F,
						2.0F),
					std::memory_order_release);
				const auto& breathing =
					currentData->shaderData.breathing;
				Hook::D3D::scopeBreathRate.store(
					std::clamp(breathing.rate, 0.0F, 4.0F),
					std::memory_order_release);
				Hook::D3D::scopeBreathSway.store(
					std::clamp(breathing.sway, 0.0F, 1.0F),
					std::memory_order_release);
				Hook::D3D::scopeBreathDrift.store(
					std::clamp(breathing.drift, 0.0F, 1.0F),
					std::memory_order_release);
				Hook::D3D::scopeBreathFigure.store(
					std::clamp(breathing.figure, 0.0F, 1.0F),
					std::memory_order_release);
				Hook::D3D::scopeBreathHold.store(
					std::clamp(breathing.hold, 0.0F, 1.0F),
					std::memory_order_release);
				Hook::D3D::scopeBreathPupilFollow.store(
					std::clamp(breathing.pupilFollow, 0.0F, 2.0F),
					std::memory_order_release);
			}

			if (!settings.AllowsProjection()) {
				hookIns->EnableRender(false);
				hookIns->QueryRender(false);
				InvalidateAutomaticSTSSelection();
				callOriginal();
				return;
			}

			// Nothing optical happens from a detached camera. Bail alongside
			// the projection gate rather than deeper in, so the whole
			// per-frame chain -- aperture search, projection, eye-box
			// tracking, and the draw classification that feeds the replay --
			// is skipped rather than computed and then discarded.
			//
			// Invalidating the selection matters as much as clearing the
			// render flags: the classifier matches draws against a published
			// aperture identity, and leaving a stale one live would let a
			// third-person weapon draw match it.
			if (!IsFirstPersonCameraView()) {
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

			// The inherited scope hook read first-person world transforms before
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
			RE::NiPoint3 scopeProjectionPoint{};
			RE::NiPoint3 previousScopeProjectionPoint{};
			RE::NiPoint3 aimProjectionPoint{};
			float scopeWorldRadius = 0.0F;
			// The vertex-measured radius of the selected mesh, which is what
			// the synthesized aperture is drawn at. Zero means the shape had no
			// readable CPU vertex copy, and the synthesized path stays off.
			float scopeMeasuredRadius = 0.0F;
			int scopeOpticalAxis = 1;
			// The optical centre from the mesh's own vertices, used to place
			// the synthesized ring. Everything else still uses the published
			// bound centre, so this cannot move the existing paths.
			RE::NiPoint3 scopeMeasuredCenter{};
			// The disc-like mesh that sizes and places the synthesized optic.
			RE::NiAVObject* scopeSynthSource = nullptr;
			// The optical plane's own model-space rim, which is the unit the
			// magnify shader's lens coordinates are in.
			float scopeLensLocalRadius = 0.0F;
			// Whether this aperture can drive the authored-geometry replay.
			// It decides which signal proves the optic rendered this frame.
			bool scopeSupportsExactReplay = true;
			if (currentData->autoProfile) {
				Hook::HangDiag::updateTicks.fetch_add(
					1U,
					std::memory_order_relaxed);
				Hook::HangDiag::updateLoopThread.store(
					GetCurrentThreadId(),
					std::memory_order_relaxed);
				Hook::HangDiag::updatePhase.store(
					1,
					std::memory_order_relaxed);
				// NOTE deliberately absent: no first-person weapon shift here.
				// Shifting the weapon subtree mid-frame to emulate a live
				// cameraOffset change tore the scope render apart in game --
				// skinned arms, the capture fingerprints, and the optical
				// projections all disagreed about the pose. A sight swap
				// writes the form's zoom data instead and the engine applies
				// it on the next aim-in. The engine-cooperative alternative is
				// MSF's UpdateAnimGraph refresh, which needs its 1.10.163
				// address (absent from the partial MSF source drop).
				const auto aperture = FindSTSAperture(firstPersonRoot);
				Hook::HangDiag::updatePhase.store(
					2,
					std::memory_order_relaxed);
				scopeNode = aperture.opticalPlane;
				// Withhold the draw identity when the aperture is not the
				// standardized annulus. Publishing it would let the replay
				// match and replace a mesh whose lens coordinates the fill
				// shader cannot derive, which renders a confidently wrong lens
				// rather than falling back. The projection below is published
				// either way, so the screen-space path keeps the opening.
				// The composite needs to tell "not this frame" from "not ever".
				// Without it a non-annulus aperture waits for a replay that
				// cannot happen and the scope shows nothing at all.
				Hook::D3D::automaticSTSApertureSupportsExactReplay.store(
					aperture.supportsExactReplay,
					std::memory_order_release);
				// The fill shader derives inner-ring lens coordinates and the
				// centre-fan apex from this ratio. Authored rings carry the
				// measured value; the synthesized ring is built at exactly
				// half, so anything not driving the exact replay publishes
				// 0.5. A ratio outside the plausible annulus band means the
				// measurement ran on something that is not a ring, and 0.5 is
				// the established behaviour for that case.
				{
					float innerRatio = 0.5F;
					if (aperture.supportsExactReplay &&
						aperture.measurementValid &&
						std::isfinite(aperture.measuredInnerRatio) &&
						aperture.measuredInnerRatio > 0.2F &&
						aperture.measuredInnerRatio < 0.8F) {
						innerRatio = aperture.measuredInnerRatio;
					}
					Hook::D3D::scopeApertureInnerRatio.store(
						innerRatio,
						std::memory_order_release);
				}
				hookIns->PublishAutomaticSTSGeometry(
					aperture.supportsExactReplay ?
						aperture.renderSurface :
						nullptr,
					aperture.reticleSurfaces,
					aperture.extentReference);
				// Hash-gated: a no-op every frame the sphere settings, the
				// selection, and the scope-active state are unchanged.
				UpdateScopeOcclusion(firstPersonRoot, scopeNode);
				scopeProjectionPoint = aperture.worldCenter;
				previousScopeProjectionPoint =
					aperture.previousWorldCenter;
				aimProjectionPoint = aperture.aimWorldCenter;
				scopeWorldRadius = aperture.worldRadius;
				// Lens Size is applied to the generated geometry rather than
				// only to the shader mask, because the mask cannot grow the
				// optic past the silhouette that is drawn -- which is exactly
				// why raising it did nothing when it lived on the mask alone.
				const float lensSize = std::clamp(
					currentData->shaderData.lensScale,
					0.05F,
					8.0F);
				// The x3 that used to sit here was a mistake founded on a
				// circular measurement: heuristicOverMeasured reads 3.0 on
				// every scope because the heuristic IS planeRadius * 3.0 and
				// the measurement approximately equals planeRadius. The
				// captured-placement path needs no compensation at all -- the
				// game's own vertex shader places the ring -- and the NDC
				// fallback keeps whatever error the CPU projection has rather
				// than an invented constant on top of it.
				scopeMeasuredRadius = aperture.measurementValid ?
					aperture.measuredWorldRadius * lensSize :
					0.0F;
				scopeOpticalAxis = aperture.measuredOpticalAxis;
				scopeSupportsExactReplay = aperture.supportsExactReplay;
				scopeSynthSource = aperture.synthesisSource;
				scopeLensLocalRadius = aperture.lensLocalRadius;

				// Feed the classifier the synthesis source's draw identity
				// and the model-space ring frame. The captured-placement
				// path draws at the mesh's own local radius; the game's
				// transform constants apply world scale exactly as they did
				// to the mesh's vertices.
				if (!aperture.supportsExactReplay &&
					aperture.measurementValid && scopeSynthSource &&
					MagnaScope::GetSettings().AllowsSynthesizedAperture()) {
					hookIns->PublishSynthesisPlacement(
						scopeSynthSource,
						aperture.measuredLocalCentroid,
						aperture.measuredLocalRadius * lensSize,
						aperture.measuredOpticalAxis);
				} else {
					Hook::D3D::InvalidateSynthesisPlacement();
				}

				// Place the NDC fallback ring on the measured optical centre
				// of the synthesis source rather than on worldBound.center.
				// On specter_lens_rear the two disagree by roughly 570 screen
				// pixels vertically, which put the whole optic below the
				// scope it belongs to.
				if (aperture.measurementValid && scopeSynthSource) {
					scopeMeasuredCenter =
						scopeSynthSource->world *
						aperture.measuredLocalCentroid;
					if (!IsFinitePoint(scopeMeasuredCenter)) {
						scopeMeasuredCenter = aperture.worldCenter;
					}
				} else {
					scopeMeasuredCenter = aperture.worldCenter;
				}

				if (scopeNode && IsInADS(player)) {
					static RE::NiAVObject* lastLoggedAperture = nullptr;
					if (lastLoggedAperture != scopeNode) {
						lastLoggedAperture = scopeNode;
						logger::verbose(
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

			// Vision sources: refresh the blob list while thermal OR night
			// vision is active on a permitting scope. In live play each want
			// mirrors the render-side two-gate (permission AND live hotkey).
			// In the editor the preview enable comes from the UI checkboxes,
			// not the saved permission, so we publish BOTH channels while
			// editing and let the shader pick -- otherwise a mode toggled on
			// live (but not yet saved) would show the overlay with no blobs.
			// Actors feed thermal; light/fire emitters feed both.
			const bool editingVision = hookIns &&
				hookIns->bEnableEditMode.load(std::memory_order_acquire);
			bool wantThermalSrc;
			bool wantNVSrc;
			if (editingVision) {
				wantThermalSrc = currentData != nullptr;
				wantNVSrc = currentData != nullptr;
			} else {
				wantThermalSrc = thermalFlag && currentData &&
					currentData->shaderData.bCanEnableThermal;
				wantNVSrc = nvgFlag && currentData &&
					currentData->shaderData.bCanEnableNV;
			}
			if (wantThermalSrc || wantNVSrc) {
				PublishVisionSources(player, wantThermalSrc, wantNVSrc);
			}

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

					// Everything Convert Zoom Data needs, measured where the
					// camera, the aim reference, the scope node and the
					// projected radius are all in hand at once. The editor
					// cannot derive any of it: BGSZoomData's offsets are
					// relative to a default eye position the engine never
					// exposes.
					ImGuiImpl::ApertureGeometrySnapshot geometry{};
					// SightHelper's formula, unchanged. cameraOffset is the
					// camera-space vector from this Camera node to the point
					// that should sit on the view axis, so camera-space X and Y
					// map straight onto offset X and Z with no negation. The
					// signs I previously inferred from the projection's
					// ndcX = -cameraPoint.x convention were both wrong.
					const RE::NiPoint3 aimCameraSpace =
						camNode->world.rotate *
						(aimProjectionPoint - camNode->world.translate);
					geometry.offsetFrameX = aimCameraSpace.x;
					geometry.offsetFrameZ = aimCameraSpace.y;
					geometry.offsetFrameY = -aimCameraSpace.z;
					geometry.distance = std::sqrt(
						aimCameraSpace.x * aimCameraSpace.x +
						aimCameraSpace.y * aimCameraSpace.y +
						aimCameraSpace.z * aimCameraSpace.z);
					geometry.projectedRadiusPixels = 0.5F *
						(apertureProjection.radiusX +
							apertureProjection.radiusY);

					// The forward depth of the glass itself. A bounding sphere
					// around the whole sighted assembly was the wrong measure:
					// its near face is not the eyepiece, and trusting it let
					// the eye travel to within 3.9 units of a lens 18.75 units
					// away -- i.e. inside the optic.
					const RE::NiPoint3 apertureCameraSpace =
						camNode->world.rotate *
						(scopeProjectionPoint - camNode->world.translate);
					geometry.apertureForwardDistance = -apertureCameraSpace.z;
					geometry.selectionRevision = zoomSelectionRevision;
					geometry.available =
						std::isfinite(geometry.distance) &&
						geometry.distance > 0.01F &&
						apertureProjection.valid;
					if (geometry.available) {
						ImGuiImpl::PublishApertureGeometry(geometry);
					}
				} else {
					tempOut = hookIns->WorldPointToScreen(
						camNode,
						scopeProjectionPoint,
						firstPersonFov);
				}
				// Editor gizmo: the occlusion sphere's vertices, expressed in
				// the fade mesh's LOCAL space and encoded in the game's own
				// 20-byte vertex format. The render thread draws them through
				// the game's live ScopeFade pipeline, so clip position and
				// depth come from the exact shader and constants that placed
				// the housing -- correctness inherited, not reconstructed.
				// Fade-local coordinates are sway-invariant (the sphere and
				// the fade node ride one rig), so the payload only changes
				// when the sliders move, and equality is the republish gate.
				{
					static RE::NiPoint3 lastCenterLocal{};
					static float lastRadiusLocal = -1.0F;
					static bool lastActive = false;
					ScopeData::OcclusionSettings occlusionPreview;
					const bool sphereWanted =
						hookIns->bEnableEditMode.load(
							std::memory_order_acquire) &&
						ImGuiImpl::OcclusionSphereGeoWanted() &&
						ImGuiImpl::GetOcclusionPreview(occlusionPreview);
					if (sphereWanted) {
						const auto& glassWorld = scopeNode->world;
						const float glassScale =
							glassWorld.scale > 1e-6F ? glassWorld.scale : 1.0F;
						const RE::NiPoint3 offsetLocal{
							occlusionPreview.sphereOffset[0],
							occlusionPreview.sphereOffset[1],
							occlusionPreview.sphereOffset[2]
						};
						const RE::NiPoint3 sphereWorld =
							glassWorld.translate +
							glassWorld.rotate.Transpose() * offsetLocal;
						const RE::NiPoint3 centerLocal =
							(glassWorld.rotate *
								(sphereWorld - glassWorld.translate)) *
							(1.0F / glassScale);
						const float radiusLocal =
							occlusionPreview.sphereRadius / glassScale;
						// The periodic force heals desyncs the detector cannot
						// see, e.g. the no-scope reset publishing inactive while
						// these statics still say "published".
						static std::uint32_t republishTick = 0U;
						const bool changed = !lastActive ||
							(++republishTick % 120U == 0U) ||
							std::abs(centerLocal.x - lastCenterLocal.x) > 1e-4F ||
							std::abs(centerLocal.y - lastCenterLocal.y) > 1e-4F ||
							std::abs(centerLocal.z - lastCenterLocal.z) > 1e-4F ||
							std::abs(radiusLocal - lastRadiusLocal) > 1e-4F;
						if (changed) {
							const auto& unitTable = OcclusionSphereUnitTable();
							Hook::D3D::OcclusionSphereGeo sphereGeo{};
							sphereGeo.vertexCount =
								static_cast<std::uint32_t>(unitTable.size());
							sphereGeo.fadeLocalVertices.assign(
								static_cast<std::size_t>(sphereGeo.vertexCount) *
									20U,
								0U);
							std::uint8_t* cursor =
								sphereGeo.fadeLocalVertices.data();
							for (const auto& direction : unitTable) {
								const std::uint16_t encoded[4] = {
									FloatToHalfBits(
										centerLocal.x +
										direction.x * radiusLocal),
									FloatToHalfBits(
										centerLocal.y +
										direction.y * radiusLocal),
									FloatToHalfBits(
										centerLocal.z +
										direction.z * radiusLocal),
									FloatToHalfBits(1.0F)
								};
								std::memcpy(cursor, encoded, sizeof(encoded));
								cursor += 20U;
							}
							sphereGeo.color[0] = 1.0F;
							sphereGeo.color[1] = 0.3F;
							sphereGeo.color[2] = 0.25F;
							sphereGeo.color[3] = 0.35F;
							sphereGeo.active = true;
							Hook::D3D::PublishOcclusionSphereGeo(
								std::move(sphereGeo));
							logger::info(
								"[sphere-gizmo] published fade-local sphere: "
								"center=({:.2f}, {:.2f}, {:.2f}) radius={:.2f} "
								"(glass scale {:.3f})",
								centerLocal.x,
								centerLocal.y,
								centerLocal.z,
								radiusLocal,
								glassScale);
							lastCenterLocal = centerLocal;
							lastRadiusLocal = radiusLocal;
							lastActive = true;
						}
					} else if (lastActive) {
						lastActive = false;
						lastRadiusLocal = -1.0F;
						Hook::D3D::PublishOcclusionSphereGeo({});
					}
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
				gcb.scopeLocalMat = scopeNode->local.rotate;
				gcb.scopeWorldMat = scopeNode->world.rotate;
				gcb.scopeScreenPos = tempOut;
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
						// One measured interval per frame, shared by the aim
						// transition and the eye-box filters.
						const float opticalFrameDelta =
							AcquireOpticalFrameDelta();
						// Reproject the synthesized aperture ring for this
						// frame. It is generated from the selected mesh's
						// transform and measured radius, so it tracks recoil,
						// sway and weapon movement the same way the authored
						// ScopeFade does -- there is no smoothing or carried
						// state to go stale.
						//
						// This runs before the activation update because the
						// ring's validity is what stands in for the observed
						// ScopeFade draw below.
						bool synthesizedRingValid = false;
						Hook::HangDiag::updatePhase.store(
							3,
							std::memory_order_relaxed);
						if (MagnaScope::GetSettings()
								.AllowsSynthesizedAperture() &&
							scopeMeasuredRadius > 0.0F && scopeSynthSource) {
							synthesizedRingValid =
								hookIns->PublishSynthesizedApertureRing(
									camNode,
									scopeSynthSource,
									scopeMeasuredCenter,
									scopeMeasuredRadius,
									scopeOpticalAxis,
									firstPersonFov);
						} else {
							Hook::D3D::InvalidateSynthesizedApertureRing();
						}

						// Activation normally waits for ScopeAiming to actually
						// submit its ScopeFade draw, because a matched draw is
						// proof the optic rendered this frame. A synthesized
						// aperture has no such draw: nothing is published for
						// the classifier to match, so that signal is false
						// forever, activation never leaves zero, and the pixel
						// shader's magnification stays at lerp(1, x, 0) == 1.
						// The lens then draws correctly and does nothing
						// visible, which is exactly what a scope without an
						// authored ScopeFade reported.
						//
						// Stand in for it with the two facts that are actually
						// available: the player is sighted, and a ring was
						// built this frame from live scene-graph data. Both are
						// per-frame, so a lost aperture still stops activation
						// advancing rather than latching it on.
						const bool exactApertureVisible =
							Hook::D3D::automaticSTSScopeFadeVisibleLastFrame
								.load(std::memory_order_acquire);
						// A matched placement draw is the synthesized optic's
						// proof of rendering, and it is the strongest signal
						// available: the lens mesh was actually submitted this
						// frame. The CPU-projected ring is only a secondary
						// witness now -- tying activation to it alone left
						// activation pinned at zero whenever that projection
						// failed, which multiplied the entire optical result,
						// magnification included, by nothing.
						const bool placementVisible =
							Hook::D3D::automaticSTSPlacementVisibleLastFrame
								.load(std::memory_order_acquire);
						const bool apertureVisible =
							scopeSupportsExactReplay ?
								exactApertureVisible :
								((placementVisible || synthesizedRingValid) &&
									IsInADS(player));
						const float activationProgress =
							UpdateAutomaticSTSTracking(
								scopeNode,
								apertureProjection,
								apertureVisible,
								opticalFrameDelta);
						Hook::HangDiag::updatePhase.store(
							4,
							std::memory_order_relaxed);
						const Hook::D3D::PhysicalEyeBoxSample physicalEyeBox =
							UpdateAutomaticSTSEyeBoxTracking(
								currentData,
								scopeNode,
								camNode,
								scopeProjectionPoint,
								scopeWorldRadius,
								firstPersonFov,
								activationProgress,
								opticalFrameDelta,
								tempOut,
								aimProjectionPoint,
								scopeLensLocalRadius);
						Hook::HangDiag::updatePhase.store(
							0,
							std::memory_order_relaxed);
						// Physical aperture and authored reticle centers must use the
						// same current-frame projection as the ScopeFade geometry and
						// lens basis. Smoothing either point against current geometry
						// changes the clipping frame at extreme pitch. Optical inertia
						// is already filtered independently by the eye-box tracker.
						Hook::D3D::scopeApertureScaleRatio.store(
							physicalEyeBox.valid ?
								physicalEyeBox.apertureScaleRatio :
								1.0F,
							std::memory_order_release);
						// Breathing phase is integrated here rather than read
						// from a clock in the shader. Multiplying an absolute
						// time by the rate makes every rate change teleport the
						// image to a new point on the curve, which is very
						// visible while dragging the slider; integrating means
						// the curve simply bends forward from where it was.
						//
						// It advances only while the optic is live, so a breath
						// does not silently continue through a loading screen
						// and resume at an arbitrary point.
						{
							constexpr float kTwoPi = 6.28318530717958647692F;
							const float breathRate = std::clamp(
								Hook::D3D::scopeBreathRate.load(
									std::memory_order_acquire),
								0.0F,
								4.0F);
							float breathPhase =
								Hook::D3D::scopeBreathPhase.load(
									std::memory_order_acquire) +
								kTwoPi * breathRate * opticalFrameDelta;
							if (!std::isfinite(breathPhase)) {
								breathPhase = 0.0F;
							}
							// Wrapping keeps the argument small enough that
							// sin() does not lose precision after an hour of
							// aiming, which would show up as the sway slowly
							// going ragged.
							breathPhase = std::fmod(breathPhase, kTwoPi);
							if (breathPhase < 0.0F) {
								breathPhase += kTwoPi;
							}
							Hook::D3D::scopeBreathPhase.store(
								breathPhase,
								std::memory_order_release);
						}
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
						// Preserve the last publication and monotonic activation
						// while the CPU projection is temporarily unavailable. A
						// real selection change is handled by the selection reset,
						// not by this transient render-time condition.
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
					if (scopeTimer >= sdh->GetCurrentScopeProfile()->scopeFrame) {
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
								// The original scope-rendering reasserts these fields while
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
					// exactly like the original scope-rendering behaved.
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
			// These three are latched by the overlay consumer above, which only
			// runs while a scope is selected. Without resetting them here, an
			// unequip taken while a secondary sight was up would leave the
			// aperture faded to zero permanently, and the next scope would be
			// invisible with nothing in the log to say why. The reticle pair is
			// reset for the same reason.
			Hook::D3D::scopeApertureActivationScale.store(
				1.0F,
				std::memory_order_release);
			Hook::D3D::scopeVariantPinnedZoom.store(
				-1.0F,
				std::memory_order_release);
			Hook::D3D::scopeCustomReticleIndex.store(
				-1,
				std::memory_order_release);
			hookIns->RequestCustomReticleTexture(std::string{});
			// Drop every occlusion substitution with the scope. A holed
			// housing must never survive onto the hip weapon or the next
			// scope; publishing empty is the fail-open state.
			UpdateScopeOcclusion(nullptr, nullptr);
			Hook::D3D::PublishOcclusionSphereGeo({});
			// A sight shift must not survive either -- it moves the
			// first-person weapon every frame it is nonzero.
			sightShiftOffset[0] = 0.0F;
			sightShiftOffset[1] = 0.0F;
			sightShiftOffset[2] = 0.0F;
			Hook::D3D::scopeFadeMagnification.store(
				1.0F,
				std::memory_order_release);
			Hook::D3D::scopeImageDenoise.store(
				0.0F,
				std::memory_order_release);
			Hook::D3D::scopeImageSharpen.store(
				0.0F,
				std::memory_order_release);
			Hook::D3D::scopeMagnificationFilter.store(
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
			Hook::D3D::scopeReticleShadowStrength.store(
				0.0F,
				std::memory_order_release);
			Hook::D3D::scopeReticleParallaxStrength.store(
				1.0F,
				std::memory_order_release);
			Hook::D3D::scopeSceneParallaxStrength.store(
				0.0F,
				std::memory_order_release);
			Hook::D3D::scopeOpticalLagStrength.store(
				1.0F,
				std::memory_order_release);
			Hook::D3D::scopeSceneDepth.store(
				1.0F,
				std::memory_order_release);
			Hook::D3D::scopeShadowDepth.store(
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
				sdh->SetCurrentScopeProfile(nullptr);
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
			if (!IsSideAim() && !player->IsInThirdPerson() && bNeedToUpdateScopeProfile) {
				if (bChangeAnimFlag) {
					InitCurrentScopeData();
				}

				currentData = sdh->GetCurrentScopeProfile();

				if (currentData && !bHasStartedScope) {
					hookIns->StartScope(true);
					hookIns->SetFinishAimAnim(true);
					bHasStartedScope = true;
				}

				bNeedToUpdateScopeProfile = false;
			}

			if (sdh->GetCurrentScopeProfile() && sdh->GetCurrentScopeProfile()->shaderData.bBoltDisable) {
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
			bNeedToUpdateScopeProfile = true;
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
	// Keep the legacy Papyrus script name so existing legacy patches continue to
	// bind without requiring authors to rebuild their script assets.
	constexpr std::string_view fileName = "MagnaScope";
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
		// before-render callback. The original scope-rendering Present, ResizeBuffers,
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
	// One directory walk, here, rather than one per weapon swap.
	RediscoverReticles();
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

	if (sdh->GetCurrentScopeProfile()) {
		gameDeltaZoom = sdh->GetCurrentScopeProfile()->shaderData.minZoom;
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
	if (settings.AllowsWorldColorCapture() &&
		!MagnaScope::WorldOnlyScopeRenderer::GetSingleton().InstallHooks()) {
		logger::error(
			"Pre-first-person world color capture hooks were not installed; automatic STS scopes will fail open to authored behavior");
	}

	REL::Trampoline& trampoline = REL::GetTrampoline();
	// Resolve the fixed OG relocation only after the runtime gate above. Eager
	// namespace-scope resolution could abort unsupported NG/AE loads before the
	// plugin had a chance to fail safely.
	const REL::Relocation<std::uintptr_t> pcUpdateMainThread{
		REL::ID(633524),
		0x22D
	};
	const auto updateCallSite = pcUpdateMainThread.address();
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

	// Scope selections live in the co-save so they fork with the save. A
	// failure here is not fatal: the plugin runs, selections just do not
	// persist, and RegisterCoSave has already said so in the log.
	(void)MagnaScope::RegisterCoSave();

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
				// Broadcast the plugin interface. kPostLoad is the point every
				// other F4SE plugin has loaded and registered its listener, so
				// this reaches consumers regardless of load order.
				// Three-way split of optics input, by what actually works in
				// game (confirmed via the [input] logs):
				//   * BINDING lives in the framework's hotkey registry (DIK scan
				//     codes, persisted in PluginHotkeys.ini, conflict warnings,
				//     rebind UI). Its press-only callback is intentionally empty.
				//   * The WHEEL is detected by the AddInputEvent callback below,
				//     which (v3.4+) does deliver MOUSE events reliably.
				//   * The KEY is polled (PollOpticsKey). The framework
				//     callback historically delivered no keyboard (mouse-only
				//     PlayerControls receiver) and now does (PlayerCamera full-queue
				//     fix), but the poll is kept: version-free, no DIK/VK ambiguity.
				// The InputEvent handle is leaked for process lifetime.
				F4SEMenuFramework::Hotkeys::Register(
					kOpticsHotkeyId,
					kOpticsHotkeyDefault,
					[]() {
						// Registration only; PollOpticsKey does the tap/hold
						// semantics.
					});
				static F4SEMenuFramework::Model::InputEvent* opticsInput =
					F4SEMenuFramework::AddInputEvent(MagnaScopeInputCallback);  /* Re-enabled per request: the callback is the input path (wheel + key). Keep registered -- do not disable again unless explicitly told. The framework added an [InputQueueHook] diagnostic to localise why this callback was not dispatching. */
				(void)opticsInput;
				logger::info(
					"Registered optics hotkey '{}' with F4SE Menu Framework "
					"(current binding scan code {})",
					kOpticsHotkeyId,
					F4SEMenuFramework::Hotkeys::GetBinding(kOpticsHotkeyId));

				if (const auto* messaging = F4SE::GetMessagingInterface()) {
					messaging->Dispatch(
						MagnaScopeAPI::kInterfaceMessage,
						MagnaScopeAPI::GetInterface(),
						sizeof(void*),
						nullptr);
					logger::info("Dispatched the MagnaScope plugin interface (v1)");
				}
			} else if (msg->type == F4SE::MessagingInterface::kGameDataReady) {
				InitializePlugin();

			} else if (msg->type == F4SE::MessagingInterface::kPostLoadGame) {
				ResetScopeStatus();
			} else if (msg->type == F4SE::MessagingInterface::kPreLoadGame) {
				ClearIsolatedZoomSession();
				sdh->SetCurrentScopeProfile(nullptr);
				currentData = nullptr;
				weaponInstanceData = nullptr;
				lastEquippedInstance = nullptr;
				lastAttachmentKey.clear();
				hasScopeSelectionSnapshot = false;
			} else if (msg->type == F4SE::MessagingInterface::kPreSaveGame) {
				savingInProgress = true;
				DetachIsolatedZoomForSave();
			} else if (msg->type == F4SE::MessagingInterface::kNewGame) {
				ResetScopeStatus();
			} else if (msg->type == F4SE::MessagingInterface::kPostSaveGame) {
				ReattachIsolatedZoomAfterSave();
				savingInProgress = false;
			} else if (msg->type == F4SE::MessagingInterface::kGameLoaded) {
				//reshadeImpl->SetRenderEffect(false);
			}
		})) {
		logger::critical("Unable to register the F4SE messaging listener");
		return false;
	}

	return true;
}
