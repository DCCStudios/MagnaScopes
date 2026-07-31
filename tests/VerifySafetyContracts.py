"""Production safety contracts for MagnaScope's late ScopeFade replay.

Stage 5c's private world-render detours are intentionally not part of this
contract.  The production path follows Fake Through Scope's demonstrated
ordering instead: record the authored aperture draw, let Fallout finish the
weapon and world color, then replay only that aperture against a coherent late
color copy after TAA or immediately before Present.
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


def strip_cpp_comments(source: str) -> str:
    source = re.sub(r"/\*.*?\*/", "", source, flags=re.DOTALL)
    return re.sub(r"//.*", "", source)


def function_body(source: str, signature: str) -> str:
    start = source.find(signature)
    require(start >= 0, f"missing function: {signature}")
    brace = source.find("{", start)
    require(brace >= 0, f"missing function body: {signature}")
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    raise RuntimeError(f"unterminated function body: {signature}")


def load_address_library(path: Path) -> dict[int, int]:
    data = path.read_bytes()
    require(len(data) >= 8, f"truncated address-library header: {path}")
    count = struct.unpack_from("<Q", data, 0)[0]
    require(
        len(data) == 8 + count * 16,
        f"address-library size mismatch: {path}",
    )
    return {
        struct.unpack_from("<QQ", data, 8 + index * 16)[0]:
        struct.unpack_from("<QQ", data, 8 + index * 16)[1]
        for index in range(count)
    }


def main() -> int:
    project = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path.cwd()
    workspace = project.parent

    hooking_raw = (project / "src" / "hooking.cpp").read_text(encoding="utf-8")
    hooking_h = (project / "src" / "hooking.h").read_text(encoding="utf-8")
    main_raw = (project / "src" / "main.cpp").read_text(encoding="utf-8")
    data_h = (project / "src" / "FTSData.h").read_text(encoding="utf-8")
    data_cpp = (project / "src" / "FTSData.cpp").read_text(encoding="utf-8")
    settings = (project / "src" / "Settings.h").read_text(encoding="utf-8")
    xmake = (project / "xmake.lua").read_text(encoding="utf-8")
    shader = (
        project / "src" / "HLSL" / "ScopeGeometryMagnify_PS.hlsl"
    ).read_text(encoding="utf-8")
    geometry_fill_shader = (
        project / "src" / "HLSL" / "ScopeGeometryFill_GS.hlsl"
    ).read_text(encoding="utf-8")
    reticle_shader = (
        project / "src" / "HLSL" / "ReticleLayer_PS.hlsl"
    ).read_text(encoding="utf-8")
    triangle_shader = (
        project / "src" / "HLSL" / "Triangle.hlsli"
    ).read_text(encoding="utf-8")
    stage5d = (
        project / "tests" / "config" / "MagnaScope.Stage5d.ini"
    ).read_text(encoding="utf-8")
    package_ini = (project / "MagnaScope.ini").read_text(encoding="utf-8")

    hooking = strip_cpp_comments(hooking_raw)
    main_cpp = strip_cpp_comments(main_raw)

    # The production fixture must enable only the already verified Stage 4
    # hook surface.  Every private-world-render gate remains off.
    for setting in (
        "VerificationStage=4",
        "AuxiliaryPassThroughHooks=0",
        "AuxiliaryObservationHooks=0",
        "AuxiliaryWorldPass=0",
        "TAACapture=1",
        "VisualProbe=0",
        "GeometryProbe=1",
        "GeometryMagnification=1",
        "Enabled=1",
    ):
        require(setting in stage5d, f"Stage 5d fixture missing {setting}")
    for setting in (
        "VerificationStage=4",
        "AuxiliaryPassThroughHooks=0",
        "AuxiliaryObservationHooks=0",
        "AuxiliaryWorldPass=0",
        "TAACapture=1",
        "VisualProbe=0",
        "GeometryProbe=1",
        "GeometryMagnification=1",
        "Enabled=1",
    ):
        require(setting in package_ini, f"production package missing {setting}")

    # The crash-producing Stage 5c renderer is absent from the production
    # translation units and explicitly excluded from the DLL. Keeping its
    # source as forensic evidence must not make it callable or linkable.
    require(
        "WorldOnlyScopeRenderer" not in main_cpp
        and "WorldOnlyScopeRenderer" not in hooking,
        "production code still references the retired auxiliary renderer",
    )
    require(
        'remove_files("src/WorldOnlyScopeRenderer.cpp")' in xmake,
        "production build can still link the retired auxiliary renderer",
    )
    require(
        "return false;" in function_body(
            settings,
            "bool AllowsAuxiliaryWorldPass() const noexcept",
        ),
        "the retired auxiliary world pass can still be authorized",
    )

    draw = function_body(
        hooking,
        "void __stdcall D3D::DrawIndexedHook(",
    )
    draw_instanced = function_body(
        hooking,
        "void __stdcall D3D::DrawIndexedInstancedHook(",
    )
    require(
        "CaptureBeforeFirstPersonDraw(" not in draw
        and "CaptureBeforeFirstPersonDraw(" not in draw_instanced,
        "D3D draw hooks still capture a partial pre-first-person color target",
    )
    require(
        "AcquireColorSRV(" not in draw
        and "AllowsAuxiliaryWorldPass(" not in draw
        and "AcquireColorSRV(" not in draw_instanced
        and "AllowsAuxiliaryWorldPass(" not in draw_instanced,
        "D3D draw hooks retain an auxiliary world-color source",
    )

    capture = function_body(
        hooking,
        "bool D3D::CaptureAutomaticSTSScopeFadeReplay(",
    )
    replay = function_body(
        hooking,
        "bool D3D::ReplayAutomaticSTSScopeFade(",
    )
    render_aperture = function_body(
        hooking,
        "bool D3D::RenderToReticleTexture()",
    )
    require(
        "CaptureAutomaticSTSScopeFadeReplay(" in draw
        and "if (captured)" in draw
        and re.search(
            r"if\s*\(captured\).*?phookD3D11DrawIndexed\s*\(\s*"
            r"pContext\s*,\s*0\s*,\s*0\s*,\s*0\s*\)",
            draw,
            flags=re.DOTALL,
        ),
        "exact ScopeFade draw is not captured and color-suppressed like FTS",
    )
    require(
        "PrepareScopeFadeSceneSource(" not in capture
        and "PSSetShader(" not in capture
        and "GSSetShader(" not in capture
        and "CopyResource(" in capture,
        "capture phase modifies pixels or omits private transform snapshots",
    )
    require(
        "ReplayAutomaticSTSScopeFade(" in render_aperture
        and "mShaderResourceView.Get()" in render_aperture
        and "PrepareScopeFadeSceneSource(" in replay
        and "DrawIndexed(" in replay
        and "BSScopeFadeReplaceRGB.Get()" in replay,
        "late coherent-color ScopeFade replay is incomplete",
    )
    exact_ready_start = render_aperture.find("if (!exactScopeFadeReady)")
    exact_ready_end = render_aperture.find(
        "ID3D11RenderTargetView* const compositeTarget",
        exact_ready_start,
    )
    require(
        exact_ready_start >= 0
        and exact_ready_end > exact_ready_start
        and "return false;"
        in render_aperture[exact_ready_start:exact_ready_end],
        "automatic STS can fall back to a detached fullscreen aperture",
    )
    require(
        "VSGetShader(vertexShader.GetAddressOf(), nullptr, nullptr)" in capture
        and "D3D11_SHADER_MAX_INTERFACES" not in capture
        and "ID3D11ClassInstance" not in capture
        and "vertexClassInstances" not in replay,
        "ScopeFade replay reintroduced unsafe dynamic-linkage array capture",
    )
    require(
        "D3D11_DEVICE_CONTEXT_IMMEDIATE" in capture
        and "HaveSameCOMIdentity(context, g_Context.Get())" in capture
        and "HaveSameCOMIdentity(device.Get(), g_Device.Get())" in capture,
        "ScopeFade capture accepts a deferred or foreign D3D context/device",
    )
    require(
        "automaticSTSReplayFrameGeneration" in capture
        and "automaticSTSReplayFrameGeneration" in replay
        and "replay.generation !=" in replay
        and "automaticSTSReplayFrameGeneration.fetch_add(" in hooking,
        "late replay can consume a stale frame's aperture transform",
    )
    prepare = function_body(
        hooking,
        "bool D3D::PrepareScopeFadeSceneSource(",
    )
    require(
        "HaveSameCOMIdentity(" in prepare
        and "sourceResource.Get()" in prepare
        and "targetResource.Get()" in prepare
        and "return false;" in prepare,
        "late replay can bind the same resource for sampling and output",
    )

    # Replay touches IA, VS, GS, PS, RS, and OM state. All touched shader
    # stages, viewports, and scissor rectangles must be restored before
    # Fallout resumes rendering. Fallout's active shaders on this path do not
    # use dynamic linkage; querying maximum-sized class-instance arrays through
    # the AAAFrameGeneration context proxy caused the 2026-07-31 ADS crash.
    save_state = function_body(hooking, "void SaveState(")
    restore_state = function_body(hooking, "void RestoreState(")
    for token in (
        "VSGetShader(",
        "GSGetShader(",
        "PSGetShader(",
        "RSGetViewports(",
        "RSGetScissorRects(",
        "OMGetRenderTargets(",
        "OMGetBlendState(",
        "OMGetDepthStencilState(",
    ):
        require(token in save_state, f"state guard does not save {token}")
    for token in (
        "VSSetShader(",
        "GSSetShader(",
        "PSSetShader(",
        "RSSetViewports(",
        "RSSetScissorRects(",
        "OMSetRenderTargets(",
        "OMSetBlendState(",
        "OMSetDepthStencilState(",
    ):
        require(token in restore_state, f"state guard does not restore {token}")
    require(
        "VSGetShader(&state.pVS, nullptr, nullptr)" in save_state
        and "GSGetShader(&state.pGS, nullptr, nullptr)" in save_state
        and "PSGetShader(&state.pPS, nullptr, nullptr)" in save_state
        and "ID3D11ClassInstance" not in save_state
        and "pVSClassInstances" not in restore_state
        and "pGSClassInstances" not in restore_state
        and "pPSClassInstances" not in restore_state
        and restore_state.count("SAFE_RELEASE_ARRAY(") >= 7,
        "state guard reintroduced unsafe class-instance arrays or leaks COM state",
    )
    require(
        "kMaxClassInstances" not in draw
        and "pixelClassInstances" not in draw
        and "geometryClassInstances" not in draw,
        "ScopeFade probe still consumes class-instance slots outside returned counts",
    )

    # Fake Through Scope runs after the original TAA callback.  Present is the
    # fallback and the replay packet is invalidated only after real Present.
    taa = function_body(hooking, "static void thunk(")
    present = function_body(hooking, "HRESULT __fastcall D3D::PresentHook(")
    require(
        taa.find("original(This, a_geometry, a_param);")
        < taa.find("D3DInstance->Render();")
        and "!renderPassHandledThisFrame" in taa,
        "TAA replay no longer runs after original TAA or lacks the frame guard",
    )
    require(
        present.find("D3DInstance->Render();")
        < present.find("oldFuncs.phookD3D11Present(")
        < present.find("ClearAutomaticSTSScopeFadeReplay();")
        and "!renderPassHandledThisFrame" in present,
        "Present is not a guarded fallback or clears replay state too early",
    )
    require(
        "if (!compositedThisFrame && !renderPassHandledThisFrame)" in taa
        and "renderedAtTAAThisFrame = false;" in taa,
        "a failed TAA replay cannot fall back to Present",
    )

    # Optical effects operate on one continuous logical ScopeFade coordinate
    # field.  Never reconstruct a flat frame independently per primitive: the
    # packed STS rings are not perfectly concentric, and those tiny differences
    # become visible radial facets and a center star after magnification.
    require(
        "float2 lensCoordinates : TEXCOORD0" in geometry_fill_shader
        and "outerCurrentCoordinates" in geometry_fill_shader
        and "innerCurrentCoordinates" in geometry_fill_shader
        and "float2(0.0f, 0.0f)" in geometry_fill_shader
        and "nointerpolation" not in geometry_fill_shader
        and "const float2 normalizedLensPosition = input.lensCoordinates"
        in shader
        and "const float2 publishedAimPixels" in shader
        and "const float2 samplePivotUv = publishedAimPixels * PixelSize"
        in shader
        and "float2(SCOPE_EYE_OFFSET_X, SCOPE_EYE_OFFSET_Y)" in shader
        and "pupilShadow" in shader
        and "const float pupilRadius" in shader
        and "rimVisibility" not in shader
        and "pupilVisibility" not in shader
        and "SCOPE_EYE_RELIEF_DELTA" not in shader
        and "SCOPE_FISHEYE_STRENGTH" in shader
        and "SCOPE_EDGE_REFRACTION_STRENGTH" in shader
        and "SCOPE_EDGE_CHROMATIC_ABERRATION" in shader
        and "edgeAwareAverage" in shader,
        "shipping shader lost lens-local optics or reintroduced global flicker",
    )
    require(
        "GetGunStateNibble" in main_cpp
        and "& 0xFU" in main_cpp
        and "CameraStates::kIronSights" in main_cpp
        and "PublishAutomaticSTSGunState(\n"
        "\t\t\t\t\tGetGunStateNibble(player))" in main_raw,
        "firing ADS is not normalized through the signed gun-state nibble and iron-sights camera",
    )
    require(
        "const bool automaticADS" in main_cpp
        and "else if (automaticSTSTracking.aperture != scopeNode)" in main_cpp
        and "Preserve the last publication and monotonic activation" in main_raw,
        "transient recoil projection loss can still reset optical activation",
    )
    require(
        "const float2 stableShadowCoordinates = normalizedLensPosition"
        in shader
        and "SCOPE_SCENE_PARALLAX_STRENGTH" in shader
        and "physicalEyeTravel /" in shader
        and "1.0f + 2.0f * sceneTravelLength" in shader
        and "sampleDelta -=\n            eyeParallaxPixels" in shader,
        "aperture-derived shadow fit or bounded scene parallax is missing",
    )
    require(
        "float radius = 2;" in data_h
        and "float maxTravel = 4;" in data_h
        and "profile->shaderData.parallax.radius = 2.0F;" in data_cpp
        and "profile->shaderData.parallax.maxTravel = 4.0F;" in data_cpp
        and "profile->shaderData.sceneParallaxStrength = 1.0F;" in data_cpp,
        "automatic STS eye-box or scene-parallax defaults regressed",
    )
    require(
        "EyeBoxRecentering::CalculateBlend(" in main_cpp
        and "state.baselineEyeLocal +=" in main_cpp
        and "normalizedDisplacement" in main_cpp
        and "planarLength > maximumTravel" in main_cpp
        and "ResetAutomaticSTSEyeBoxTracking();" in main_cpp
        and "float2(SCOPE_EYE_OFFSET_X, SCOPE_EYE_OFFSET_Y)" in shader
        and "absolute distance from screen" in shader
        and "CalculateScopeDrawTimeEyeTravel(" not in shader,
        "eye-box motion is not self-centering or still depends on absolute look direction",
    )
    # The reticle is a true second layer. The exact authored draw is redirected
    # into same-format black- and white-background private targets only after
    # this frame's exact ScopeFade capture has transferred aperture authority.
    # The pair reconstructs the authored source contribution and destination
    # transmittance without guessing from alpha. Suppression is earned only
    # after both private draws succeed, so every failure falls through to the
    # ordinary authored reticle draw.
    reticle_capture = function_body(
        hooking,
        "bool D3D::BeginAutomaticSTSReticleLayerCapture(",
    )
    reticle_suppression = function_body(
        hooking,
        "bool D3D::ApplyAutomaticSTSReticleColorSuppression(",
    )
    reticle_complete = function_body(
        hooking,
        "void D3D::CompleteAutomaticSTSReticleLayerCapture(",
    )
    reticle_composite = function_body(
        hooking,
        "bool D3D::CompositeAutomaticSTSReticleLayer(",
    )
    reticle_clear = function_body(
        hooking,
        "void D3D::ClearAutomaticSTSReticleLayer() noexcept",
    )
    require(
        "layerDescription.Format = sourceDescription.Format;"
        in reticle_capture
        and "layerSRVDescription.Format = sourceViewDescription.Format;"
        in reticle_capture
        and "layerDescription.Format != sourceDescription.Format"
        in reticle_capture
        and "layerViewDescription.Format !=" in reticle_capture
        and "sourceViewDescription.Format" in reticle_capture
        and "DXGI_FORMAT_R8G8B8A8_UNORM" not in reticle_capture
        and "D3D11_BIND_RENDER_TARGET" in reticle_capture
        and "D3D11_BIND_SHADER_RESOURCE" in reticle_capture
        and "bool whiteBackground" in reticle_capture
        and "mAutomaticSTSReticleLayerWhiteTexture" in reticle_capture
        and "mAutomaticSTSReticleLayerWhiteRTV" in reticle_capture
        and "mAutomaticSTSReticleLayerWhiteSRV" in reticle_capture
        and "whiteBackground ?" in reticle_capture
        and "mAutomaticSTSReticleLayerWhiteRTV.Get()" in reticle_capture
        and "mAutomaticSTSReticleLayerRTV.Get()" in reticle_capture
        and "OMSetRenderTargets(1U, &layerTarget, sourceDepth)"
        in reticle_capture
        and "OMSetBlendState(" not in reticle_capture
        and re.search(
            r"opaqueBlack\s*\[\s*4\s*\]\s*\{\s*"
            r"0\.0F\s*,\s*0\.0F\s*,\s*0\.0F\s*,\s*1\.0F\s*\}",
            reticle_capture,
        )
        and re.search(
            r"opaqueWhite\s*\[\s*4\s*\]\s*\{\s*"
            r"1\.0F\s*,\s*1\.0F\s*,\s*1\.0F\s*,\s*1\.0F\s*\}",
            reticle_capture,
        )
        and "ClearRenderTargetView(\n\t\t\t\tmAutomaticSTSReticleLayerRTV.Get(),"
        in reticle_capture
        and "ClearRenderTargetView(\n\t\t\t\tmAutomaticSTSReticleLayerWhiteRTV.Get(),"
        in reticle_capture
        and "mAutomaticSTSReticleLayerCaptureGeneration != frameGeneration"
        in reticle_capture
        and "mAutomaticSTSReticleLayerCaptureGeneration = frameGeneration;"
        in reticle_capture,
        "reticle capture lost its paired same-format authored-blend contract",
    )
    require(
        "targetBlend.SrcBlend = D3D11_BLEND_ONE;" in reticle_capture
        and "targetBlend.DestBlend = D3D11_BLEND_SRC1_COLOR;"
        in reticle_capture
        and "targetBlend.BlendOp = D3D11_BLEND_OP_ADD;"
        in reticle_capture
        and "targetBlend.SrcBlendAlpha = D3D11_BLEND_ZERO;"
        in reticle_capture
        and "targetBlend.DestBlendAlpha = D3D11_BLEND_ONE;"
        in reticle_capture
        and "targetBlend.BlendOpAlpha = D3D11_BLEND_OP_ADD;"
        in reticle_capture
        and "mAutomaticSTSReticleLayerCompositeBlend" in reticle_capture,
        "reticle composite lost B + destination*T dual-source blending",
    )
    require(
        "OMGetDepthStencilState(" in reticle_capture
        and "mAutomaticSTSReticleLayerAuthoredDepthState" in reticle_capture
        and "mAutomaticSTSReticleLayerAuthoredDepthWasNull" in reticle_capture
        and "mAutomaticSTSReticleLayerReadOnlyDepthState" in reticle_capture
        and "readOnlyDescription.DepthWriteMask =\n\t\t\t\tD3D11_DEPTH_WRITE_MASK_ZERO;"
        in reticle_capture
        and "readOnlyDescription.StencilWriteMask = 0U;" in reticle_capture
        and "CreateDepthStencilState(" in reticle_capture
        and "OMSetDepthStencilState(\n\t\t\tmAutomaticSTSReticleLayerReadOnlyDepthState.Get(),"
        in reticle_capture,
        "paired reticle capture no longer preserves authored read-only depth tests",
    )
    require(
        "D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT" in reticle_capture
        and "const bool targetZeroMatches" in reticle_capture
        and "if (!targetZeroMatches)" in reticle_capture
        and "OMGetBlendState(" in reticle_capture
        and "isSourceOnlyFactor" in reticle_capture
        and "authoredTarget.BlendOp != D3D11_BLEND_OP_ADD"
        in reticle_capture
        and "D3D11_DEVICE_CONTEXT_IMMEDIATE" in reticle_suppression
        and "HaveSameCOMIdentity(device.Get(), g_Device.Get())"
        in reticle_suppression
        and "OMGetBlendState(" in reticle_suppression
        and "blendFactor" in reticle_suppression
        and "sampleMask" in reticle_suppression
        and "suppressedDescription.IndependentBlendEnable = TRUE;"
        in reticle_suppression
        and "suppressedDescription.RenderTarget[slot] ="
        in reticle_suppression
        and "suppressedDescription.RenderTarget[0].RenderTargetWriteMask = 0U;"
        in reticle_suppression
        and "mAutomaticSTSReticleColorSuppressionBlend"
        in reticle_suppression
        and "OMSetBlendState(" in reticle_suppression,
        "reticle isolation no longer preserves auxiliary MRT state while muting only displayed RT0 color",
    )
    for token in (
        "D3D11_DEVICE_CONTEXT_IMMEDIATE",
        "HaveSameCOMIdentity(context, g_Context.Get())",
        "HaveSameCOMIdentity(device.Get(), g_Device.Get())",
        "sourceDescription.SampleDesc.Count != 1U",
        "D3D11_RTV_DIMENSION_TEXTURE2D",
    ):
        require(
            token in reticle_capture,
            f"reticle capture lost its fail-closed guard: {token}",
        )

    exact_reticle_start = draw.find("if (exactReticleMatch)")
    exact_reticle_end = draw.find("bool exactHousingMatch", exact_reticle_start)
    require(
        exact_reticle_start >= 0 and exact_reticle_end > exact_reticle_start,
        "indexed reticle ownership block is missing",
    )
    indexed_reticle = draw[exact_reticle_start:exact_reticle_end]
    indexed_lock = indexed_reticle.find(
        "LockAutomaticSTSReticleLayerCapture()"
    )
    indexed_authority = indexed_reticle.find(
        "automaticSTSExactScopeFadeReplacementThisFrame.load("
    )
    indexed_black_begin = indexed_reticle.find(
        "BeginAutomaticSTSReticleLayerCapture("
    )
    indexed_white_begin = indexed_reticle.find(
        "BeginAutomaticSTSReticleLayerCapture(",
        indexed_black_begin + 1,
    )
    indexed_black_draw = indexed_reticle.find(
        "oldFuncs.phookD3D11DrawIndexed("
    )
    indexed_white_draw = indexed_reticle.find(
        "oldFuncs.phookD3D11DrawIndexed(",
        indexed_black_draw + 1,
    )
    indexed_suppression = indexed_reticle.find(
        "ApplyAutomaticSTSReticleColorSuppression(",
        indexed_white_draw + 1,
    )
    indexed_authored_draw = indexed_reticle.find(
        "oldFuncs.phookD3D11DrawIndexed(",
        indexed_white_draw + 1,
    )
    indexed_success = indexed_reticle.find(
        "bool captured = capturedBlack && capturedWhite;"
    )
    indexed_complete = indexed_reticle.find(
        "CompleteAutomaticSTSReticleLayerCapture("
    )
    indexed_return = indexed_reticle.rfind("return;")
    require(
        0 <= indexed_authority < indexed_lock < indexed_black_begin
        < indexed_black_draw
        < indexed_white_begin < indexed_white_draw < indexed_success
        < indexed_suppression < indexed_authored_draw
        < indexed_complete < indexed_return
        and indexed_reticle.count(
            "BeginAutomaticSTSReticleLayerCapture("
        ) == 2
        and indexed_reticle.count(
            "oldFuncs.phookD3D11DrawIndexed("
        ) == 3
        and "bool capturedBlack = false;" in indexed_reticle
        and "bool capturedWhite = false;" in indexed_reticle
        and "if (capturedBlack)" in indexed_reticle
        and "ScopedContextState restoreAfterBlackCapture" in indexed_reticle
        and "ScopedContextState restoreAfterWhiteCapture" in indexed_reticle
        and "bool captured = capturedBlack && capturedWhite;"
        in indexed_reticle
        and "ApplyAutomaticSTSReticleColorSuppression(" in indexed_reticle
        and "ScopedContextState restoreAfterSuppression" in indexed_reticle
        and "if (captured) {\n\t\t\t\t\t\t\treturn;" in indexed_reticle
        and "return oldFuncs.phookD3D11DrawIndexed(pContext, IndexCount"
        in draw,
        "indexed reticle can be suppressed without both authored captures",
    )

    instanced_reticle_start = draw_instanced.find(
        "const bool exactReticleMatch"
    )
    instanced_reticle_end = draw_instanced.find(
        "if (matchesPublished(",
        instanced_reticle_start,
    )
    require(
        instanced_reticle_start >= 0
        and instanced_reticle_end > instanced_reticle_start,
        "instanced reticle ownership block is missing",
    )
    instanced_reticle = draw_instanced[
        instanced_reticle_start:instanced_reticle_end
    ]
    instanced_lock = instanced_reticle.find(
        "LockAutomaticSTSReticleLayerCapture()"
    )
    instanced_authority = instanced_reticle.find(
        "automaticSTSExactScopeFadeReplacementThisFrame.load("
    )
    instanced_black_begin = instanced_reticle.find(
        "BeginAutomaticSTSReticleLayerCapture("
    )
    instanced_white_begin = instanced_reticle.find(
        "BeginAutomaticSTSReticleLayerCapture(",
        instanced_black_begin + 1,
    )
    instanced_black_draw = instanced_reticle.find(
        "oldFuncs.phookD3D11DrawIndexedInstanced("
    )
    instanced_white_draw = instanced_reticle.find(
        "oldFuncs.phookD3D11DrawIndexedInstanced(",
        instanced_black_draw + 1,
    )
    instanced_suppression = instanced_reticle.find(
        "ApplyAutomaticSTSReticleColorSuppression(",
        instanced_white_draw + 1,
    )
    instanced_authored_draw = instanced_reticle.find(
        "oldFuncs.phookD3D11DrawIndexedInstanced(",
        instanced_white_draw + 1,
    )
    instanced_success = instanced_reticle.find(
        "bool captured = capturedBlack && capturedWhite;"
    )
    instanced_complete = instanced_reticle.find(
        "CompleteAutomaticSTSReticleLayerCapture("
    )
    instanced_return = instanced_reticle.rfind("return;")
    require(
        0 <= instanced_authority < instanced_lock < instanced_black_begin
        < instanced_black_draw
        < instanced_white_begin < instanced_white_draw < instanced_success
        < instanced_suppression < instanced_authored_draw
        < instanced_complete < instanced_return
        and instanced_reticle.count(
            "BeginAutomaticSTSReticleLayerCapture("
        ) == 2
        and instanced_reticle.count(
            "oldFuncs.phookD3D11DrawIndexedInstanced("
        ) == 3
        and "bool capturedBlack = false;" in instanced_reticle
        and "bool capturedWhite = false;" in instanced_reticle
        and "if (capturedBlack)" in instanced_reticle
        and "ScopedContextState restoreAfterBlackCapture" in instanced_reticle
        and "ScopedContextState restoreAfterWhiteCapture" in instanced_reticle
        and "bool captured = capturedBlack && capturedWhite;"
        in instanced_reticle
        and "ApplyAutomaticSTSReticleColorSuppression(" in instanced_reticle
        and "ScopedContextState restoreAfterSuppression" in instanced_reticle
        and "if (captured) {\n\t\t\t\t\treturn;" in instanced_reticle
        and draw_instanced.rfind("callOriginal();") > instanced_reticle_end,
        "instanced reticle can be suppressed without both authored captures",
    )

    scopefade_authority_start = draw.find(
        "if (exactGeometryMatch && exactSuballocationMatch)"
    )
    require(
        scopefade_authority_start >= 0,
        "indexed ScopeFade ownership block is missing",
    )
    scopefade_authority = draw[scopefade_authority_start:]
    require(
        "CaptureAutomaticSTSScopeFadeReplay(" in scopefade_authority
        and "automaticSTSExactScopeFadeReplacementThisFrame.store("
        in scopefade_authority
        and re.search(
            r"automaticSTSExactScopeFadeReplacementThisFrame\.store\(\s*"
            r"captured\s*,\s*std::memory_order_release\s*\)",
            scopefade_authority,
        ),
        "reticle ownership authority is not derived from ScopeFade capture",
    )

    # The optical replay must finish before the known reticle-layer pixel
    # shader runs. Slot b4 carries the shared projection and resolution
    # constants used by the post-optics reticle scaler and aperture clip.
    replay_position = render_aperture.find("ReplayAutomaticSTSScopeFade(")
    reticle_position = render_aperture.find(
        "CompositeAutomaticSTSReticleLayer("
    )
    require(
        0 <= replay_position < reticle_position
        and "if (replayed)" in render_aperture[
            replay_position:reticle_position
        ]
        and "m_pPixelShader_STSReticleLayer.Get()" in reticle_composite
        and "ID3D11ShaderResourceView* layerSources[2]"
        in reticle_composite
        and "mAutomaticSTSReticleLayerSRV.Get()" in reticle_composite
        and "mAutomaticSTSReticleLayerWhiteSRV.Get()" in reticle_composite
        and "PSSetShaderResources(4U, 2U, layerSources)"
        in reticle_composite
        and "PSSetShaderResources(4U, 2U, nullSources)"
        in reticle_composite
        and "mAutomaticSTSReticleLayerCompositeBlend.Get()"
        in reticle_composite
        and "PSSetConstantBuffers(4U, 1U, &resolutionBuffer)"
        in reticle_composite
        and '#include "Triangle.hlsli"' in reticle_shader
        and "register(b4)" in triangle_shader
        and "tBACKBUFFER : register(t4)" in triangle_shader
        and "ReticleTex : register(t5)" in triangle_shader,
        "reticle is not composited through the known b4/t4/t5 shader after optics",
    )
    for forbidden_optic in (
        "SCOPE_MAGNIFICATION",
        "SCOPE_FISHEYE_STRENGTH",
        "SCOPE_EDGE_REFRACTION_STRENGTH",
        "SCOPE_EDGE_CHROMATIC_ABERRATION",
        "SCOPE_DENOISE_STRENGTH",
        "SCOPE_SHARPEN_STRENGTH",
        "SCOPE_SCENE_PARALLAX_STRENGTH",
        "SCOPE_EYE_OFFSET_X",
        "SCOPE_EYE_OFFSET_Y",
    ):
        require(
            forbidden_optic not in reticle_shader,
            f"scene optic leaked into the isolated reticle: {forbidden_optic}",
        )
    require(
        "struct ReticleCompositeOutput" in reticle_shader
        and "SV_Target0" in reticle_shader
        and "SV_Target1" in reticle_shader
        and "ReticleCompositeOutput SampleAuthoredReticle(float2 uv)"
        in reticle_shader
        and "tBACKBUFFER.SampleLevel(gSamLinear, uv, 0.0f)"
        in reticle_shader
        and "ReticleTex.SampleLevel(gSamLinear, uv, 0.0f)"
        in reticle_shader
        and "saturate(whiteCapture - blackCapture)" in reticle_shader
        and "return SampleAuthoredReticle(input.tex);" in reticle_shader
        and "return SampleAuthoredReticle(sourceUv);" in reticle_shader
        and "const float2 reticleCenterPixel = float2(" in reticle_shader
        and "SCOPE_AIM_CENTER_X" in reticle_shader
        and "SCOPE_AIM_CENTER_Y" in reticle_shader
        and "saturate(SCOPE_FADE_ACTIVATION)" in reticle_shader
        and "(outputPixel - reticleCenterPixel) / reticleScale"
        in reticle_shader
        and "SCOPE_AIM_OFFSET_VALID" in reticle_shader
        and "basisX * authoredOffset.x" in reticle_shader
        and "basisZ * authoredOffset.y" in reticle_shader
        and "dot(lensCoordinates, lensCoordinates)" in reticle_shader,
        "reticle layer lost dual-source reconstruction, invalid-projection preservation, local-pivot scaling, or lens clipping",
    )

    # The retired paths either modified authored vertices before the scene
    # optics or replayed an unknown material shader into the final RTV. Their
    # definitions remain for forensic comparison, but no runtime site may call
    # them.
    scaled_reticle = function_body(
        hooking,
        "bool DrawReticleWithScaledVertices(",
    )
    captured_reticle_replay = function_body(
        hooking,
        "bool D3D::CaptureAutomaticSTSReticleReplay(",
    )
    authored_reticle_replay = function_body(
        hooking,
        "bool D3D::ReplayAutomaticSTSReticle(",
    )
    runtime_without_retired_reticle_paths = hooking.replace(
        scaled_reticle,
        "",
    ).replace(
        captured_reticle_replay,
        "",
    ).replace(
        authored_reticle_replay,
        "",
    )
    require(
        "DrawReticleWithScaledVertices(" not in runtime_without_retired_reticle_paths
        and "CaptureAutomaticSTSReticleReplay("
        not in runtime_without_retired_reticle_paths
        and "ReplayAutomaticSTSReticle("
        not in runtime_without_retired_reticle_paths,
        "a retired reticle mutation or authored-shader replay is callable",
    )

    require(
        "if (!captured)" in reticle_complete
        and "mAutomaticSTSReticleLayerReady = false;" in reticle_complete
        and "mAutomaticSTSReticleLayerCaptureGeneration = 0U;"
        in reticle_complete
        and "mAutomaticSTSReticleLayerGeneration = frameGeneration;"
        in reticle_complete
        and "mAutomaticSTSReticleLayerReady =" in reticle_complete
        and "mAutomaticSTSReticleLayerSRV.Get()" in reticle_complete
        and "mAutomaticSTSReticleLayerWhiteSRV.Get()" in reticle_complete
        and "mAutomaticSTSReticleLayerGeneration !="
        in reticle_composite
        and "automaticSTSReplayFrameGeneration.load("
        in reticle_composite
        and "mAutomaticSTSReticleLayerResourceGeneration !="
        in reticle_composite
        and "mScopeFadeResourceGeneration.load("
        in reticle_composite
        and "mAutomaticSTSReticleLayerReady = false;"
        in reticle_composite
        and "mAutomaticSTSReticleLayerGeneration = 0U;" in reticle_clear
        and "mAutomaticSTSReticleLayerResourceGeneration = 0U;"
        not in reticle_clear
        and "thread_local bool bSelfDraw = false;" in hooking
        and present.find("oldFuncs.phookD3D11Present(")
        < present.find("ClearAutomaticSTSReticleLayer();")
        < present.find("automaticSTSReplayFrameGeneration.fetch_add("),
        "reticle layer can survive its frame or resource generation",
    )

    # OG-only address contracts.  A37940 was the earlier crash-causing interior
    # address and must never be treated as a callable address-library entry.
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
        "HUD projection relocation changed in the OG corpus",
    )
    require(
        database.get(TAA_VTABLE_ID) == TAA_VTABLE_OG_RVA,
        "TAA vtable relocation changed in the OG corpus",
    )
    require(
        database.get(CRASH_OWNER_ID) == CRASH_OWNER_RVA
        and CRASH_CALL_RVA not in database.values(),
        "known crash interior address is being treated as a function entry",
    )
    require(
        "!REX::FModule::IsRuntimeOG()" in main_cpp
        and "MagnaScope is disabled safely" in main_raw,
        "unsupported NG/AE runtimes do not fail closed",
    )

    # Preserve the live projection fixture that caught the earlier incorrect
    # Y-forward camera-axis assumption.
    camera_x, camera_y, camera_z = 5.6302, -1.7986, -15.8803
    width, height, fov_degrees = 3840.0, 2160.0, 80.0
    half_height = math.tan(math.radians(fov_degrees) * 0.5)
    forward = -camera_z
    aspect = width / height
    pixel_x = (
        (-camera_x / (forward * half_height * aspect) + 1.0)
        * 0.5
        * width
    )
    pixel_y = (
        1.0 + camera_y / (forward * half_height)
    ) * 0.5 * height
    require(
        1450.0 < pixel_x < 1480.0
        and 920.0 < pixel_y < 950.0
        and forward > 0.0,
        "live ScopeFade projection fixture moved outside the viewport",
    )

    print(
        "Production safety contracts PASSED: late coherent ScopeFade replay, "
        "same-frame generation, immediate-context/device identity, SRV/RTV "
        "anti-alias guard, proxy-safe VS/GS/PS state restoration, TAA->Present "
        f"fallback, OG relocations, production Stage 4 package; fixture=({pixel_x:.2f}, "
        f"{pixel_y:.2f})."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
