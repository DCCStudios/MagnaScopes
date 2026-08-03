"""Production safety contracts for MagnaScope's late ScopeFade replay.

Stage 5c's private world-render detours are intentionally not part of this
contract.  The production path follows MagnaScope's demonstrated
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
    data_h = (project / "src" / "ScopeProfile.h").read_text(encoding="utf-8")
    data_cpp = (project / "src" / "ScopeProfile.cpp").read_text(encoding="utf-8")
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
    shadow_shader = (
        project / "src" / "HLSL" / "ScopeShadow.hlsli"
    ).read_text(encoding="utf-8")
    eyebox_header = (
        project / "src" / "EyeBoxRecentering.h"
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

    # The retired nested-world renderer remains disabled, but its safe
    # two-hook pre-first-person color snapshot is linked and explicitly used.
    # This path never invokes a second Render_PreUI, Z pre-pass, or Umbra pass.
    require(
        "WorldOnlyScopeRenderer::GetSingleton().InstallHooks()" in main_cpp
        and "WorldOnlyScopeRenderer::GetSingleton()" in hooking,
        "production code does not wire the safe world-color snapshot",
    )
    require(
        'remove_files("src/WorldOnlyScopeRenderer.cpp")' not in xmake,
        "production build still excludes the safe world-color snapshot",
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
        "CaptureBeforeFirstPersonDraw(pContext)" in draw
        and "CaptureBeforeFirstPersonDraw(pContext)" in draw_instanced,
        "D3D draw hooks do not capture the verified pre-first-person color target",
    )
    require(
        "AllowsWorldColorCapture()" in draw
        and "AllowsWorldColorCapture()" in draw_instanced
        and "AllowsAuxiliaryWorldPass(" not in draw
        and "AllowsAuxiliaryWorldPass(" not in draw_instanced,
        "D3D draw hooks do not gate the safe snapshot independently of the retired auxiliary pass",
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
        "exact ScopeFade draw is not captured and color-suppressed like MagnaScope",
    )
    # Both optical passes must bind b5. Every depth and separation control the
    # magnify shader reads lives there -- ScopeSceneDepth, ScopeShadowDepth,
    # ScopeImageStillness, ScopeAxialBreathing, ScopeApertureScaleRatio -- and
    # the reticle layer reads the first two to follow the image and share the
    # exit pupil. Binding only b4 made the shaders sample whatever the game left
    # in slot 5; shadow depth reading zero collapsed the pupil displacement, so
    # no eye-box setting could produce a crescent. The WARP harnesses bind b5
    # themselves and therefore cannot catch this, which is why it survived every
    # green test run.
    reticle_composite = function_body(
        hooking,
        "bool D3D::CompositeAutomaticSTSReticleLayer(",
    )
    require(
        "PSSetConstantBuffers(5, 1, &scopeEffectBuffer)" in replay
        and "PSSetConstantBuffers(5U, 1U, &scopeEffectBuffer)"
        in reticle_composite,
        "optical passes do not bind the b5 depth/separation constants",
    )
    require(
        "PrepareScopeFadeSceneSource(" not in capture
        and "PSSetShader(" not in capture
        and "GSSetShader(" not in capture
        and "CopyResource(" in capture,
        "capture phase modifies pixels or omits private transform snapshots",
    )
    # The optical source must share one colour encoding with the composite
    # target. The retired pre-first-person RT4 snapshot was taken before
    # Fallout's image-space and tone-mapping work while the composite runs
    # against the finished display-encoded frame, and nothing converted between
    # them -- the complete optical image was uniformly darker than the scene
    # around it no matter how the shadow and image controls were set. Passing no
    # preferred source makes PrepareScopeFadeSceneSource copy the composite
    # target itself, which cannot mismatch.
    require(
        "ReplayAutomaticSTSScopeFade(\n\t\t\t\t\tnullptr," in render_aperture
        and "AcquireColorSRV(" not in render_aperture
        and "worldSource" not in render_aperture
        and "PrepareScopeFadeSceneSource(" in replay
        and "DrawIndexed(" in replay
        and "BSScopeFadeReplaceRGB.Get()" in replay,
        "optical source is not the composite target's own colour encoding",
    )
    require(
        "return false;" in function_body(
            settings,
            "bool AllowsWorldColorCapture() const noexcept",
        ),
        "the retired pre-first-person world colour capture can still run",
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

    # MagnaScope runs after the original TAA callback.  Present is the
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

    # The fabricated center apex is 2*inner - outer because that is the exact
    # screen point each wedge's own affine map sends to lens (0, 0). It gives
    # the fan the same projective frame as the annulus, so the shared edge at
    # radius 0.5 is continuous. Any other apex, including a game-thread lens
    # center, splits the frame and shows as a faceted circle partway out.
    require(
        "center.position = 2.0f * input[1].position - input[0].position;"
        in geometry_fill_shader
        and "SCOPE_LENS_CENTER_X" not in geometry_fill_shader
        and "GSSetConstantBuffers(4" not in hooking,
        "the center fan apex must keep the annulus projective frame",
    )

    # The WARP harnesses resolve Compile/Shaders from the working directory.
    # Starting them in build/tests let a stale shader tree shadow the freshly
    # built one and report failures that do not exist in the real output.
    require(
        xmake.count('set_rundir("$(projectdir)")') == 4,
        "every shader harness must run from the project directory",
    )

    # Optical effects operate on one continuous homogeneous ScopeFade field.
    # The noperspective numerator and reciprocal-W are affine in display space,
    # so the pixel shader can solve the projective center exactly. Differentiating
    # normalized perspective-correct coordinates only gives a local tangent and
    # makes the inferred center vary across triangles.
    require(
        "noperspective float3 lensProjective : TEXCOORD0"
        in geometry_fill_shader
        and "MakeProjectiveLensCoordinates" in geometry_fill_shader
        and "outerCurrentCoordinates" in geometry_fill_shader
        and "innerCurrentCoordinates" in geometry_fill_shader
        and "float2(0.0f, 0.0f)" in geometry_fill_shader
        and "nointerpolation" not in geometry_fill_shader
        and "noperspective float3 lensProjective : TEXCOORD0" in shader
        and "input.lensProjective.xy / safeReciprocalClipW"
        in shader
        and "const float2 numeratorDx = ddx(projectiveNumerator)" in shader
        and "const float2 numeratorDy = ddy(projectiveNumerator)" in shader
        and "const float reciprocalWDx = ddx(reciprocalClipW)" in shader
        and "const float reciprocalWDy = ddy(reciprocalClipW)" in shader
        and "SolvePixelOffset" in shader
        and "const float2 pixelsToCenter" in shader
        and "const float2 pixelsToUnitX" in shader
        and "const float2 pixelsToUnitZ" in shader
        and "const float2 currentAimPixels" in shader
        # The sample pivot now declines a configurable fraction of the
        # aperture's own screen motion, which is what makes the image read
        # as sitting far behind the housing. Scaling the pivot rather than
        # the delta is required: only the pivot form cancels that motion
        # identically at every magnification.
        and "const float imageStillness = saturate(ScopeImageStillness);"
        in shader
        and "aperturePivotPixels += apertureMotionPixels * imageStillness;"
        in shader
        and "const float2 samplePivotUv = aperturePivotPixels * PixelSize"
        in shader
        # Fore/aft breathing must stay independent of lateral parallax so
        # camera yaw can never read as depth.
        and "axialEyeRelief * axialBreathing" in shader
        and "1.0f - axialEyeRelief * sceneDepth" not in shader
        and "const bool exactDrawFrameValid" in shader
        and "if (!exactDrawFrameValid)" in shader
        and "tBACKBUFFER.SampleLevel(gSamLinear, screenUv, 0.0f)" in shader
        and "publishedCenterPixels" not in shader
        and "float2(SCOPE_EYE_OFFSET_X, SCOPE_EYE_OFFSET_Y)" in shader
        and "EvaluateScopeShadow(" in shader
        and "shadow.visibility" in shader
        and "SCOPE_EYE_RELIEF_DELTA" in shader
        and "const float axialSceneScale" in shader
        and "const float opticalMagnification" in shader
        and "const float axialPupilScale" in shader
        and "SCOPE_FISHEYE_STRENGTH" in shader
        and "SCOPE_EDGE_REFRACTION_STRENGTH" in shader
        and "SCOPE_EDGE_CHROMATIC_ABERRATION" in shader
        and "edgeAwareAverage" in shader,
        "shipping shader lost lens-local optics or reintroduced global flicker",
    )
    require(
        "baselineEyeReliefDistance" in main_cpp
        and "CalculateEyeReliefDistance(" in main_cpp
        and "CalculateAxialEyeRelief(" in main_cpp
        and "state.baselineEyeReliefDistance +=" in main_cpp
        and "axialEyeRelief.valid ? axialEyeRelief.normalizedDelta : 0.0F" in main_cpp
        and "A malformed or temporarily unavailable depth" in main_raw
        and "SCOPE_EYE_RELIEF_DELTA" in shader
        and "const float axialPupilScale" in shader
        # The disc handed to the shadow contract is the recessed image
        # plane, not the aperture itself: it sits toward the front of the
        # tube so a ring of wall separates it from the rear glass.
        and "const float imageDiscRadius =" in shader
        and "imageDiscRadius," in shader
        # The resting rim is computed unconditionally inside the shared
        # contract, so losing physical eye telemetry can only remove the
        # moving crescent -- never the authored tube rim.
        and "layers.restingRim = pow(" in shadow_shader
        and "if (eyeTravelValid) {" in shadow_shader
        and "layers.movingCrescent = 0.0f;" in shadow_shader
        and "max(layers.restingRim, layers.movingCrescent)" in shadow_shader
        and "physicalEyeTravelValid * activation" not in shader,
        "optional axial eye relief can still invalidate lateral travel or disable the fixed resting rim",
    )

    # Priority 0 regression guard. In-game testing showed the exit pupil
    # darkening the entire optical image. The cause was an unbounded pupil
    # displacement: published eye travel is limited only by Eye Box Max Travel
    # (four aperture radii by default) while the lit disc has radius one, so
    # any travel past roughly 1.16 pushed every pixel -- including the aligned
    # centre -- outside the pupil and multiplied the whole lens by zero.
    #
    # Two structural invariants now prevent that, and both are additionally
    # asserted against rendered WARP output by ScopeGeometryFillShaderTest:
    #   * the resting rim starts no lower than 0.75, so it cannot reach the
    #     centre at any Vignette Reach;
    #   * the pupil displacement is soft-limited to strictly less than
    #     (pupilRadius - feather), so the crescent is exactly zero at the
    #     aligned lens centre for every published travel.
    require(
        "ScopeShadowSoftLimit" in shadow_shader
        and "rsqrt(1.0f + normalized * normalized)" in shadow_shader
        and "ScopeShadowMaximumPupilOffset" in shadow_shader
        and "max(pupilRadius - pupilFeather - 0.02f, 0.0f)" in shadow_shader
        and "ScopeShadowSoftLimitVector(\n"
        "            rawOffset,\n"
        "            ScopeShadowMaximumPupilOffset(" in shadow_shader
        and "return 1.0f - 0.25f * hardnessNormalizedReach;" in shadow_shader
        # The forgiveness divisor is what makes Eye Box Radius a live control
        # in the scene shader. It was previously read only by the reticle.
        and "max(exitPupilForgiveness, 0.10f)" in shadow_shader
        and "eyeTravel * (max(eyeRelief, 0.0f) / forgiveness)" in shadow_shader
        # Both optical layers must consume the one shared contract.
        and '#include "ScopeShadow.hlsli"' in shader
        and '#include "ScopeShadow.hlsli"' in reticle_shader
        and "SCOPE_EYEBOX_RADIUS," in shader
        and "SCOPE_EYEBOX_RADIUS," in reticle_shader
        # The retired unbounded forms must not return.
        and "const float2 pupilCenter = eyeTravelLens * shadowDepth;"
        not in shader
        and "const float pupilRadius = axialPupilScale;" not in shader,
        "scope shadow can again darken the aligned optical centre or ignore exit-pupil forgiveness",
    )

    # The game thread must saturate eye travel with the same smooth shape the
    # shaders use. A hard clamp put a derivative discontinuity in the middle of
    # a fast pan, and the raw screen-impulse ratio settled at two to three
    # aperture radii during ordinary ADS panning, pinning the crescent at its
    # bound instead of tracking motion.
    require(
        "kAngularLagGain" in main_cpp
        and "SoftLimitVector(" in main_cpp
        and "inline constexpr float kAngularLagGain" in eyebox_header
        and "value / std::sqrt(1.0F + normalized * normalized)"
        in eyebox_header
        and "state.angularLagX += impulseX;" in main_cpp
        and "const float scale = maximumTravel / impulseLength;"
        not in main_cpp,
        "angular eye-box lag is unbounded, hard-clamped, or uncalibrated",
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
        "const float currentProjectedRadius =" in shader
        and "const float2 stableShadowCoordinates =" in shader
        and "normalizedLensPosition" in shader
        and "float2 eyeTravelLens =" in shader
        and "const float2 numeratorAtEye =" in shader
        and "const float reciprocalWAtEye =" in shader
        and "SCOPE_SCENE_PARALLAX_STRENGTH" in shader
        and (
            "physicalEyeTravel /" in shader
            or "const float2 depthTravel = physicalEyeTravel * sceneDepth" in shader
        )
        # Scene parallax saturates through the same shared soft limiter the
        # exit pupil uses, bounded to one aperture radius. The retired
        # 1/(1 + 2m) form removed roughly 29% of an ordinary 0.2-radius shift
        # and flattened exactly the motion that conveys optical depth.
        and "ScopeShadowSoftLimitVector(depthTravel, 1.0f)" in shader
        and "1.0f + 2.0f * sceneTravelLength" not in shader
        and "sampleDelta -=\n            eyeParallaxPixels" in shader,
        "aperture-derived shadow fit or bounded scene parallax is missing",
    )
    require(
        "float maxTravel = 4;" in data_h
        and "profile->shaderData.parallax.radius = 1.55F;" in data_cpp
        and "profile->shaderData.parallax.relativeFogRadius = 7.0F;"
        in data_cpp
        and "profile->shaderData.parallax.scopeSwayAmount = 18.0F;"
        in data_cpp
        and "profile->shaderData.parallax.maxTravel = 4.0F;" in data_cpp
        and "profile->shaderData.sceneParallaxStrength = 1.0F;" in data_cpp
        and "profile->shaderData.reticleMagnification = 1.0F;" in data_cpp
        and "profile->zoomDataOverwrite.fovMul = zoomData.fovMult;" in data_cpp
        and "float defaultMagnification = 1.0F;" in settings,
        "automatic STS eye-box, authored zoom, scene-parallax, or neutral magnification defaults regressed",
    )
    require(
        "EyeBoxRecentering::CalculateResponseAlpha(" in main_cpp
        and "kOpticalFollowerTimeConstant" in main_cpp
        and "kOpticalFollowerTimeConstant = 0.090F" in main_cpp
        and "const float followerBlend =" in main_cpp
        and "state.baselineEyeLocalX +=" in main_cpp
        and "state.baselineEyeLocalZ +=" in main_cpp
        and "ProjectLocalEyeOffsetToScreen(" in main_cpp
        and "cameraApertureLocal.x" in main_cpp
        and "cameraApertureLocal.z" in main_cpp
        and "baselineScreenX" not in main_cpp
        and "planarLength > maximumTravel" in main_cpp
        and "ResetAutomaticSTSEyeBoxTracking();" in main_cpp
        and "kRequiredStableSeconds" not in main_cpp
        and "kMaximumCalibrationVelocity" not in main_cpp
        and "boundedDeltaSeconds,\n\t\t\t0.010F" in main_raw
        and "float2(SCOPE_EYE_OFFSET_X, SCOPE_EYE_OFFSET_Y)" in shader
        and "const float currentProjectedRadius =" in shader
        and "const float2 stableShadowCoordinates =" in shader
        and "float2 eyeTravelLens =" in shader
        and "const float2 numeratorAtEye =" in shader
        and "const float reciprocalWAtEye =" in shader
        and "CalculateScopeDrawTimeEyeTravel(" not in shader,
        "eye-box motion is not self-centering in ScopeFade-local coordinates",
    )
    require(
        "previousCameraPoseReady" in main_cpp
        and "previousForwardWorld" in main_cpp
        and "currentForwardWorld" in main_cpp
        and "previousForwardScreen.x - currentForwardScreen.x" in main_cpp
        and "previousForwardScreen.y - currentForwardScreen.y" in main_cpp
        and "kAngularLagDecaySeconds = 0.055F" in main_cpp
        and "state.angularLagX *= decay" in main_cpp
        and "state.angularLagY *= decay" in main_cpp
        # Eye travel still reaches the pupil in exact ScopeFade replay
        # coordinates rather than through a CPU projection from another frame.
        and "float2 eyeTravelLens =" in shader
        and "numeratorAtEye / reciprocalWAtEye" in shader
        # Eye travel now carries the optical-tube parallax term. A
        # recessed image disc that stays concentric with the aperture
        # only looks smaller; it still tracks the housing one-for-one.
        # Two circles at different depths separate only when the eye is
        # off the optical axis, and the eye is the camera, so the axis is
        # screen centre.
        and "eyeTravelLens + tubeParallaxLens," in shader
        and "const float2 opticalAxisPixels = 0.5f * ScreenSize;" in shader
        and "const float2 tubeParallaxLens =" in shader,
        "heading-independent camera-space eye-box inertia or pupil travel regressed",
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
        and "OMSetRenderTargets(1U, &layerTarget, nullptr)"
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
        "OMSetRenderTargets(1U, &layerTarget, nullptr);" in reticle_capture
        and "mAutomaticSTSReticleLayerReadOnlyDepthState" in reticle_capture
        and "readOnlyDescription.DepthEnable = FALSE;" in reticle_capture
        and "readOnlyDescription.DepthWriteMask =\n\t\t\t\tD3D11_DEPTH_WRITE_MASK_ZERO;"
        in reticle_capture
        and "readOnlyDescription.DepthFunc = D3D11_COMPARISON_ALWAYS;"
        in reticle_capture
        and "readOnlyDescription.StencilEnable = FALSE;" in reticle_capture
        and "readOnlyDescription.StencilWriteMask = 0U;" in reticle_capture
        and "CreateDepthStencilState(" in reticle_capture
        and "OMSetDepthStencilState(\n\t\t\tmAutomaticSTSReticleLayerReadOnlyDepthState.Get(),"
        in reticle_capture,
        "paired reticle capture no longer reconstructs the complete reticle independently of live depth",
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
        "ScopeFade authority is not exact, generation-local, and capture-derived",
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
        and "ReticleCompositeOutput reticle = SampleAuthoredReticle(sourceUv);"
        in reticle_shader
        and "const float2 reticleCenterPixel = float2(" in reticle_shader
        and "SCOPE_AIM_CENTER_X" in reticle_shader
        and "SCOPE_AIM_CENTER_Y" in reticle_shader
        and "saturate(SCOPE_FADE_ACTIVATION)" in reticle_shader
        and "(outputPixel - reticleOutputPivot) / reticleScale"
        in reticle_shader
        and "SCOPE_RETICLE_SIZE" in reticle_shader
        and "SCOPE_RETICLE_OFFSET_X" in reticle_shader
        and "SCOPE_RETICLE_OFFSET_Y" in reticle_shader
        and "SCOPE_SCENE_PARALLAX_STRENGTH" in reticle_shader
        and "SCOPE_EYE_OFFSET_X" in reticle_shader
        and "reticle.sourceContribution *= reticleVisibility"
        in reticle_shader
        and "reticle.destinationTransmittance = lerp(" in reticle_shader
        and "float4(1.0f, 1.0f, 1.0f, 1.0f)" in reticle_shader
        and "SCOPE_LENS_CENTER_X" in reticle_shader
        and "SCOPE_LENS_CENTER_Y" in reticle_shader
        and "outputPixel - lensCenterPixel" in reticle_shader
        and "const float projectedRadius = max(" in reticle_shader
        # The reticle shadow is evaluated in the same basis-inverted
        # optic-local frame the scene replay uses. The retired isotropic
        # pixel-radius coordinate disagreed with the scene layer whenever the
        # optic was foreshortened or rolled, so the reticle stayed lit inside
        # a crescent the scene had already darkened.
        and "const float2 shadowLensCoordinates = lensCoordinates;"
        in reticle_shader
        and "const float2 screenLensCoordinates =" not in reticle_shader
        and "const bool physicalEyeTravelValid =" in reticle_shader
        and "if (physicalEyeTravelValid)" in reticle_shader
        and "if (projectedBasisValid && SCOPE_PHYSICAL_EYEBOX_VALID"
        not in reticle_shader
        and "ScopeFadePolygonRadius" not in reticle_shader
        and "reticleBoundary" not in reticle_shader,
        "reticle layer lost non-destructive dual-source composition, independent size/offset, stable optical motion, or shadow occlusion",
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
