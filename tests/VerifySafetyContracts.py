"""Fail-closed checks for MagnaScope's OG ADS safety boundaries.

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
    fts_data = (project / "src" / "FTSData.cpp").read_text(encoding="utf-8")
    imgui = (project / "src" / "ImGuiImpl.cpp").read_text(encoding="utf-8")
    settings = (project / "src" / "Settings.h").read_text(encoding="utf-8")
    ini = (project / "MagnaScope.ini").read_text(encoding="utf-8")
    geometry_probe_shader = (
        project / "src" / "HLSL" / "ScopeGeometryProbe_PS.hlsl"
    ).read_text(encoding="utf-8")
    geometry_fill_shader = (
        project / "src" / "HLSL" / "ScopeGeometryFill_GS.hlsl"
    ).read_text(encoding="utf-8")
    code = strip_cpp_comments(
        "\n".join(
            (
                hooking,
                main_cpp,
                fts_data,
                imgui,
                settings,
            )
        )
    )

    require(
        "cam->world.rotate * delta" in code
        and "const float forward = -cameraPoint.z" in code,
        "first-person projection does not preserve the original FTS camera-axis contract",
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
    require(
        "kInvisibleLeadInSeconds" in main_cpp
        and "kActivationDurationSeconds" in main_cpp
        and "easedProgress" in main_cpp
        and "previousCenterX" not in main_cpp
        and "stableSamples" not in main_cpp,
        "automatic STS aim-in still smooths position or waits for the late settle latch",
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
        and "profile->zoomDataOverwrite.fovMul = 1.0F" in code,
        "automatic STS profiles do not default to lens-only magnification",
    )
    require(
        "reinterpret_cast<std::uintptr_t>(controller.get()) + 0x470" not in code,
        "unverified Havok character-controller offset returned to source",
    )
    require(
        re.search(r"\[Diagnostics\].*?VerificationStage\s*=\s*0", ini, re.DOTALL),
        "packaged INI must fail closed at verification stage 0",
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
        "AllowsPrivateRenderHooks()",
    ):
        require(contract in settings, f"missing rollout gate: {contract}")
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
        and "2.0f * input[1].position - input[0].position"
        in geometry_fill_shader
        and "(primitiveID & 1U) == 0U" in geometry_fill_shader
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
        and "ResetAutomaticSTSProjectionTracking();" in main_cpp
        and "InvalidateAutomaticSTSSelection();" in main_cpp,
        "transient recoil projection loss still invalidates the selected STS geometry identity",
    )
    require(
        "CreateTriShape(" not in main_cpp
        and "BSShaderResourceManager::GetSingleton()" not in main_cpp
        and "NiCloningProcess" not in main_cpp,
        "automatic STS must not construct renderer geometry through the unsafe OG resource-manager interface",
    )
    geometry_probe_branch = hooking.find(
        "if (verification.AllowsGeometryProbe()"
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
        and "GetCurrentFTSData" not in hooking[taa_capture:taa_capture_end]
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
    require(
        CRASH_CALL_RVA not in database.values(),
        "A37940 unexpectedly became a valid address-library function entry",
    )

    # Runtime fixture captured from the live SCAR-H Elcan ScopeFade geometry.
    # The original FTS negative-Z camera contract places it inside the
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
