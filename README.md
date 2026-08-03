# MagnaScopes

MagnaScope is an F4SE plugin for Fallout 4 1.10.163 that adds screen-space
optical magnification and scope effects to first-person weapons configured for
See Through Scopes (STS). It detects existing STS scene-graph conventions at
runtime, so weapon authors and users do not need an additional NIF or ESP
patch.

The rendered frame is resampled only through the live `ScopeFade` aperture.
The surrounding view stays unchanged, while the lens can apply magnification,
eye-box shadow, vignette, scene and reticle parallax, fisheye distortion, edge
refraction, chromatic aberration, cleanup, and sharpening. This is a
screen-space effect rather than a second world camera.

## Runtime status

Only Fallout 4 1.10.163 is supported. Other runtimes fail closed before any
runtime-specific relocation or hook is installed.

The build and shader contracts can be verified offline, but visual correctness
still requires the staged in-game checks in `VERIFICATION.md`. Successful
compilation, hook installation, or render telemetry is not proof of a correct
scope image.

## Automatic STS profiles

MagnaScope detects STS scopes from the equipped first-person weapon's
`ScopeAiming`, `ScopeFade`, and reticle geometry. A profile is keyed by:

- the weapon's source plugin and local FormID;
- the equipped scope OMOD, or a deterministic attachment-set identity when a
  single scope OMOD cannot be isolated.

Saving a profile writes it under:

`Data/F4SE/Plugins/MagnaScope/Auto`

One weapon file can contain separate settings for each equipped scope. New
profiles start from the weapon's authored FOV and sighted-camera data, with
scene magnification and reticle scale at 1x.

## Configuration and editing

`Data/F4SE/Plugins/MagnaScope.ini` controls automatic detection defaults and
the staged verification gates. `Data/F4SE/Plugins/MagnaScopeConfig.json`
stores framework-level keys.

Per-scope settings are edited through F4SE Menu Framework. The popout supports
live preview while aimed, profile save/reload, and explicit restoration of the
scope's authored zoom data. Temporary zoom and camera overrides are applied at
the correct aim lifecycle point and restored when the profile or weapon is
deselected.

## Build and offline tests

From a Visual Studio developer environment:

```powershell
xmake build MagnaScope
xmake build AutoSTSShaderTest ScopeGeometryFillShaderTest ReticleLayerShaderTest DrawTimeEyeBoxTest
```

The development DLL and matching PDB are staged under
`Compile/F4SE/Plugins`. The end-user package is staged under
`Package/MagnaScope`.

## In-game acceptance

The release candidate must be visibly checked on multiple STS optics with TAA
and non-TAA anchors, plus the active ENB/upscaler/frame-generation stack. It
must show lens-only magnification and stable optical effects without a black
lens, fullscreen flicker, reticle clipping, render-state leakage, stuck input,
crashes, or persistent mutation of unrelated weapon data.
