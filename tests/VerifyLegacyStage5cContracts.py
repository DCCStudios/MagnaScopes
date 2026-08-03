"""Historical Stage 5c fail-closed checks for MagnaScope's OG ADS boundaries.

This test intentionally validates both source structure and the exact
Fallout 4 1.10.163 address-library corpus.  It catches the class of failure
that caused the A37942 ADS crashes: treating an address inside an engine
function as a callable function entry point.
"""

from __future__ import annotations

import math
import re
import struct
import sys
from pathlib import Path


HUD_WORLD_TO_SCREEN_ID = 1_132_313
HUD_WORLD_TO_SCREEN_OG_RVA = 0xAE2840
TAA_VTABLE_ID = 1_340_364
TAA_VTABLE_OG_RVA = 0x3098B88
CRASH_CALL_RVA = 0xA37940
CRASH_OWNER_ID = 386_055
CRASH_OWNER_RVA = 0xA37890
WORLD_ONLY_OG_TARGETS = {
    984_743: 0x2857480,
    1_048_494: 0x282EF70,
    1_491_502: 0x1D12980,
    1_264_353: 0x284F1D0,
    901_559: 0x1D1B730,
    1_430_301: 0x6723358,
    163_482: 0x609DBDC,
    382_658: 0x609DBE4,
}


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def load_address_library(path: Path) -> dict[int, int]:
    data = path.read_bytes()
    require(len(data) >= 8, f"truncated address-library header: {path}")
    count = struct.unpack_from("<Q", data, 0)[0]
    require(
        len(data) == 8 + count * 16,
        f"address-library size mismatch: {path}",
    )
    entries: dict[int, int] = {}
    for index in range(count):
        relocation_id, rva = struct.unpack_from("<QQ", data, 8 + index * 16)
        entries[relocation_id] = rva
    return entries


def strip_cpp_comments(source: str) -> str:
    source = re.sub(r"/\*.*?\*/", "", source, flags=re.DOTALL)
    return re.sub(r"//.*", "", source)


def main() -> int:
    project = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path.cwd()
    workspace = project.parent
    hooking = (project / "src" / "hooking.cpp").read_text(encoding="utf-8")
    hooking_h = (project / "src" / "hooking.h").read_text(encoding="utf-8")
    main_cpp = (project / "src" / "main.cpp").read_text(encoding="utf-8")
    profile_data = (project / "src" / "ScopeProfile.cpp").read_text(encoding="utf-8")
    profile_data_h = (project / "src" / "ScopeProfile.h").read_text(encoding="utf-8")
    imgui = (project / "src" / "ImGuiImpl.cpp").read_text(encoding="utf-8")

    # The experimental Stage 5 auxiliary renderer was retired after its broad
    # world-render hooks were implicated in an external-culling crash. Keep
    # this historical regression test useful by enforcing the only supported
    # production contract: verificationStage=5 must return before any normal
    # MagnaScope hook, menu, Papyrus, or profile initialization occurs.
    retired_stage5 = main_cpp.find("if (settings.verificationStage == 5)")
    normal_plugin_init = main_cpp.find(
        "hookIns = Hook::D3D::GetSington();",
        retired_stage5,
    )
    if retired_stage5 >= 0:
        retired_body = main_cpp[retired_stage5:normal_plugin_init]
        require(
            normal_plugin_init > retired_stage5
            and "Stage 5 auxiliary-renderer diagnostics are retired" in retired_body
            and "failed closed" in retired_body
            and "return true;" in retired_body
            and "InstallHooks(" not in retired_body
            and "InstallObservationHooks(" not in retired_body
            and "InstallPassThroughHooks(" not in retired_body
            and "RequestFrame(" not in retired_body
            and "InitializePlugin(" not in retired_body
            and "RegisterFuncs(" not in retired_body,
            "retired Stage 5 configuration can reach production initialization",
        )
        print("Legacy Stage 5 contract passed: retired configuration fails closed")
        return 0
    settings = (project / "src" / "Settings.h").read_text(encoding="utf-8")
    world_only_renderer = (
        project / "src" / "WorldOnlyScopeRenderer.cpp"
    ).read_text(encoding="utf-8")
    world_only_renderer_header = (
        project / "src" / "WorldOnlyScopeRenderer.h"
    ).read_text(encoding="utf-8")
    ini = (project / "MagnaScope.ini").read_text(encoding="utf-8")
    stage5c_observation_ini = (
        project / "tests" / "config" / "MagnaScope.Stage5c-a.ini"
    ).read_text(encoding="utf-8")
    stage5c_world_pass_ini = (
        project / "tests" / "config" / "MagnaScope.Stage5c.ini"
    ).read_text(encoding="utf-8")
    stage5d_late_replay_ini = (
        project / "tests" / "config" / "MagnaScope.Stage5d.ini"
    ).read_text(encoding="utf-8")
    geometry_probe_shader = (
        project / "src" / "HLSL" / "ScopeGeometryProbe_PS.hlsl"
    ).read_text(encoding="utf-8")
    geometry_fill_shader = (
        project / "src" / "HLSL" / "ScopeGeometryFill_GS.hlsl"
    ).read_text(encoding="utf-8")
    geometry_magnify_shader = (
        project / "src" / "HLSL" / "ScopeGeometryMagnify_PS.hlsl"
    ).read_text(encoding="utf-8")
    reticle_layer_shader = (
        project / "src" / "HLSL" / "ReticleLayer_PS.hlsl"
    ).read_text(encoding="utf-8")
    geometry_magnify_code = strip_cpp_comments(geometry_magnify_shader)
    triangle_shader = (
        project / "src" / "HLSL" / "Triangle.hlsli"
    ).read_text(encoding="utf-8")
    code = strip_cpp_comments(
        "\n".join(
            (
                hooking,
                main_cpp,
                profile_data,
                imgui,
                settings,
            )
        )
    )

    require(
        "cam->world.rotate * delta" in code
        and "const float forward = -cameraPoint.z" in code,
        "first-person projection does not preserve the original scope-rendering camera-axis contract",
    )
    require(
        '"Reticle:0"' in code
        and '"ScopeFade:0"' in code
        and "opticalPlane" in code
        and "aimReference" in code
        and "extentReference" in code
        and "scopeViewParts->GetObjectByName" in code
        and "worldBound" in code
        and "ProjectWorldSphereToScreen" in code
        and "WorldPointToScreen" in code,
        "automatic STS projection does not use authored aperture geometry",
    )
    fade_candidate = main_cpp.find('"ScopeFade:0"')
    reticle_candidate = main_cpp.find('"Reticle:0"', fade_candidate)
    require(
        fade_candidate >= 0 and reticle_candidate > fade_candidate,
        "automatic STS selection must use ScopeFade as the optical plane before the reticle aim reference",
    )
    aperture_selection_start = main_cpp.find(
        "STSApertureSelection FindSTSAperture"
    )
    aperture_aim_reference = main_cpp.find(
        "RE::NiAVObject* aimReference",
        aperture_selection_start,
    )
    require(
        aperture_selection_start >= 0
        and aperture_aim_reference > aperture_selection_start
        and 'GetObjectByName("ScopeFade:0")'
        in main_cpp[aperture_selection_start:aperture_aim_reference]
        and "planeNames" not in main_cpp[
            aperture_selection_start:aperture_aim_reference
        ],
        "generated STS lens does not require the universal ScopeFade plane",
    )
    hooked_update = main_cpp.find("void HookedUpdate()")
    original_after_lifecycle = main_cpp.find(
        "callOriginal();",
        main_cpp.find("if (!bFirstTimeZoomData)", hooked_update),
    )
    post_update_projection = main_cpp.find(
        "ProjectWorldSphereToScreen(",
        hooked_update,
    )
    require(
        hooked_update >= 0
        and original_after_lifecycle > hooked_update
        and post_update_projection > original_after_lifecycle,
        "automatic STS projection is still published before the game refreshes first-person world transforms",
    )
    require(
        "projectedLensRadiusX" in hooking
        and "projectedLensRadiusY" in hooking
        and "projectedActivationProgress" in hooking
        and "projection.radiusX * scaleX" in imgui
        and "projection.radiusY * scaleY" in imgui
        and "fullRadius * projection.activationProgress" in imgui
        and "referenceDiameter" not in imgui,
        "Stage 4a does not use projected authored bounds with an inside-only activation transition",
    )
    aperture_tracking_start = main_cpp.find("float UpdateAutomaticSTSTracking(")
    aperture_tracking_end = main_cpp.find(
        "bool IsFinitePoint(",
        aperture_tracking_start,
    )
    aperture_tracking = main_cpp[
        aperture_tracking_start:aperture_tracking_end
    ]
    require(
        aperture_tracking_start >= 0
        and aperture_tracking_end > aperture_tracking_start
        and "kActivationDurationSeconds" in aperture_tracking
        and "easedProgress" in aperture_tracking
        and "if (!apertureVisible)" in aperture_tracking
        and "previousCenterX" not in aperture_tracking
        and "stableSamples" not in aperture_tracking,
        "automatic STS aperture aim-in still smooths position or waits for a settle latch",
    )
    require(
        "UpdateAutomaticSTSEyeBoxTracking(" in main_cpp
        and "cameraApertureLocal.x" in main_cpp
        and "cameraApertureLocal.z" in main_cpp
        and "state.baselineEyeLocalX" in main_cpp
        and "state.baselineEyeLocalZ" in main_cpp
        and "ProjectLocalEyeOffsetToScreen(" in main_cpp
        and "averageBasisLength" in main_cpp
        and "kOpticalFollowerTimeConstant" in main_cpp
        and "kOpticalFollowerTimeConstant = 0.090F" in main_cpp
        and "const float followerBlend =" in main_cpp
        and "state.baselineEyeLocalX +=" in main_cpp
        and "state.baselineEyeLocalZ +=" in main_cpp
        and "kRequiredStableSeconds" not in main_cpp
        and "kMaximumCalibrationVelocity" not in main_cpp
        and "boundedDeltaSeconds" in main_cpp
        and "const float currentProjectedRadius ="
        in geometry_magnify_shader
        and "const float2 stableShadowCoordinates ="
        in geometry_magnify_shader
        and "float2 eyeTravelLens ="
        in geometry_magnify_shader
        and "physicalEyeBoxReady" in hooking_h
        and "projectedPhysicalEyeBoxReady" in hooking,
        "physical eye-box telemetry is not a continuous screen-space follower",
    )
    require(
        "projectedLensSequence.fetch_add" in hooking
        and "projectedLensSequence.load" in hooking
        and "sequenceBefore & 1U" in hooking
        and "sequenceBefore == sequenceAfter" in hooking,
        "lens and physical eye-box telemetry is not published as one coherent snapshot",
    )
    require(
        re.search(
            r"const float magnification\s*=\s*lerp\(\s*"
            r"1\.0f,\s*clamp\(SCOPE_FADE_MAGNIFICATION.*?"
            r"activation\s*\)",
            geometry_magnify_code,
            re.DOTALL,
        )
        and "SCOPE_FISHEYE_STRENGTH" in geometry_magnify_code
        and "SCOPE_EDGE_REFRACTION_STRENGTH" in geometry_magnify_code
        and "saturate(SCOPE_PHYSICAL_EYEBOX_VALID)" in geometry_magnify_code
        and "clamp(SCOPE_EYEBOX_MAX_TRAVEL" in geometry_magnify_code
        and "float2(SCOPE_EYE_OFFSET_X, SCOPE_EYE_OFFSET_Y)"
        in geometry_magnify_code
        and "const float currentProjectedRadius =" in geometry_magnify_code
        and "const float2 eyeParallaxPixels ="
        in geometry_magnify_code
        and "sceneTravel * currentProjectedRadius" in geometry_magnify_code
        and "const float2 stableShadowCoordinates ="
        in geometry_magnify_code
        and "float2 eyeTravelLens =" in geometry_magnify_code
        and "float2(SCOPE_AIM_OFFSET_X, SCOPE_AIM_OFFSET_Y)"
        in geometry_magnify_code
        and not re.search(
            r"float2\(SCOPE_EYE_OFFSET_X,\s*SCOPE_EYE_OFFSET_Y\)\s*"
            r"\*\s*SCOPE_VIGNETTE_SHARPNESS",
            geometry_magnify_code,
        ),
        "scope optics do not fade smoothly or physical eye travel is coupled to vignette sharpness",
    )
    require(
        not re.search(
            r'scopeNode\s*=\s*firstPersonRoot->GetObjectByName\(\s*"ReticleNode"',
            code,
        ),
        "automatic STS projection returned to the generic ReticleNode pivot",
    )
    require(
        "RE::HUDMenuUtils::WorldPtToScreenPt3" not in code,
        "worldspace HUD projection returned to the first-person scope path",
    )
    require(
        not re.search(r"REL::Offset\s*\(\s*0x0*A37940", code, re.IGNORECASE),
        "invalid A37940 callable relocation returned to source",
    )
    require(
        not re.search(r"(?:ComPtr\s*<\s*)?IDXGISwapChain3\s*\*", code),
        "unsafe IDXGISwapChain3 proxy call path returned to source",
    )
    require(
        not re.search(r"ConcreteFormFactory\s*<[^>]*BGSZoomData", code),
        "synthetic BGSZoomData allocation returned to source",
    )
    require(
        "BSAutoLock lock{ middleHigh->equippedItemsLock }" in code,
        "equipped weapon snapshot is not protected by equippedItemsLock",
    )
    require(
        "originalZoomForm->zoomData = originalZoomData" not in code
        and not re.search(
            r"Imgui_InstanceData->zoomData->zoomData\s*=\s*currOriZoomData",
            code,
        ),
        "zoom lifecycle overwrites overlay or image-space fields it does not own",
    )
    require(
        "originalZoomForm->zoomData.fovMult = originalZoomData.fovMult" in code
        and "originalZoomForm->zoomData.cameraOffset" in code
        and "ClearIsolatedZoomSession();" in code
        and "DetachIsolatedZoomForSave();" in code
        and "ReattachIsolatedZoomAfterSave();" in code,
        "reversible selected-weapon zoom lifecycle is incomplete",
    )
    require(
        "profile->zoomDataOverwrite.enableZoomDateOverwrite = true" in code
        and "profile->zoomDataOverwrite.fovMul = zoomData.fovMult" in code,
        "automatic STS profiles do not begin from authored ZoomData",
    )
    require(
        "reinterpret_cast<std::uintptr_t>(controller.get()) + 0x470" not in code,
        "unverified Havok character-controller offset returned to source",
    )
    require(
        re.search(r"\[Diagnostics\].*?VerificationStage\s*=\s*4", ini, re.DOTALL)
        and re.search(r"\[Diagnostics\].*?TAACapture\s*=\s*1", ini, re.DOTALL)
        and re.search(r"\[Diagnostics\].*?GeometryProbe\s*=\s*1", ini, re.DOTALL)
        and re.search(r"\[Diagnostics\].*?GeometryMagnification\s*=\s*1", ini, re.DOTALL)
        and re.search(r"\[Diagnostics\].*?AuxiliaryWorldPass\s*=\s*0", ini, re.DOTALL),
        "packaged INI must enable the verified Stage 4 production pipeline only",
    )
    for contract in (
        "AllowsProjection()",
        "AllowsOverrides()",
        "AllowsCameraOverrides()",
        "AllowsRenderer()",
        "AllowsComposite()",
        "AllowsTAACapture()",
        "AllowsVisualProbe()",
        "AllowsGeometryProbe()",
        "AllowsGeometryMagnification()",
        "AllowsScopeFadeGeometry()",
        "AllowsPrivateRenderHooks()",
        "AllowsAuxiliaryRuntimePreflight()",
        "AllowsAuxiliaryPassThroughHooks()",
        "AllowsAuxiliaryObservationHooks()",
        "AllowsAuxiliaryWorldPass()",
    ):
        require(contract in settings, f"missing rollout gate: {contract}")
    taa_gate_start = settings.find(
        "[[nodiscard]] bool AllowsTAACapture()"
    )
    taa_gate_end = settings.find(
        "[[nodiscard]] bool AllowsAuxiliaryRuntimePreflight()",
        taa_gate_start,
    )
    taa_gate_body = settings[taa_gate_start:taa_gate_end]
    require(
        taa_gate_start >= 0
        and taa_gate_end > taa_gate_start
        and "verificationStage == 4" in taa_gate_body
        and "verificationStage == 3 && verificationTaaCapture"
        in taa_gate_body
        and "AllowsAuxiliaryWorldPass()" not in taa_gate_body,
        "functional Stage 5c must not install the TAA vtable hook; its pre-first-person source is carried through ordinary TAA",
    )
    require(
        "verificationStage = std::clamp<std::uint32_t>(" in main_cpp
        and "\n\t\t\t5U);" in main_cpp
        and re.search(
            r"AllowsAuxiliaryRuntimePreflight\(\).*?"
            r"return\s+verificationStage\s*==\s*5\s*&&\s*"
            r"!verificationAuxiliaryPassThroughHooks\s*&&\s*"
            r"!verificationAuxiliaryObservationHooks\s*&&\s*"
            r"!verificationAuxiliaryWorldPass\s*;",
            settings,
            re.DOTALL,
        )
        and re.search(
            r"AllowsAuxiliaryPassThroughHooks\(\).*?"
            r"return\s+verificationStage\s*==\s*5\s*&&\s*"
            r"verificationAuxiliaryPassThroughHooks\s*&&\s*"
            r"!verificationAuxiliaryObservationHooks\s*&&\s*"
            r"!verificationAuxiliaryWorldPass\s*;",
            settings,
            re.DOTALL,
        )
        and re.search(
            r"AllowsAuxiliaryObservationHooks\(\).*?"
            r"return\s+verificationStage\s*==\s*5\s*&&\s*"
            r"!verificationAuxiliaryPassThroughHooks\s*&&\s*"
            r"verificationAuxiliaryObservationHooks\s*&&\s*"
            r"!verificationAuxiliaryWorldPass\s*;",
            settings,
            re.DOTALL,
        )
        and re.search(
            r"AllowsAuxiliaryWorldPass\(\).*?return\s+false\s*;",
            settings,
            re.DOTALL,
        ),
        "Stage 5 diagnostic gates are not bounded or the retired auxiliary world pass can still activate",
    )
    stage5_world_pass = main_cpp.find(
        "if (settings.AllowsAuxiliaryWorldPass())"
    )
    stage5_observation = main_cpp.find(
        "if (settings.AllowsAuxiliaryObservationHooks())"
    )
    stage5_pass_through = main_cpp.find(
        "if (settings.AllowsAuxiliaryPassThroughHooks())"
    )
    stage5_preflight = main_cpp.find(
        "if (settings.AllowsAuxiliaryRuntimePreflight())"
    )
    stage5_observation_body = main_cpp[
        stage5_observation:stage5_pass_through
    ]
    stage5_world_pass_body = main_cpp[
        stage5_world_pass:stage5_observation
    ]
    normal_plugin_init = main_cpp.find(
        "hookIns = Hook::D3D::GetSington();",
        stage5_preflight,
    )
    require(
        stage5_world_pass >= 0
        and stage5_observation > stage5_world_pass
        and stage5_pass_through > stage5_observation
        and stage5_preflight > stage5_pass_through
        and normal_plugin_init > stage5_preflight
        and "InstallHooks()" in stage5_world_pass_body
        and "if (!installed)" in stage5_world_pass_body
        and "return true;" in stage5_world_pass_body
        and "failed closed" in stage5_world_pass_body
        and "RequestFrame(" not in stage5_world_pass_body
        and "InstallObservationHooks()" in main_cpp[
            stage5_observation:stage5_pass_through
        ]
        and "return true;" in stage5_observation_body
        and "RequestFrame(" not in stage5_observation_body
        and "AcquireColorSRV(" not in stage5_observation_body
        and "InitializePlugin(" not in stage5_observation_body
        and "RegisterFuncs(" not in stage5_observation_body
        and "hookIns" not in stage5_observation_body
        and "sdh" not in stage5_observation_body
        and "only forward original calls and record bounded"
        in stage5_observation_body
        and "no auxiliary render request, private renderer resource"
        in stage5_observation_body
        and "visual, menu, Papyrus function, profile update"
        in stage5_observation_body
        and "InstallPassThroughHooks()" in main_cpp[
            stage5_pass_through:stage5_preflight
        ]
        and "return true;" in main_cpp[
            stage5_pass_through:stage5_preflight
        ]
        and "four detours only forward original calls"
        in main_cpp[stage5_pass_through:stage5_preflight]
        and "ProbeRuntimeTargets()" in main_cpp[
            stage5_preflight:normal_plugin_init
        ]
        and "return true;" in main_cpp[
            stage5_preflight:normal_plugin_init
        ]
        and "No MagnaScope hooks, menus, Papyrus functions, profile updates"
        in main_cpp[stage5_preflight:normal_plugin_init],
        "isolated Stage 5 startup does not return before normal MagnaScope initialization",
    )
    request_world_source = main_cpp.find(
        "const bool requestWorldOnlySource ="
    )
    request_world_call = main_cpp.find(
        "WorldOnlyScopeRenderer::GetSingleton().RequestFrame(",
        request_world_source,
    )
    request_world_end = main_cpp.find(";", request_world_call)
    request_world_body = main_cpp[
        request_world_source:request_world_end + 1
    ]
    require(
        request_world_source >= 0
        and request_world_call > request_world_source
        and request_world_end > request_world_call
        and "settings.AllowsAuxiliaryWorldPass()"
        in request_world_body
        and "InGameFlag" in request_world_body
        and "player" in request_world_body
        and "currentData" in request_world_body
        and "currentData->autoProfile" in request_world_body
        and "IsInADS(player)" in request_world_body
        and "RequestFrame(\n\t\trequestWorldOnlySource)"
        in request_world_body
        and main_cpp.count("RequestFrame(") == 1,
        "functional Stage 5c requests a source outside active automatic STS ADS",
    )
    invalid_stage5 = main_cpp.find(
        "if (settings.verificationStage == 5 &&",
        stage5_preflight,
    )
    require(
        invalid_stage5 > stage5_preflight
        and invalid_stage5 < normal_plugin_init
        and "!settings.AllowsAuxiliaryWorldPass()"
        in main_cpp[invalid_stage5:normal_plugin_init]
        and "return true;" in main_cpp[invalid_stage5:normal_plugin_init]
        and "failed closed" in main_cpp[invalid_stage5:normal_plugin_init],
        "ambiguous Stage 5 sub-gates can fall through to normal plugin initialization",
    )
    require(
        re.search(
            r"\[Diagnostics\].*?AuxiliaryPassThroughHooks\s*=\s*0",
            ini,
            re.DOTALL,
        ),
        "packaged INI must keep the Stage 5b pass-through hook sub-gate disabled",
    )
    require(
        re.search(
            r"\[Diagnostics\].*?AuxiliaryObservationHooks\s*=\s*0",
            ini,
            re.DOTALL,
        ),
        "packaged INI must keep Stage 5c-a observation hooks disabled",
    )
    require(
        re.search(
            r"\[Diagnostics\].*?AuxiliaryWorldPass\s*=\s*0",
            ini,
            re.DOTALL,
        ),
        "packaged INI must keep the functional Stage 5c world pass disabled",
    )
    require(
        re.search(
            r"\[Diagnostics\].*?VerificationStage\s*=\s*5",
            stage5c_observation_ini,
            re.DOTALL,
        )
        and re.search(
            r"\[Diagnostics\].*?AuxiliaryPassThroughHooks\s*=\s*0",
            stage5c_observation_ini,
            re.DOTALL,
        )
        and re.search(
            r"\[Diagnostics\].*?AuxiliaryObservationHooks\s*=\s*1",
            stage5c_observation_ini,
            re.DOTALL,
        )
        and re.search(
            r"\[Diagnostics\].*?CameraOverride\s*=\s*0",
            stage5c_observation_ini,
            re.DOTALL,
        )
        and re.search(
            r"\[Diagnostics\].*?TAACapture\s*=\s*0",
            stage5c_observation_ini,
            re.DOTALL,
        )
        and re.search(
            r"\[Diagnostics\].*?VisualProbe\s*=\s*0",
            stage5c_observation_ini,
            re.DOTALL,
        )
        and re.search(
            r"\[Diagnostics\].*?GeometryProbe\s*=\s*0",
            stage5c_observation_ini,
            re.DOTALL,
        )
        and re.search(
            r"\[Diagnostics\].*?GeometryMagnification\s*=\s*0",
            stage5c_observation_ini,
            re.DOTALL,
        )
        and re.search(
            r"\[AutoSTS\].*?Enabled\s*=\s*0",
            stage5c_observation_ini,
            re.DOTALL,
        ),
        "Stage 5c-a fixture enables a visual, profile, override, or earlier rollout path",
    )
    require(
        re.search(
            r"\[Diagnostics\].*?VerificationStage\s*=\s*5",
            stage5c_world_pass_ini,
            re.DOTALL,
        )
        and re.search(
            r"\[Diagnostics\].*?AuxiliaryPassThroughHooks\s*=\s*0",
            stage5c_world_pass_ini,
            re.DOTALL,
        )
        and re.search(
            r"\[Diagnostics\].*?AuxiliaryObservationHooks\s*=\s*0",
            stage5c_world_pass_ini,
            re.DOTALL,
        )
        and re.search(
            r"\[Diagnostics\].*?AuxiliaryWorldPass\s*=\s*1",
            stage5c_world_pass_ini,
            re.DOTALL,
        )
        and re.search(
            r"\[Diagnostics\].*?CameraOverride\s*=\s*0",
            stage5c_world_pass_ini,
            re.DOTALL,
        )
        and re.search(
            r"\[Diagnostics\].*?TAACapture\s*=\s*0",
            stage5c_world_pass_ini,
            re.DOTALL,
        )
        and re.search(
            r"\[Diagnostics\].*?VisualProbe\s*=\s*0",
            stage5c_world_pass_ini,
            re.DOTALL,
        )
        and re.search(
            r"\[Diagnostics\].*?GeometryProbe\s*=\s*0",
            stage5c_world_pass_ini,
            re.DOTALL,
        )
        and re.search(
            r"\[Diagnostics\].*?GeometryMagnification\s*=\s*0",
            stage5c_world_pass_ini,
            re.DOTALL,
        )
        and re.search(
            r"\[AutoSTS\].*?Enabled\s*=\s*1",
            stage5c_world_pass_ini,
            re.DOTALL,
        ),
        "functional Stage 5c fixture is not isolated to the world-only automatic STS path",
    )
    require(
        re.search(r"\[Diagnostics\].*?CameraOverride\s*=\s*0", ini, re.DOTALL),
        "packaged INI must keep the Stage 2 camera sub-gate disabled",
    )
    require(
        re.search(r"\[Diagnostics\].*?TAACapture\s*=\s*0", ini, re.DOTALL),
        "packaged INI must keep the Stage 3 TAA sub-gate disabled",
    )
    require(
        re.search(r"\[Diagnostics\].*?VisualProbe\s*=\s*0", ini, re.DOTALL),
        "packaged INI must keep the Stage 4a HUD probe disabled",
    )
    require(
        re.search(r"\[Diagnostics\].*?GeometryProbe\s*=\s*0", ini, re.DOTALL),
        "packaged INI must keep the Stage 4b geometry probe disabled",
    )
    require(
        re.search(
            r"\[Diagnostics\].*?GeometryMagnification\s*=\s*0",
            ini,
            re.DOTALL,
        ),
        "packaged INI must keep Stage 4e.2 magnification disabled",
    )
    require(
        re.search(
            r"if\s*\(\s*settings\.AllowsPrivateRenderHooks\(\)\s*\)\s*\{.*?"
            r"CreateThread\s*\(",
            main_cpp,
            re.DOTALL,
        ),
        "Stage 3 must not install MagnaScope-owned DX11 hooks",
    )
    require(
        "Verification stage 3 {} source capture completed" in hooking
        and "no shaders, RTVs, or draws" in hooking,
        "Stage 3 read-only source-capture contract is missing",
    )
    require(
        "AddHudElement(RenderScopeVisualProbe)" in imgui
        and "AllowsVisualProbe()" in imgui
        and "ImDrawListManager::AddCircleFilled" in imgui
        and "no MagnaScope shaders or render-target binds" in imgui,
        "Stage 4a does not use the framework-owned HUD aperture probe",
    )
    require(
        "PublishAutomaticSTSGeometry(" in main_cpp
        and "aperture.renderSurface" in main_cpp
        and "aperture.aimReference" in main_cpp
        and "aperture.extentReference" in main_cpp
        and 'GetObjectByName("ScopeFade:0")' in main_cpp
        and "scopeFade->numTriangles != 48U" in main_cpp
        and "scopeFade->numVertices != 48U" in main_cpp
        and "RE::NiAVObject* renderSurface = scopeFade" in main_cpp
        and "IsDescendantOf(scopeViewParts, scopeAiming)" in main_cpp
        and "automaticSTSGeometryReady" in hooking
        and "currentVertexBuffer.Get()" in hooking
        and "currentIndexBuffer.Get()" in hooking
        and "exactGeometryMatch" in hooking
        and "automaticSTSVertexDataOffset" in hooking
        and "automaticSTSIndexDataOffset" in hooking
        and "exactSuballocationMatch" in hooking
        and "ScopeFade buffer candidate rejected" in hooking
        and "m_pGeometryShader_STSGeometryFill" in hooking
        and "GSGetShader" in hooking
        and "GSSetShader" in hooking
        and "m_pPixelShader_STSGeometryProbe" in hooking
        and "m_pPixelShader_STSGeometryMagnify" in hooking
        and "2.0f * input[1].position - input[0].position"
        in geometry_fill_shader
        and "nointerpolation float4 centerClip : TEXCOORD7"
        in geometry_fill_shader
        and "const bool oddPrimitive = (primitiveID & 1U) != 0U"
        in geometry_fill_shader
        and "[maxvertexcount(6)]" in geometry_fill_shader
        and "float4(0.0f, 0.82f, 1.0f, 1.0f)" in geometry_probe_shader,
        "Stage 4d does not identify and safely fill the exact ScopeFade draw",
    )
    require(
        "DrawIndexedInstancedHook" in hooking
        and "HookInfo{ 20" in hooking
        and "automaticSTSHousingGeometryReady" in hooking
        and "Stage 4d.2d draw telemetry" in hooking
        and "phookD3D11DrawIndexedInstanced(" in hooking,
        "Stage 4d.2d does not safely observe the alternate indexed-instanced path and aiming housing",
    )
    render_texture_start = hooking.find("bool D3D::RenderToReticleTexture()")
    render_texture_end = hooking.find(
        "void D3D::RenderToReticleTextureNew",
        render_texture_start,
    )
    draw_indexed_start = hooking.find(
        "void __stdcall D3D::DrawIndexedHook("
    )
    draw_instanced_start = hooking.find(
        "void __stdcall D3D::DrawIndexedInstancedHook("
    )
    draw_instanced_end = hooking.find(
        "bool D3D::CaptureVerificationSource(",
        draw_instanced_start,
    )
    require(
        render_texture_start >= 0
        and render_texture_end > render_texture_start
        and draw_indexed_start >= 0
        and draw_instanced_start > draw_indexed_start
        and draw_instanced_end > draw_instanced_start
        and "automaticSTSExactScopeFadeReplacementThisFrame.load"
        in hooking[render_texture_start:render_texture_end]
        and "automaticSTSExactScopeFadeReplacementThisFrame.store"
        in hooking[draw_indexed_start:draw_instanced_start]
        and "automaticSTSExactScopeFadeReplacementThisFrame.store"
        not in hooking[draw_instanced_start:draw_instanced_end]
        and "lastRenderProducedComposite =\n"
        "\t\t\t\t\t\tD3DInstance->RenderToReticleTexture();"
        in hooking
        and "automaticSTSExactScopeFadeReplacementThisFrame"
        in hooking_h,
        "Automatic STS can suppress its fullscreen fail-safe or report a composite without an exact DrawIndexed ScopeFade replacement",
    )
    projection_reset_start = main_cpp.find(
        "void ResetAutomaticSTSProjectionTracking()"
    )
    selection_reset_start = main_cpp.find(
        "void InvalidateAutomaticSTSSelection()"
    )
    projection_reset_body = main_cpp[
        projection_reset_start:selection_reset_start
    ]
    require(
        projection_reset_start >= 0
        and selection_reset_start > projection_reset_start
        and "InvalidateLensProjection()" in projection_reset_body
        and "InvalidateAutomaticSTSGeometry()" not in projection_reset_body
        and "const bool automaticADS" in main_cpp
        and "else if (automaticSTSTracking.aperture != scopeNode)"
        in main_cpp
        and "Preserve the last publication and monotonic activation"
        in main_cpp
        and "InvalidateAutomaticSTSSelection();" in main_cpp,
        "transient recoil projection loss does not preserve activation while selection reset remains available",
    )
    require(
        "CreateTriShape(" not in main_cpp
        and "BSShaderResourceManager::GetSingleton()" not in main_cpp
        and "NiCloningProcess" not in main_cpp,
        "automatic STS must not construct renderer geometry through the unsafe OG resource-manager interface",
    )
    geometry_probe_branch = hooking.find(
        "if (verification.AllowsScopeFadeGeometry()"
    )
    geometry_probe_return = hooking.find(
        "// STS owns its 3D reticle.",
        geometry_probe_branch,
    )
    require(
        geometry_probe_branch >= 0
        and geometry_probe_return > geometry_probe_branch
        and "PSSetShader" in hooking[geometry_probe_branch:geometry_probe_return]
        and "GSSetShader" in hooking[geometry_probe_branch:geometry_probe_return]
        and "GSGetShader" in hooking[geometry_probe_branch:geometry_probe_return]
        and "OMSetRenderTargets" not in hooking[
            geometry_probe_branch:geometry_probe_return
        ]
        and "OMSetDepthStencilState" not in hooking[
            geometry_probe_branch:geometry_probe_return
        ]
        and "IASetVertexBuffers" not in hooking[
            geometry_probe_branch:geometry_probe_return
        ],
        "Stage 4d replacement changes render targets, depth state, or input-assembly geometry",
    )
    require(
        "isEnableRender.load" not in hooking[
            geometry_probe_branch:geometry_probe_return
        ]
        and "actual draw is therefore the authoritative visibility signal"
        in hooking,
        "Stage 4d still relies on transient gunState instead of ScopeAiming draw visibility",
    )
    prepare_scope_source_start = hooking.find(
        "bool D3D::PrepareScopeFadeSceneSource("
    )
    prepare_scope_source_end = hooking.find(
        "void D3D::CreateBlender()",
        prepare_scope_source_start,
    )
    prepare_scope_source = hooking[
        prepare_scope_source_start:prepare_scope_source_end
    ]
    require(
        "PrepareScopeFadeSceneSource(" in hooking
        and "context->GetDevice(" in hooking
        and "EnsureGeometryProbeDeviceResources(contextDevice.Get())"
        in hooking
        and "OMGetRenderTargets(" in hooking[
            geometry_probe_branch:geometry_probe_return
        ]
        and prepare_scope_source_start >= 0
        and prepare_scope_source_end > prepare_scope_source_start
        and "sourceTexture->GetDevice(" not in prepare_scope_source
        and "contextDevice->CreateTexture2D(" in hooking
        and "contextDevice->CreateShaderResourceView(" in hooking
        and "mScopeFadeResourceDevice" in hooking_h
        and "mScopeFadeResolutionBuffer" in hooking_h
        and "mScopeFadeSampler" in hooking_h
        and "mScopeFadeGeometryMutex" in hooking_h
        and "std::scoped_lock geometryLock(" in hooking[
            geometry_probe_branch:geometry_probe_return
        ]
        and "mScopeFadeSceneTexture" in hooking
        and "mScopeFadeSceneSRV" in hooking
        and "copyDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE"
        in hooking
        and "context->CopyResource(" in hooking
        and "mScopeFadeSceneTexture.Get()," in hooking
        and "sourceTexture.Get()" in hooking
        and "PSGetShaderResources(" in hooking
        and "PSGetSamplers(" in hooking
        and "PSGetConstantBuffers(" in hooking
        and "PSSetShaderResources(" in hooking
        and "PSSetSamplers(" in hooking
        and "PSSetConstantBuffers(" in hooking
        and "return oldFuncs.phookD3D11DrawIndexed" in hooking[
            geometry_probe_branch:geometry_probe_return
        ]
        and "input.position.xy * PixelSize" in geometry_magnify_shader
        and "noperspective float3 lensProjective : TEXCOORD0"
        in geometry_magnify_shader
        and "input.lensProjective.xy / safeReciprocalClipW"
        in geometry_magnify_shader
        and "const float2 numeratorDx = ddx(projectiveNumerator)"
        in geometry_magnify_shader
        and "const float2 numeratorDy = ddy(projectiveNumerator)"
        in geometry_magnify_shader
        and "const float reciprocalWDx = ddx(reciprocalClipW)"
        in geometry_magnify_shader
        and "const float reciprocalWDy = ddy(reciprocalClipW)"
        in geometry_magnify_shader
        and "SolvePixelOffset" in geometry_magnify_shader
        and "const float2 pixelsToCenter" in geometry_magnify_shader
        and "tBACKBUFFER.SampleLevel" in geometry_magnify_shader
        and "SCOPE_FADE_MAGNIFICATION" in geometry_magnify_shader
        and "const float2 currentAimPixels ="
        in geometry_magnify_shader
        and "const float2 samplePivotUv = currentAimPixels * PixelSize"
        in geometry_magnify_shader
        and "const bool exactDrawFrameValid"
        in geometry_magnify_shader
        and "if (!exactDrawFrameValid)"
        in geometry_magnify_shader
        and "publishedCenterPixels" not in geometry_magnify_shader
        and "(screenUv - samplePivotUv) / opticalMagnification"
        in geometry_magnify_shader
        and "projection.aimCenterX - projection.centerX"
        in prepare_scope_source
        and "projection.aimCenterY - projection.centerY"
        in prepare_scope_source
        # MagnaScope owns this buffer's creation, so the Lens Center/Lens Size
        # row was appended rather than stolen from an existing field. The size
        # is still pinned: the shader-side layout and both WARP harness mirrors
        # have to move with it or the wrong floats reach the optics.
        and "sizeof(ConstBufferData) == 176" in hooking_h
        and "SCOPE_AIM_OFFSET_VALID" in triangle_shader
        and "SCOPE_IMAGE_DENOISE" in triangle_shader
        and "SCOPE_IMAGE_SHARPEN" in triangle_shader
        and "SCOPE_PHYSICAL_EYEBOX_VALID" in triangle_shader
        and "SCOPE_LENS_BASIS_XX" in triangle_shader
        and "normalizedLensPosition - pupilCenter"
        in geometry_magnify_shader
        and "scopeFadeMagnification.load" in prepare_scope_source,
        "Stage 4e sampling does not preserve the exact ScopeFade geometry with a reticle-aligned private source",
    )
    draw_indexed_body = hooking[
        draw_indexed_start:draw_instanced_start
    ]
    draw_instanced_body = hooking[
        draw_instanced_start:draw_instanced_end
    ]
    require(
        "CaptureBeforeFirstPersonDraw(pContext)" not in draw_indexed_body
        and "CaptureBeforeFirstPersonDraw(pContext)"
        not in draw_instanced_body
        and "CaptureAutomaticSTSScopeFadeReplay(" in draw_indexed_body
        and "return oldFuncs.phookD3D11DrawIndexed(" in draw_indexed_body
        and "ReplayAutomaticSTSScopeFade(" in hooking[
            render_texture_start:render_texture_end
        ]
        and "mShaderResourceView.Get()" in hooking[
            render_texture_start:render_texture_end
        ]
        and re.search(
            r"\[Diagnostics\].*?"
            r"VerificationStage\s*=\s*4.*?"
            r"AuxiliaryWorldPass\s*=\s*0.*?"
            r"TAACapture\s*=\s*1.*?"
            r"GeometryProbe\s*=\s*1.*?"
            r"GeometryMagnification\s*=\s*1",
            stage5d_late_replay_ini,
            re.DOTALL,
        ),
        "Stage 5d does not retire pre-first-person color capture and replay exact ScopeFade geometry against the coherent late source",
    )
    auxiliary_source_gate = hooking.find(
        ".AllowsAuxiliaryWorldPass()) {",
        geometry_probe_branch,
    )
    acquire_world_source = hooking.find(
        "AcquireColorSRV(token)",
        auxiliary_source_gate,
    )
    missing_world_source = hooking.find(
        "if (!worldOnlySceneResource.Get())",
        acquire_world_source,
    )
    prepare_world_source = hooking.find(
        "PrepareScopeFadeSceneSource(",
        missing_world_source,
    )
    missing_world_source_body = hooking[
        missing_world_source:prepare_world_source
    ]
    require(
        auxiliary_source_gate >= 0
        and acquire_world_source > auxiliary_source_gate
        and missing_world_source > acquire_world_source
        and prepare_world_source > missing_world_source
        and "A missing current-generation source is not"
        in missing_world_source_body
        and "permission to reuse the primary frame"
        in missing_world_source_body
        and "return oldFuncs.phookD3D11DrawIndexed("
        in missing_world_source_body
        and "mScopeFadeSceneSRV" not in missing_world_source_body
        and "worldOnlySceneResource.Get()))"
        in hooking[prepare_world_source:prepare_world_source + 320]
        and "worldOnlySceneResource.Get() ?" in hooking[
            prepare_world_source:geometry_probe_return
        ],
        "functional Stage 5c can reuse the primary frame when its current-generation source acquisition fails",
    )
    require(
        "ImageDenoise" in profile_data
        and "ImageSharpen" in profile_data
        and "ReticleMagnificationInfluence" in profile_data
        and "imageDenoise" in profile_data_h
        and "imageSharpen" in profile_data_h
        and "reticleMagnificationInfluence" in profile_data_h
        and "edgeAwareAverage" in geometry_magnify_shader
        and "localMinimum" in geometry_magnify_shader
        and "localMaximum" in geometry_magnify_shader,
        "Aperture cleanup or reticle influence is not profile-backed and bounded",
    )
    require(
        "BeginAutomaticSTSReticleLayerCapture(" in hooking
        and "CompleteAutomaticSTSReticleLayerCapture(" in hooking
        and "CompositeAutomaticSTSReticleLayer(" in hooking
        and "layerDescription.Format = sourceDescription.Format;" in hooking
        and "automaticSTSExactScopeFadeReplacementThisFrame.load(" in hooking
        and "m_pPixelShader_STSReticleLayer.Get()" in hooking
        and "PSSetConstantBuffers(4U, 1U, &resolutionBuffer)" in hooking
        and "SCOPE_RETICLE_MAGNIFICATION" in reticle_layer_shader
        and "SCOPE_RETICLE_SIZE" in reticle_layer_shader
        and "SCOPE_RETICLE_OFFSET_X" in reticle_layer_shader
        and "reticleOutputPivot" in reticle_layer_shader
        and "reticleVisibility" in reticle_layer_shader
        and "reticle.sourceContribution *= reticleVisibility"
        in reticle_layer_shader
        and "reticle.destinationTransmittance = lerp("
        in reticle_layer_shader
        and "float4(1.0f, 1.0f, 1.0f, 1.0f)"
        in reticle_layer_shader
        and "const float projectedRadius = max(" in reticle_layer_shader
        and "const float2 shadowLensCoordinates = lensCoordinates;"
        in reticle_layer_shader
        and "EvaluateScopeShadow(" in reticle_layer_shader
        and "const bool physicalEyeTravelValid =" in reticle_layer_shader
        and "if (physicalEyeTravelValid)" in reticle_layer_shader
        and "if (projectedBasisValid && SCOPE_PHYSICAL_EYEBOX_VALID"
        not in reticle_layer_shader
        and "ScopeFadePolygonRadius" not in reticle_layer_shader
        and "reticleBoundary" not in reticle_layer_shader
        and "SCOPE_FADE_MAGNIFICATION" not in reticle_layer_shader
        and hooking.count("DrawReticleWithScaledVertices(") == 1,
        "Reticle is not isolated into a non-destructive post-optics layer with stable screen-space shadowing",
    )
    require(
        "PublishEditorPreview(" in imgui
        and "GetEditorPreviewSnapshot()" in main_cpp
        and "ApplySelectedEditorPreview(" in main_cpp
        and "RequestProfileSave(editedProfile)" in imgui
        and "ConsumeProfileSave()" in main_cpp
        and "IsSameProfileIdentity(" in main_cpp
        and "Imgui_InstanceData->zoomData" not in imgui
        and "\n\t\t\tInitCurrentScopeData();" not in imgui
        and "std::atomic_bool bEnableEditMode" in hooking_h
        and "std::atomic_bool bRefreshChar" in hooking_h,
        "Menu Framework editor still mutates game objects or reselects profiles from the renderer thread",
    )
    require(
        "ApplyForcedAimImmediately" not in imgui
        and "SetInIronSightsImpl" not in imgui
        and "RE::PlayerCharacter::GetSingleton()" not in imgui
        and "captureMouseOutsidePanel" in imgui
        and "frameworkMenuOpen" in imgui
        and "BlockUserInput.store(" in imgui
        and imgui.find("BlockUserInput.store(")
        < imgui.find("scopeEditorWindow->IsOpen.store(false)"),
        "The popout capture lifecycle touches game objects or closes before releasing framework input",
    )
    stage3_branch = hooking.find("if (!verification.AllowsComposite())")
    stage4_init = hooking.find("if (bIsFirst)", stage3_branch)
    require(
        stage3_branch >= 0
        and stage4_init > stage3_branch
        and "InitEffect()" not in hooking[stage3_branch:stage4_init]
        and "UpdateScene(" not in hooking[stage3_branch:stage4_init]
        and "RenderToReticleTexture" not in hooking[stage3_branch:stage4_init]
        and "OMSetRenderTargets" not in hooking[stage3_branch:stage4_init],
        "Stage 3a performs shader initialization, constant updates, draws, or target binding",
    )
    require(
        "if (!REX::FModule::IsRuntimeOG())" in hooking
        and "targetOwner != falloutModule" in hooking
        and "TAA vtable slot 1 is already owned by another module" in hooking
        and "InstallVerificationTAAHook()" in main_cpp,
        "Stage 3b TAA hook is not guarded by runtime and module ownership",
    )
    require(
        "static std::atomic_bool isEnableRender" in hooking_h
        and "CaptureVerificationTAASource()" in hooking
        and "No camera, weapon, scene graph, or profile object is touched here" in hooking
        and "taaVerificationCapturedSinceFramework.exchange" in hooking,
        "Stage 3b does not use an atomic, capture-only render-thread handoff",
    )
    taa_capture = hooking.find("bool D3D::CaptureVerificationTAASource()")
    taa_capture_end = hooking.find("void D3D::Render()", taa_capture)
    require(
        taa_capture >= 0
        and taa_capture_end > taa_capture
        and "GetCurrentScopeProfile" not in hooking[taa_capture:taa_capture_end]
        and "pcam" not in hooking[taa_capture:taa_capture_end]
        and "player" not in hooking[taa_capture:taa_capture_end]
        and "OMSetRenderTargets" not in hooking[taa_capture:taa_capture_end],
        "Stage 3b TAA capture reads live game objects or mutates render targets",
    )
    publish_original = hooking.find(
        "HookedRender_TAA::func =",
        hooking.find("bool InstallGuardedTAAHook()"),
    )
    publish_thunk = hooking.find(
        "const auto replacedRender = taaVtable.write_vfunc(",
        hooking.find("bool InstallGuardedTAAHook()"),
    )
    require(
        publish_original >= 0
        and publish_thunk > publish_original
        and "installedRender !=" in hooking[publish_thunk:],
        "TAA callback publication can race a null original or is not verified",
    )

    require(
        "enum class Phase : std::uint8_t" in world_only_renderer_header
        and "kPrimaryEligible" in world_only_renderer_header
        and "GetPhaseToken() const noexcept" in world_only_renderer_header
        and "AcquireColorSRV(" in world_only_renderer_header
        and "PhaseToken token" in world_only_renderer_header
        and "state.generation.load(std::memory_order_acquire) !="
        in world_only_renderer,
        "World-only output is not restricted to the exact current primary generation",
    )
    probe_start = world_only_renderer.find(
        "bool WorldOnlyScopeRenderer::ProbeRuntimeTargets()"
    )
    install_start = world_only_renderer.find(
        "bool WorldOnlyScopeRenderer::InstallHooks()"
    )
    forwarding_install_start = world_only_renderer.find(
        "bool WorldOnlyScopeRenderer::InstallForwardingHooks("
    )
    pass_through_install_start = world_only_renderer.find(
        "bool WorldOnlyScopeRenderer::InstallPassThroughHooks()"
    )
    request_start = world_only_renderer.find(
        "void WorldOnlyScopeRenderer::RequestFrame(",
        install_start,
    )
    probe_body = world_only_renderer[probe_start:install_start]
    install_body = world_only_renderer[install_start:request_start]
    forwarding_install_body = world_only_renderer[
        forwarding_install_start:install_start
    ]
    pass_through_install_body = world_only_renderer[
        pass_through_install_start:install_start
    ]
    require(
        "[[nodiscard]] bool ProbeRuntimeTargets();" in world_only_renderer_header
        and probe_start >= 0
        and install_start > probe_start
        and request_start > install_start
        and "REL::Version supportedRuntime{ 1, 10, 163, 0 }"
        in probe_body
        and "!REX::FModule::IsRuntimeOG()" in probe_body
        and "runtimeVersion != supportedRuntime" in probe_body
        and "resolvedAddress != expectedAddress" in probe_body
        and "VirtualQuery(" in probe_body
        and "memory.State != MEM_COMMIT" in probe_body
        and "memory.Type != MEM_IMAGE" in probe_body
        and "memory.AllocationBase != moduleHandle" in probe_body
        and "IsExecutableProtection(memory.Protect)" in probe_body
        and "IsWritableProtection(memory.Protect)" in probe_body
        and "IMAGE_SCN_MEM_EXECUTE" in probe_body
        and "IMAGE_SCN_MEM_WRITE" in probe_body
        and "resolvedAddress % target.alignment != 0" in probe_body
        and "kLoggedPrologueBytes = 16" in world_only_renderer
        and "prologue[16]" in probe_body,
        "World-only OG preflight does not prove exact loaded-image RVAs, permissions, alignment, and bounded prologue logging",
    )
    for prologue_name, expected_bytes in (
        (
            "kRenderPreUIPrologue",
            "0x4C, 0x8B, 0xDC, 0x56, 0x48, 0x83, 0xEC, 0x70",
        ),
        (
            "kRenderBatchesPrologue",
            "0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C",
        ),
        (
            "kDoZPrePassPrologue",
            "0x48, 0x8B, 0xC4, 0x55, 0x53, 0x41, 0x57, 0x48",
        ),
        (
            "kDoUmbraQueryPrologue",
            "0x48, 0x81, 0xEC, 0x98, 0x00, 0x00, 0x00, 0xE8",
        ),
    ):
        require(
            prologue_name in world_only_renderer
            and expected_bytes in world_only_renderer,
            f"missing exact Stage 5a measurement for {prologue_name}",
        )
    require(
        "[[nodiscard]] bool InstallPassThroughHooks();"
        in world_only_renderer_header
        and pass_through_install_start > probe_start
        and "ProbeRuntimeTargets()" in pass_through_install_body
        and "MatchesKnownPrologue(" in pass_through_install_body
        and "std::array<HookBinding, 4>" in pass_through_install_body
        and "HookDoUmbraQuery" in pass_through_install_body
        and "MH_CreateHook(" in pass_through_install_body
        and "MH_QueueEnableHook(" in pass_through_install_body
        and "MH_ApplyQueued()" in pass_through_install_body
        and "MH_EnableHook(" not in pass_through_install_body
        and "rollback();" in pass_through_install_body
        and "MH_QueueDisableHook(" in pass_through_install_body
        and "const auto applyDisableStatus = MH_ApplyQueued();"
        in pass_through_install_body
        and "cleanupSucceeded" in pass_through_install_body
        and "forwarding mode remains latched"
        in pass_through_install_body
        and "passThroughOnly.store(true" in pass_through_install_body,
        "Stage 5b does not install all four exact-prologue detours as one queued forwarding transaction",
    )
    hook_render_preui_start = world_only_renderer.find(
        "void __fastcall HookRenderPreUI("
    )
    hook_render_batches_start = world_only_renderer.find(
        "void __fastcall HookRenderBatches(",
        hook_render_preui_start,
    )
    hook_render_preui_body = world_only_renderer[
        hook_render_preui_start:hook_render_batches_start
    ]
    first_functional_hook = forwarding_install_body.find(
        '"DrawWorld::Render_PreUI"'
    )
    second_functional_hook = forwarding_install_body.find(
        '"BSShaderAccumulator::RenderBatches"',
        first_functional_hook,
    )
    first_excluded_hook = forwarding_install_body.find(
        '"Renderer::DoZPrePass"',
        second_functional_hook,
    )
    capture_boundary_start = world_only_renderer.find(
        "bool WorldOnlyScopeRenderer::CaptureBeforeFirstPersonDraw("
    )
    capture_boundary_end = world_only_renderer.find(
        "bool WorldOnlyScopeRenderer::IsInstalled()",
        capture_boundary_start,
    )
    capture_boundary_body = world_only_renderer[
        capture_boundary_start:capture_boundary_end
    ]
    qualified_boundary_start = world_only_renderer.find(
        "[[nodiscard]] bool IsQualifiedMainColorBoundary("
    )
    qualified_boundary_end = world_only_renderer.find(
        "class ScratchContentGuard final",
        qualified_boundary_start,
    )
    qualified_boundary_body = world_only_renderer[
        qualified_boundary_start:qualified_boundary_end
    ]
    acquire_color_start = world_only_renderer.find(
        "WorldOnlyScopeRenderer::AcquireColorSRV(PhaseToken token)"
    )
    acquire_color_end = world_only_renderer.find(
        "WorldOnlyScopeRenderer::AcquireDepthSRV(",
        acquire_color_start,
    )
    acquire_color_body = world_only_renderer[
        acquire_color_start:acquire_color_end
    ]
    require(
        forwarding_install_start >= 0
        and forwarding_install_start < install_start
        and "return InstallForwardingHooks(false, true);"
        in install_body
        and "const std::size_t hookCount = functional ? 2 : hooks.size();"
        in forwarding_install_body
        and first_functional_hook >= 0
        and second_functional_hook > first_functional_hook
        and first_excluded_hook > second_functional_hook
        and "for (std::size_t index = 0; index < hookCount; ++index)"
        in forwarding_install_body
        and "state.passThroughOnly.store(false"
        in forwarding_install_body
        and hook_render_preui_start >= 0
        and hook_render_batches_start > hook_render_preui_start
        and "ExecuteAuxiliaryPass(" not in hook_render_preui_body
        and "g_auxiliaryPassActive" not in hook_render_preui_body
        and "Phase::kAuxiliary" not in hook_render_preui_body
        and "No nested Render_PreUI call is made."
        in hook_render_preui_body,
        "functional Stage 5c is not restricted to ordinary Render_PreUI plus RenderBatches, or still permits auxiliary Render_PreUI re-entry",
    )
    require(
        capture_boundary_start >= 0
        and capture_boundary_end > capture_boundary_start
        and "thread_local std::uint32_t g_firstPersonCaptureArmDepth"
        in world_only_renderer
        and "thread_local std::uint64_t g_firstPersonCaptureFrame"
        in world_only_renderer
        and "g_firstPersonCaptureArmDepth == 0"
        in capture_boundary_body
        and "g_firstPersonCaptureFrame == 0"
        in capture_boundary_body
        and "g_firstPersonCaptureFrame !="
        in capture_boundary_body
        and "state.captureFrameGeneration.load("
        in capture_boundary_body
        and "firstPersonCaptureClaimed.compare_exchange_strong("
        in capture_boundary_body
        and "IsQualifiedMainColorBoundary(" in capture_boundary_body
        and "context->GetDevice(contextDevice.GetAddressOf())"
        in capture_boundary_body
        and "mainColor->GetDevice(resourceDevice.GetAddressOf())"
        in capture_boundary_body
        and "contextDevice.As(&contextDeviceIdentity)"
        in capture_boundary_body
        and "resourceDevice.As(&resourceDeviceIdentity)"
        in capture_boundary_body
        and "contextDeviceIdentity.Get() !="
        in capture_boundary_body
        and "resourceDeviceIdentity.Get()" in capture_boundary_body
        and "context->CopyResource(\n"
        "\t\t\t\t\tstate.privateColor.texture.Get(),\n"
        "\t\t\t\t\tmainColor);" in capture_boundary_body
        and capture_boundary_body.count("context->CopyResource(") == 1
        and "mainDepth" not in capture_boundary_body
        and "privateDepth" not in capture_boundary_body
        and "AcquireDepthSRV" not in capture_boundary_body
        and "state.outputReady.store(true" in capture_boundary_body
        and "Phase::kPrimaryEligible" in capture_boundary_body
        and "if (!published)" in capture_boundary_body
        and "state.firstPersonCaptureClaimed.store("
        in capture_boundary_body
        and "false," in capture_boundary_body[
            capture_boundary_body.find("if (!published)") :
        ]
        and "context->GetType(" not in capture_boundary_body
        and "D3D11_DEVICE_CONTEXT_IMMEDIATE" not in capture_boundary_body
        and "g_Context" not in capture_boundary_body
        and "renderPreUIOriginal" not in capture_boundary_body
        and "doZPrePassOriginal" not in capture_boundary_body
        and "doUmbraQueryOriginal" not in capture_boundary_body
        and "ExecuteAuxiliaryPass(" not in capture_boundary_body,
        "functional Stage 5c capture is not TLS-admitted, deferred-context capable, color-only, retryable, or bound to the exact context/RT4 device",
    )
    require(
        qualified_boundary_start >= 0
        and qualified_boundary_end > qualified_boundary_start
        and "context->OMGetRenderTargets(" in qualified_boundary_body
        and "for (auto*& renderTarget : renderTargets)"
        in qualified_boundary_body
        and "GetResource(" in qualified_boundary_body
        and "IID_PPV_ARGS(" in qualified_boundary_body
        and "mainColorIdentity" in qualified_boundary_body
        and "resourceIdentity.Get() ==" in qualified_boundary_body
        and "mainColorIdentity.Get()" in qualified_boundary_body
        and "++mainColorMatches;" in qualified_boundary_body
        and "return mainColorMatches == 1;"
        in qualified_boundary_body
        and "context->GetType(" not in qualified_boundary_body
        and "D3D11_DEVICE_CONTEXT_IMMEDIATE" not in qualified_boundary_body
        and "g_Context" not in qualified_boundary_body,
        "Stage 5c boundary qualification does not prove one unique live OM RTV identity match to the engine renderer's RT4 resource on the passed context",
    )
    require(
        acquire_color_start >= 0
        and acquire_color_end > acquire_color_start
        and "token.phase != Phase::kPrimaryEligible"
        in acquire_color_body
        and "token.generation == 0" in acquire_color_body
        and "token.generation != g_renderPhaseGeneration"
        in acquire_color_body
        and "!state.outputReady.load(" in acquire_color_body
        and "state.generation.load(std::memory_order_acquire) !="
        in acquire_color_body
        and "token.generation" in acquire_color_body
        and "state.privateColor.shaderResourceView" in acquire_color_body,
        "Stage 5c color acquisition can consume a stale or non-current capture generation",
    )
    render_batches_position = world_only_renderer.find(
        "void __fastcall HookRenderBatches("
    )
    render_batches_end = world_only_renderer.find(
        "void __fastcall HookDoZPrePass(",
        render_batches_position,
    )
    render_batches_body = world_only_renderer[
        render_batches_position:render_batches_end
    ]
    require(
        render_batches_position >= 0
        and render_batches_end > render_batches_position
        and "const bool captureBoundary =" in render_batches_body
        and "state.requested.load(std::memory_order_acquire)"
        in render_batches_body
        and "accumulator ==\n"
        "\t\t\t\t\t\tstate.primaryFirstPersonAccumulator.load("
        in render_batches_body
        and "FirstPersonCaptureArm arm(" in render_batches_body
        and "state.captureFrameGeneration.load("
        in render_batches_body
        and "state.renderBatchesOriginal(" in render_batches_body
        and "g_firstPersonCaptureArmDepth" not in render_batches_body
        and "doZPrePassOriginal" not in render_batches_body
        and "doUmbraQueryOriginal" not in render_batches_body
        and "ExecuteAuxiliaryPass(" not in render_batches_body,
        "functional Stage 5c does not arm capture only around the ordinary first-person RenderBatches call",
    )

    database_path = (
        workspace
        / "PluginTemplate"
        / "AddressLibrary"
        / "F4SE"
        / "Plugins"
        / "version-1-10-163-0.bin"
    )
    require(database_path.is_file(), f"missing OG address library: {database_path}")
    database = load_address_library(database_path)
    require(
        database.get(HUD_WORLD_TO_SCREEN_ID) == HUD_WORLD_TO_SCREEN_OG_RVA,
        "HUD world-to-screen ID does not resolve to AE2840 in the OG corpus",
    )
    require(
        database.get(TAA_VTABLE_ID) == TAA_VTABLE_OG_RVA,
        "TAA vtable ID does not resolve to 3098B88 in the OG corpus",
    )
    require(
        database.get(CRASH_OWNER_ID) == CRASH_OWNER_RVA,
        "expected owner function 386055 does not resolve to A37890",
    )
    for relocation_id, expected_rva in WORLD_ONLY_OG_TARGETS.items():
        require(
            database.get(relocation_id) == expected_rva,
            "world-only OG target "
            f"{relocation_id} does not resolve to {expected_rva:X}",
        )
    require(
        WORLD_ONLY_OG_TARGETS[1_430_301] % 8 == 0
        and WORLD_ONLY_OG_TARGETS[163_482] % 4 == 0
        and WORLD_ONLY_OG_TARGETS[382_658] % 4 == 0,
        "world-only OG data RVAs do not satisfy their 8/4-byte alignment contracts",
    )
    require(
        CRASH_CALL_RVA not in database.values(),
        "A37940 unexpectedly became a valid address-library function entry",
    )

    # Runtime fixture captured from the live SCAR-H Elcan ScopeFade geometry.
    # The original scope-rendering negative-Z camera contract places it inside the
    # 3840x2160 viewport. The discarded Y-forward projection placed the same
    # point at y=-4485, so this fixture prevents that plausible-looking but
    # incorrect transform from returning.
    camera_x, camera_y, camera_z = 5.6302, -1.7986, -15.8803
    width, height, fov_degrees = 3840.0, 2160.0, 80.0
    half_height = math.tan(math.radians(fov_degrees) * 0.5)
    forward = -camera_z
    aspect = width / height
    ndc_x = -camera_x / (forward * half_height * aspect)
    ndc_y = -camera_y / (forward * half_height)
    pixel_x = (ndc_x + 1.0) * 0.5 * width
    pixel_y = (1.0 - ndc_y) * 0.5 * height
    require(
        1450.0 < pixel_x < 1480.0
        and 920.0 < pixel_y < 950.0
        and forward > 0.0,
        "live ScopeFade fixture no longer projects inside the viewport",
    )
    # The active Elcan has no Glass:0 extent. Its direct ScopeAiming _STS
    # housing has a live sphere radius of about 4.02. MagnaScope takes a
    # conservative planar fraction while retaining ScopeFade center and depth.
    housing_world_radius = 4.0211
    aperture_world_radius = housing_world_radius * 0.82
    projected_radius = (
        aperture_world_radius / (forward * half_height) * 0.5 * height
    )
    require(
        265.0 < projected_radius < 275.0,
        "live Elcan ScopeFade-plane aperture no longer matches the housing-derived extent",
    )

    print(
        "Safety contracts passed: HUD projection ID "
        f"{HUD_WORLD_TO_SCREEN_ID} -> {HUD_WORLD_TO_SCREEN_OG_RVA:X}; "
        f"bad RVA {CRASH_CALL_RVA:X} remains inside "
        f"ID {CRASH_OWNER_ID} at +{CRASH_CALL_RVA - CRASH_OWNER_RVA:X}; "
        f"ScopeFade fixture -> ({pixel_x:.2f}, {pixel_y:.2f}); "
        f"Elcan ScopeFade-plane radius -> {projected_radius:.2f}px; "
        "package stage=0."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
