# MagnaScopes

MagnaScope is an F4SE plugin for Fallout 4 1.10.163. It is derived from
[Fake Through Scope](https://github.com/ss7332337/Fake-Through-Scope) and keeps
support for existing FTS JSON profiles while adding automatic, patch-free
detection of first-person weapons set up for See Through Scopes.

The effect magnifies a copy of the rendered frame inside the projected scope
lens. It is screen-space rendering, not a second world camera.

## Runtime status

The current development build compiles and its automatic STS pixel shader
passes a D3D11 WARP contract test: pixels change inside the lens and remain
unchanged outside it. In-game acceptance of the final render anchors and
captured-mesh FTS path is still required before release.

Only Fallout 4 1.10.163 is supported. Other runtimes fail closed before
installing runtime-specific hooks.

## Automatic STS profiles

When no explicit `FTS_` profile applies, MagnaScope looks for `ReticleNode` or
`ScopeAiming` in the equipped first-person weapon. It derives the optical center
from that node and creates a profile keyed by:

- the weapon's source plugin and local FormID;
- the equipped scope OMOD, or a deterministic attachment-set key when no
  scope-specific OMOD can be identified.

Saving an automatic profile writes it under
`Data/F4SE/Plugins/FTS/Auto`. No NIF or ESP patch is required.

## Existing FTS profiles

Existing JSON files under `Data/F4SE/Plugins/FTS` remain supported. If several
profiles share one `FTS_` keyword, MagnaScope evaluates their additional and
animation-flavor keywords and chooses the most-specific matching entry.

## Configuration

`Data/F4SE/Plugins/MagnaScope.ini` controls defaults for newly detected scopes:

```ini
[AutoSTS]
Enabled=1
DefaultMaskDiameter=700.0
DefaultMagnification=2.0
ZoomSpread=1.5
```

Per-scope magnification, eye-box shadow, vignette, parallax, chromatic
aberration, fisheye distortion, and optional FOV/camera overrides are edited
through F4SE Menu Framework. Overrides are applied to an instance-local copy of
the equipped weapon's zoom data and are removed before save serialization or
when the weapon/profile is deselected.

## Build and offline test

From a Visual Studio developer environment:

```powershell
xmake build MagnaScope
xmake build AutoSTSShaderTest
.\build\tests\AutoSTSShaderTest.exe
```

The MO2-ready runtime tree is staged in `Package/MagnaScope`.

## In-game acceptance checklist

A release build must be visibly checked in Fallout 4 1.10.163:

1. Automatic STS weapon: lens-only magnification and every optical effect.
2. Explicit FTS-profile weapon: captured lens mesh and final composite.
3. TAA on and off, plus the active ENB/upscaler/frame-generation setup.
4. Profile save, reload, unsaved revert, weapon switch, and scope OMOD switch.
5. Live edit while aimed, with no stuck input after closing the menu.
6. No black lens, invisible composite, state leakage, crash, or persistent
   mutation of shared weapon zoom data.
