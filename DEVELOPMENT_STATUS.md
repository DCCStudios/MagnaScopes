# MagnaScope Development Status

Date: 2026-08-03

This document records the current state of MagnaScope, the work completed so far, the approaches that did not hold up in game, and the remaining work. Development is paused after this document. No build or deployment was performed while writing it.

## Project goal

MagnaScope is intended to be a low-configuration companion to See Through Scopes for Fallout 4 OG 1.10.163. It should:

- detect existing STS scopes without NIF or ESP patches;
- use the authored `ScopeFade` geometry as the exact optical aperture;
- magnify only the scene visible through that aperture while leaving the surrounding view unzoomed;
- preserve the STS reticle as an independent optical layer;
- provide believable eye-box shadow, vignette, parallax, refraction, chromatic aberration, fisheye distortion, and image cleanup;
- key automatic settings to the equipped weapon and scope attachment identity;
- support live editing and reliable save, reload, preview, and revert behavior;
- support TAA and non-TAA render paths where the engine and installed render stack permit it;
- fail safely on unsupported NG or AE runtimes.

MagnaScope no longer needs compatibility with an installed Fake Through Scope plugin or its legacy data paths. Automatic profiles belong under `Data/F4SE/Plugins/MagnaScope/Auto`.

## Current in-game result

The latest in-game test confirms that the optical shader appears again after moving the world-source capture to the exact first-person batch boundary.

### Scope shadow: cause proven and repaired offline (awaiting in-game confirmation)

The blocking shadow defect has a proven mathematical cause. It is **not** a coordinate mismatch:

- The exit pupil was displaced by `eyeTravelLens * shadowDepth` with **no bound relative to its own radius**, while the lit disc radius was fixed at approximately 1.0.
- Published eye travel is bounded only by Eye Box Max Travel, whose default permits **four aperture radii**.
- Any travel past roughly 1.16 therefore moved the entire aperture — including the aligned centre — outside the pupil disc, and the shader multiplied every optical pixel by zero. That is exactly the reported "darkens most or all of the optical image."
- `SCOPE_EYEBOX_RADIUS` (exit-pupil forgiveness) was read by the reticle shader but **never by the scene shader**, so it was a dead control. That is why adjusting shadow settings produced weak, absent, or globally dark results rather than a distinct exit pupil.
- The angular-lag term compounded this: its impulse was a raw screen-delta-to-aperture-radius ratio, which settles at roughly two to three aperture radii during an ordinary brisk ADS pan.

The repair introduces one shared contract, `src/HLSL/ScopeShadow.hlsli`, consumed by both the magnified scene replay and the late reticle composite, with two structural invariants:

1. The resting rim starts no lower than 0.75 radii, so it cannot reach the centre at any Vignette Reach.
2. The exit-pupil displacement is soft-limited to strictly less than `pupilRadius - feather`, so the moving crescent is **exactly zero at the aligned lens centre for every publishable eye travel**. The limit is derived from the same radius and feather the mask uses, so the invariant survives retuning.

The physical model now matches the parameters real scope shaders expose (3D Shader Scopes' `s3ds_eye_relief` and `s3ds_exit_pupil`): eye relief multiplies the displacement, exit-pupil forgiveness divides it.

Verified offline, not yet in game:

- `ScopeGeometryFillShaderTest` renders the production shader on WARP and asserts the aligned centre stays at full brightness under a raw displacement of 40 aperture radii, that the rim spares everything inside 0.75, that displacement produces a one-sided crescent, and that Eye Box Radius and Shadow Depth are both live controls.
- `ReticleLayerShaderTest` asserts the aligned reticle survives maximum travel and still vanishes inside a genuine crescent.
- Both static verifiers pin the new invariants and forbid the retired unbounded forms.

This remains offline evidence. A WARP harness is not proof of correct in-game optics.

Several features have worked in at least one prior in-game build, but they must be reconfirmed together in a final candidate because later changes caused regressions:

- STS scope detection and automatic profile creation;
- authored FOV and camera override handling;
- exact `ScopeFade` geometry replay, including fabrication of its missing center;
- magnification that remains visible while firing;
- independent late reticle rendering with stable size controls;
- reticle clipping resistance at steep pitch;
- live profile editing and saving;
- direct control over fisheye, refraction, sharpening, pupil, parallax, and reticle values.

Known unresolved or insufficiently verified behavior includes:

- weapon foreground geometry entering the magnified source, especially at steep upward or downward pitch;
- eye-box movement that can be weak, heading-dependent, reversed, or insufficiently localized to the optic;
- optical image and exit-pupil separation not yet producing a convincing long-tube depth effect;
- smooth ADS activation and deactivation across all scopes;
- consistent behavior across TAA, non-TAA, ENB, upscalers, and frame generation;
- crash-free save loading, repeated ADS, firing, weapon switching, and menu editing across a broader scope set.

## Current architecture

### Scope discovery and identity

- Automatic STS discovery uses the first-person `ScopeAiming` hierarchy and its `ScopeFade` and reticle descendants.
- Profiles are associated with the weapon and an equipped scope OMOD or equivalent attachment identity.
- New automatic profiles use authored zoom and camera data as their baseline.
- Scene magnification and reticle scale default to 1.0 for new scopes.

### Aperture rendering

- The live `ScopeFade` draw is treated as the authoritative aperture rather than a generic centered circle.
- The geometry shader can fill the missing center of annular `ScopeFade` meshes.
- Exact draw identity and buffer offsets are required. Unsafe approximate matches must fail closed.
- The aperture geometry owns coverage and depth. Scene magnification, eye-box shadow, and reticle rendering are separate logical layers.

### Scene source and composition

- The magnified image is a screen-space resample of a captured scene texture, not a second world camera.
- The latest change captures RT4 at the authoritative pre-first-person `RenderBatches` boundary.
- Strict draw-time capture remains as a fallback.
- This boundary is intended to provide the completed world before first-person weapon geometry is drawn.
- The result is replayed through the exact `ScopeFade` geometry and composited at a verified late render anchor.

### Reticle

- The STS reticle is captured and rendered in a separate late pass.
- Reticle size, offset, magnification, parallax, and shadow participation have dedicated settings.
- The reticle should move with the optical lens while keeping its own configured scale.
- The reticle should not be distorted by scene fisheye, refraction, chromatic aberration, or image sharpening.

### Editing and persistence

- The F4SE Menu Framework panel exposes automatic scope settings and per-profile overrides.
- The popout supports mouse capture for editing while the main menu is closed.
- Profile save and reload use the MagnaScope namespace.
- Authored zoom data can be restored as an editing baseline.

## Work completed

1. Renamed and restructured the project as MagnaScope while preserving Fallout 4 OG behavior and safe unsupported-runtime handling.
2. Established a repeatable native build, shader build, package staging, and LoreOut deployment workflow.
3. Added automatic STS scope detection and per-attachment profile identity.
4. Added automatic profile persistence under the MagnaScope data path.
5. Reworked zoom and camera override timing so the game can sample the requested values before aim-in.
6. Added exact `ScopeFade` draw discovery and fail-closed replay validation.
7. Added center geometry fabrication for annular STS aperture meshes.
8. Stabilized aperture persistence through recoil and firing.
9. Implemented magnification, fisheye, edge refraction, chromatic aberration, cleanup, and sharpening controls.
10. Added separate reticle capture, composition, size, offset, magnification, and parallax controls.
11. Added configurable eye-box, vignette, scene parallax, optical lag, and depth-related controls.
12. Added smooth recentering work intended to eliminate snapping and heading-dependent drift.
13. Moved the world-source capture toward the exact pre-first-person boundary to avoid magnifying weapon foreground geometry.
14. Added deterministic shader and safety harnesses for aperture fill, reticle composition, eye-box behavior, and static contract checks.

## Approaches that did not hold up

These approaches should not be restored without new evidence:

- A screen-centered or bounds-derived circular mask. STS authors may align an optic away from screen center, and `ScopeFade` is the guaranteed aperture contract.
- Depending on an optional lens surface. Not every STS NIF contains a suitable solid lens mesh.
- Capturing the final frame after first-person rendering. This magnifies the weapon, reticle, laser, and nearby first-person geometry and produces silhouettes or projected artifacts.
- Using CPU-projected scope bounds as the authoritative aperture position. That introduced lag, off-center placement, breathing, and pitch or heading dependence.
- Driving optical lag from world-space or camera-heading-dependent axes. The sign changed with facing direction and did not behave as optic-local inertia.
- Applying the scene shader directly to the reticle. This magnified, warped, vignetted, or clipped the reticle.
- Treating a successful draw call, log line, pixel probe, build, or WARP harness as proof of correct in-game optics.
- Relaxing exact `ScopeFade` buffer-offset validation to make a candidate match. A rejected candidate can be another draw sharing the same resources.
- Repeated telemetry-only deployments. Future builds should combine a bounded diagnosis with a substantive fix whenever possible.

## Remaining work

### Priority 0: Confirm the repaired scope shadow in game

The shader contract is implemented and asserted offline. What remains is in-game
confirmation against the tester checklist below, and tuning of the authored
defaults (`parallax.radius` 1.55 as exit-pupil forgiveness, `parallax.shadowDepth`
1.0 as eye relief, `relativeFogRadius` 7.0 as rim reach, `scopeSwayAmount` 18.0 as
rim hardness) once the shape is confirmed correct.

The desired contract is:

- `ScopeFade` defines the fixed physical aperture.
- A resting rim shadow affects only the edge region and leaves the aligned center clear.
- Eye displacement creates a directional crescent from a displaced virtual exit pupil.
- The shadow moves opposite apparent weapon or camera motion, then returns smoothly to center.
- The shadow does not uniformly lower the exposure of the entire lens.
- The scene, pupil mask, resting rim, and reticle remain separate inputs until final composition.

The next investigation should inspect the actual ranges of:

- normalized lens coordinates;
- projected aperture radius;
- `restingRimShadow`;
- `directionalPupilShadow`;
- `pupilShadow`;
- activation and final optical visibility.

The likely failure area is the normalized coordinate and pupil-radius contract or the final full-color multiplication. That is a hypothesis, not a verified cause. The next fix should add deterministic shader tests proving that the aligned lens center stays unchanged, the resting rim affects only the outer band, and a displaced pupil produces a one-sided crescent.

### Priority 0: Finish clean scene-source capture

- Confirm that the pre-first-person RT4 snapshot contains the complete world and excludes the weapon at level aim, straight up, and straight down.
- Reject stale or partially rendered sources instead of displaying them.
- Keep strict fallback capture rules and never accept a convenient but unproven draw.
- Verify that firing, recoil, and first-person inertia do not replace or invalidate the source.

### Priority 0: Preserve crash safety

- Keep exact geometry and suballocation validation.
- Guard all render resources, scene nodes, weapon instance data, zoom forms, and menu-held pointers.
- Restore D3D11 state completely after every pass.
- Recheck save loading, repeated ADS, automatic fire, weapon switching, attachment changes, resize, and menu close paths.

### Priority 1: Rebuild eye-box and tube-depth behavior

- Compute motion in a stable optic-local screen basis rather than world heading.
- Keep the aperture centered at rest regardless of pitch or yaw.
- Use continuous critically damped or exponential settling with no threshold snap.
- Make lag direction, strength, recenter speed, maximum travel, pupil depth, scene depth, and axial breathing independently tunable.
- Allow the magnified image plane to move less than the rear aperture so the user perceives a long optical tube.
- Keep axial size change subtle by default and bounded so ordinary camera rotation cannot collapse the lens.
- Integrate FPGunplayOverhaul inertia without depending on that plugin or duplicating its state.

### Priority 1: Reticle completion

- Keep reticle scale independent from scene magnification.
- Move the reticle with the optical image using its own parallax scale.
- Prevent clipping at every pitch and heading.
- Keep it behind the physical shadow when configured, but do not cull it using mismatched pupil coordinates.
- Preserve authored reticle alignment and allow local size and offset correction.

### Priority 1: ADS transition and editing lifecycle

- Start the optical effect at aim-transition start.
- Blend aperture visibility, magnification, and shadow smoothly into the fully aimed state.
- Keep live editing stable while aimed, with reliable save, reload, preview, and revert behavior.
- Ensure menu input capture cannot leave the player stuck aiming or without controls.
- Prevent edits for one weapon or scope attachment from changing unrelated weapons.

### Priority 2: Compatibility and image quality

- Test TAA and non-TAA anchors separately.
- Test plain rendering, ENB, common upscalers, and frame generation.
- Add image stabilization only after the source and mask are correct. A denoiser must not hide a wrong source, ghost motion, or blur the reticle.
- Remove remaining obsolete Fake Through Scope names, paths, and compatibility code.
- Finalize user and mod-author documentation around automatic detection and optional profile overrides.

## Efficient resume plan

To avoid another long sequence of telemetry-only builds:

1. Fix the shadow mask contract and add deterministic center, rim, and displaced-pupil shader assertions.
2. In the same development cycle, verify the pre-first-person source contract at level and steep pitch.
3. Run static safety checks, shader harnesses, the native build, package staging, and artifact hash comparison.
4. Deploy one candidate after all offline stages pass.
5. Give the tester an exact visual checklist for aligned center brightness, rim shape, directional crescent, pitch behavior, firing, and reticle independence.
6. Only issue a diagnostic-only build when the result will distinguish between two concrete implementation paths.

## Final in-game acceptance matrix

The release candidate should be verified with at least two materially different STS scopes:

- level aim, straight up, straight down, and multiple compass headings;
- slow and fast camera motion;
- forward and backward player movement;
- semi-automatic and sustained automatic fire;
- ADS enter and exit repeated many times;
- weapon and scope attachment switching;
- save, reload, preview, revert, game save, and game load;
- TAA enabled and disabled;
- plain rendering and the intended ENB, upscaler, and frame-generation configurations.

Pass criteria:

- magnification is visible only through the authored aperture;
- the aligned central image is not globally darkened;
- no black lens, full-screen flash, invisible pass, triangle faceting, thin untreated ring, or weapon silhouette appears;
- the resting shadow is a controllable rim and motion creates a directional crescent;
- motion direction is optic-local and consistent at every heading;
- settling is smooth with no snap;
- the reticle keeps its configured size and remains visible without inheriting scene distortion;
- firing and recoil do not remove or reset the effect;
- no render-state leakage, stuck input, crash, or persistent weapon-data corruption occurs.

## Pause point

The repository has a large intentional dirty working tree containing the current MagnaScope restructuring and optical work. Do not reset or discard it when resuming.

- Current Git HEAD: `64a0677` (`Smooth STS eye-box recentering`)
- Latest deployed DLL SHA-256: `6B1EB1FED24E03F8991551719402A67726B47CAA131EA28C2B6CF7F06140B1E0`
- Latest deployment backup: `deployment_backups/20260803-004953`
- Current blocking visual issue: scope shadow darkens the entire optical image instead of remaining an edge and displaced-pupil effect.

Development is paused here. No code, shader, build artifact, installed profile, or deployed DLL was changed as part of this status write-up.
