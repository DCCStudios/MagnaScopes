# StsConverter

Converts a non-STS Fallout 4 scope NIF into the See Through Scopes node layout,
so it can be used by STS and by the MagnaScope F4SE plugin.

## Reticle route status (in-game verdicts, 2026-08-15)

- **`--keep-reticle-material` — CONFIRMED WORKING.** The reticle keeps the
  source mod's own BGEM and renders exactly as it did before conversion. This
  is the recommended route. STS's built-in reticle swap stays inert on these
  scopes; MagnaScope's own reticle switching covers that feature instead.
- **Custom missing-material route (`--reticle-texture` / `--materials`) —
  BROKEN in game.** Renders a solid black square: the missing-material
  fallback applies STS-style alpha blending to a texture that was authored
  against the mod's own BGEM blend recipe. Fixing it means parsing the BGEM's
  blend/alpha fields (blendState/blendFunc1/blendFunc2/alphaTest, NiAlpha
  enum values) and replicating them on the NIF shader.
- **`--reticle-preset` (STS Defaults materials) — untested in game.** Passes
  every structural check; renders STS's own crosshair rather than the mod's.

Built on NiflySharp (NuGet `Nifly` 1.0.0), targeting `net8.0`. It is a separate
project from `tests\NifInspector`, which is untouched.

## Build and run

```
cd "E:\Fallout 4 Modding\F4SE\MagnaScopes\tests\StsConverter"
dotnet build
dotnet run -- --help
```

## Commands

### inspect

```
dotnet run -- inspect <input.nif>
```

Lists every shape with block index, name, parent, triangle count, world-space
bounds centre and radius, plus a heuristic `GLASS?` / `RETICLE?` guess and a
per-shape aperture measurement. Ends with a ready-to-paste `convert` command.

### convert

```
dotnet run -- convert <input.nif> --glass <name|#index> --reticle <name|#index> --out <output.nif> [options]
```

Builds the STS tree, reparents the meshes into it, generates `ScopeFade:0`,
writes the output, then reloads the output and verifies numerically that every
mesh kept its world transform. Exit code is non-zero if verification fails.

Selections accept either a shape name (`Glass:0`) or the index printed by
`inspect` (`#16`). If `--glass`, `--reticle` or `--out` are omitted and the
session is interactive, it lists the shapes and prompts; the argument form is
what matters for scripting.

Key options (`--help` lists them all):

| option | meaning |
|---|---|
| `--dot <sel>` | second reticle element, renamed to `Dot:0` |
| `--fade-from <sel>` | measure the aperture from a different shape than `--glass` |
| `--fade-scale <r>` | fraction of the measured aperture radius; default 0.94 |
| `--fade-radius <r>` | absolute world outer radius, overrides the measurement |
| `--rename-fade` | fallback: rename the chosen glass to `ScopeFade:0` instead of generating one |
| `--no-scope-fade` | do not create a `ScopeFade:0` at all |
| `--hide-when-aiming a,b` | subtrees that belong to the hip model only |
| `--no-duplicate` | do not clone the model into `ScopeNormal` |
| `--reticle-preset <name>` | recommended: use a reticle set STS ships in `Materials\Scope\Defaults\` (see `presets`) |
| `--reticle-texture <path>` | custom route: repoint the material at the non-existent `ReticleCrossCustom` and fall back to this texture |
| `--dot-texture <path>` | the same for `Dot:0` |
| `--keep-reticle-material` | force-keep the reticle's real material even if a preset or texture was given |
| `--flat-viewparts` | give `ScopeViewParts` an identity transform instead of the STS template offset |
| `--force` | convert a file that already has STS nodes |

### verify

```
dotnet run -- verify <input.nif> <output.nif>
```

Recomputes every shape's root-relative transform in both files and reports the
maximum translation, rotation, scale and bounds-centre error. Run automatically
at the end of `convert`; exposed separately so a file can be re-checked later.

### dump

```
dotnet run -- dump <nif> [<nif>...]
```

The reconnaissance tool the appendix below was derived from: full node
tree with child ordering, transforms, extra data, shader and alpha properties,
controller sequences with their bool keys, and annulus statistics for optical
geometry.

## What convert produces

```
root  (unchanged: name, BSXFlags, connect points, collision; gains a ControlManager)
+-- ScopeNormal   flags 0x0E, NiVisController      <- a clone of the whole model,
|                                                     names suffixed "_full"
+-- ScopeAiming   flags 0x0F, NiVisController      <- the original meshes
      +-- ScopeViewParts   local (0.004677, -18.882536, 1.844049)
      |     +-- ScopeFade:0        generated 24-segment annulus
      |     +-- Adjustments        local = exact inverse of ScopeViewParts (LAST child)
      |           +-- TextureLoader:0   scale 0, holds the reticle/dot textures
      |           +-- Reticle:0
      |           +-- Dot:0             only with --dot
      +-- the rest of the original tree
```

Plus a `NiControllerManager`, a `NiDefaultAVObjectPalette`, two
`NiVisController`s and the five sequences `scopeInit`, `scopeStartAiming`,
`scopeRechamber`, `scopeAiming`, `scopeFire`, matching the reference scopes
field for field.

Input files are never modified; `convert` refuses to write over its own input.

## Limitations, read these

1. **The aiming model is not actually cut down.** STS needs a second model with
   the rear lens removed so the player can see through it. That is a modelling
   job. The converter duplicates the full model into both branches so the file
   is structurally complete and nothing disappears in game, and leaves you to
   delete the occluding geometry from the `ScopeAiming` branch in NifSkope or
   Outfit Studio. Nothing automates that.

2. **ScopeFade size and position are a heuristic, not a derivation.** The
   *geometry* is exact - see the appendix. Where to put it and how big to make it is
   author-tuned in every reference scope (0.88x to 0.98x the ocular lens
   radius, no consistent rule), so the converter measures the chosen glass
   mesh's rear opening and applies `--fade-scale`, default 0.94. Check it in
   NifSkope and adjust with `--fade-scale` or `--fade-radius`. The measurement
   has two modes and reports which it used: a mesh that is already a flat disc
   facing along the sight axis is taken as the aperture directly; a thick lens
   body is measured at its rear end along its principal axis.

3. **The rearward direction is assumed to be -Y**, the Fallout 4 first-person
   weapon convention, matching the reference `ScopeFade` vertex normals of
   `(0,-1,0)`. A scope authored on a different axis will get a wrongly oriented
   fade.

4. **Clone names differ from the originals.** NIF node names must be unique, so
   the `ScopeNormal` copies are suffixed `_full`. Any material swap that targets
   a shape by name will apply to the aiming copy but not the hip copy. The
   reference STS scopes have the same situation (`acogFull` vs `acogSTS:1`).

5. **No `Dot:0` is created unless you pass `--dot`.** Every reference scope has
   one; STS's dot-reticle swap has nothing to act on without it.

6. **The reticle has three routes. Read this before shipping a conversion.**

   | Route | How | Result |
   |---|---|---|
   | **From material** (preferred) | `--materials <folder>` | Reads the reticle's and dot's real textures out of their `.BGEM` materials and retains them, then enables the swap trick. The scope keeps its own reticle. |
   | **Preset** | `--reticle-preset <name>` | Points `Reticle:0`/`Dot:0` at a set STS ships in `Materials\Scope\Defaults\`. The material exists, so nothing rests on an engine quirk. `StsConverter presets` lists all 31. |
   | **Manual** | `--reticle-texture <path>` | Same trick, with a path you supply and have verified. |
   | **Keep** (default) | nothing | The reticle keeps its own material and renders exactly as before conversion. STS reticle swapping is off. |

   Precedence: preset, then manual texture, then material, then keep. The
   material route is the one to reach for — it is the only one that preserves
   the scope's authored reticle without you having to know anything.

   **An earlier build did the Custom route by default, and it produced solid
   red reticles in game.** The reason is worth stating, because it is a trap
   for anything that edits these meshes: in a non-STS scope the reticle's real
   texture is named inside its `.BGEM` material, and the NIF's own Source
   Texture field is *ignored by the engine while that material exists*. It is
   therefore routinely stale — one test mesh carried an MK18 material path and
   an RU556 texture path in a single M4A1 shader property. Deleting the
   material promotes that dead field to authoritative, nothing loads, and the
   reticle renders as a flat emissive quad that floods the optic once
   magnified.

   The real path *can* be recovered — it is inside the `.BGEM` — which is why
   `--materials` is the preferred route. Point it at the mod's `Materials`
   folder (or the `Data` folder above it) and the converter reads each
   reticle's diffuse texture straight out of the material the engine actually
   uses. On the five test scopes this resolved to
   `Textures\Weapons\MK18\Attachments\Effects\ReticleACOG.dds`,
   `...\ReticleEXPS3.dds` and `...\ReticleSpecterDR.dds` respectively, each
   correct for its own optic.

   The material file is read by signature scan rather than a field-by-field
   header parse: the header layout shifts between material versions (the test
   corpus alone has BGEM v1 and v2) while the string encoding — uint32 length,
   printable bytes, NUL terminator — does not. Texture paths inside a material
   are relative to `Data\Textures`, so `Textures\` is prefixed when writing
   them into the shader property.

7. **Nothing here has been tested in game.** Every claim in this README about
   structure and transforms is verified numerically against the files; whether
   Fallout 4 and STS accept the result is unproven.

8. `BSConnectPoint::Parents` / `BSConnectPoint::Children` are opaque to
   NiflySharp 1.0.0. They round-trip byte-identically (checked on all five test
   inputs) but are not interpreted, so a connect point that referred to a node
   by name is not updated if that node is renamed.

## Verification results

All five test conversions, default settings apart from the magnifier's
`--fade-from`:

| file | shapes | max translation error (units) | max rotation error (deg) | max scale error |
|---|---|---|---|---|
| M4A1_Sight_ACOG | 9 | 5.68e-14 | 0 | 0 |
| M4A1_Sight_EXPS3_Magnifier | 8 | 5.68e-14 | 0 | 0 |
| M4A1_Sight_SpectreDR | 7 | 5.68e-14 | 0 | 0 |
| MK18_Sight_ACOG | 8 | 0 | 0 | 0 |
| RU556_Sight_ACOG | 9 | 1.14e-13 | 0 | 0 |

The hip-model clones were verified separately against the same reference
snapshot and produced the same figures. All errors are at double-precision
round-off level: the reparenting is exact.

---

# Appendix: the STS layout as measured from the reference files

Everything below was read out of the six reference NIFs with
`StsConverter dump`, not taken from the STS documentation. Where the files
disagree with the docs, or with each other, it is called out.

Files: `AcogSTS`, `AimpointSTS`, `ElcanSpectreSTS`, `SPRAcogSTS`,
`SPROpticalSightSTS`, `SpecialOpticSTS_1` (all FO4, 20.2.0.7 / user 12 /
stream 130).

## A1. Hierarchy

Common to five of the six (`SpecialOpticSTS_1` is the outlier, see A1.1):

```
0 NiNode "HuntingScope"                  flags 0x0000000E
    BSXFlags "BSX" = 194
    2 x NiUnknown (BSConnectPoint::Parents, BSConnectPoint::Children)
    Controller -> NiControllerManager
    CollisionObject -> bhkNPCollisionObject
  |
  +-- child 0: NiNode "ScopeNormal"      flags 0x0000000E   local IDENTITY
  |       Controller -> NiVisController
  |     +-- the full hip model, one BSTriShape per part
  |
  +-- child 1: NiNode "ScopeAiming"      flags 0x0000000F   local IDENTITY
  |       Controller -> NiVisController
  |     +-- child 0: NiNode "ScopeViewParts"    flags 0x0000000E
  |     |       local translation (0.004677, -18.882536, 1.844049), scale 1
  |     |     +-- child 0: BSTriShape "ScopeFade:0"
  |     |     +-- child 1 (LAST): NiNode "Adjustments"   flags 0x0008000E
  |     |             local translation (-0.004677, 18.882536, -1.844049)
  |     |             = the exact inverse of ScopeViewParts
  |     |           +-- BSTriShape "TextureLoader:0"   (node scale 0)
  |     |           +-- BSTriShape "Reticle:0"
  |     |           +-- BSTriShape "Dot:0"
  |     |           +-- (optionally more scope parts)
  |     +-- child 1..n: the cut-down see-through model
  |
  +-- child 2..n: shapes visible in BOTH states (iron sights, front sight)
```

Where this differs from the documentation:

* **No node called `ReticleNode` exists in any reference file.** The node
  holding `Reticle:0` and `Dot:0` is called **`Adjustments`** in all five files
  that have one. The converter uses that name.
* `ScopeAiming` has flag bit 0 set (0x0F vs 0x0E), i.e. it starts hidden;
  `ScopeNormal` starts visible. The sequences swap them.
* `ScopeNormal` / `ScopeAiming` are not the only root children. Shapes visible
  in both states hang directly off the root.
* `ScopeViewParts` carries a non-identity translation and `Adjustments` carries
  its exact negation, so the pair cancels. The value
  `(0.004677, -18.882536, 1.844049)` is byte-identical across five files, which
  says "shared template", not "engine requirement". `SpecialOpticSTS_1` uses
  `(-0.22, -6.87, 6.60)` at scale 0.655 with no `Adjustments` node at all,
  proving it is not a constant the game depends on.

### A1.1 SpecialOpticSTS_1 is different

* Root is `Scene Root`, not `HuntingScope`; it has an extra `ScopeAlways` node.
* `ScopeViewParts` holds `ScopeFade:0`, `ScopeViewParts:104`,
  `ScopeViewParts:107` and **no `Adjustments`, `TextureLoader:0`, `Reticle:0`
  or `Dot:0`** - it has no STS customisable reticle at all.
* Its `NiDefaultAVObjectPalette` still has an entry literally named
  `"HuntingScope"` pointing at the node actually named `Scene Root`. Either the
  palette name is not used for lookup, or this is a latent bug nobody noticed.

## A2. Child ordering

The docs say `TextureLoader:0` must be the first child of `ScopeViewParts` and
the reticle node must be last. What the files actually do:

| file | ScopeViewParts children | Adjustments children |
|---|---|---|
| AcogSTS | `ScopeFade:0`, `Adjustments` | `TextureLoader:0`, `Reticle:0`, `Dot:0`, `HardAlpha2`, `SoftAlpha1`, `acogPiece` |
| SPRAcogSTS | `ScopeFade:0`, `Adjustments` | same as AcogSTS |
| ElcanSpectreSTS | `ScopeFade:0`, `Adjustments` | `TextureLoader:0`, `Reticle:0`, `Dot:0`, `ELCAN_LP_001:2_LensSingle` |
| SPROpticalSightSTS | `ScopeFade:0`, `Adjustments` | `TextureLoader:0`, `Reticle:0`, `Dot:0`, `Glass2` |
| AimpointSTS | `ScopeFade:0`, `Adjustments` | **`TritiumDot1`**, `TextureLoader:0`, `Reticle:0`, `Dot:0` |
| SpecialOpticSTS_1 | `ScopeFade:0`, `ScopeViewParts:104`, `ScopeViewParts:107` | n/a |

* `TextureLoader:0` is never a child of `ScopeViewParts` - it is a child of
  `Adjustments`. Read charitably the doc rule means "first child of the node
  holding the reticles", and 4 of 5 obey it. **AimpointSTS breaks it** and
  evidently still works.
* The reticle is never last: `Reticle:0` is always followed by `Dot:0` and
  often more. What *is* consistently last is the `Adjustments` node itself, as
  the last child of `ScopeViewParts`. That is the rule the converter enforces.
* `ScopeFade:0` is child 0 of `ScopeViewParts` in all six files.

## A3. ControlManager and the sequences

Identical across all six files apart from block indices:

* One `NiControllerManager` on the root: `Flags 0x004C` (cycle CLAMP, active,
  compute scaled time), `Frequency 1`, `StartTime +FLT_MAX`,
  `StopTime -FLT_MAX`, `Cumulative false`, `Target -> root`.
* One `NiVisController` per model node: `Flags 0x006C` (as above plus
  manager-controlled), each with its own `NiBlendBoolInterpolator`
  (`ManagerControlled`, `ArraySize 2`, `SingleIndex 255`, priorities -128,
  times/weights -FLT_MAX).
* One `NiDefaultAVObjectPalette` with exactly three entries: root,
  `ScopeNormal`, `ScopeAiming`.
* **Five** `NiControllerSequence` blocks, each with its own
  `NiTextKeyExtraData` (`"start"` at 0, `"end"` at the stop time),
  `CycleType CYCLE_CLAMP`, `Weight 1`, `AccumRootName = <root node name>`,
  `AccumFlags ACCUM_X_FRONT`, and exactly two `ControlledBlock` entries
  (`NodeName` ScopeNormal / ScopeAiming, `ControllerType "NiVisController"`,
  everything else empty).

Model switching is expressed purely as visibility keys; there is no geometry
swapping. `NiBoolData` interpolation is `CONST_KEY` throughout.

| sequence | stop | ScopeNormal keys | ScopeAiming keys |
|---|---|---|---|
| `scopeInit` | 1.0 | (0,1) (1,1) | (0,0) (1,0) |
| `scopeStartAiming` | 0.15 | (0,1) (0.13,0) (0.15,0) | (0,0) (0.13,1) (0.15,1) |
| `scopeRechamber` | 2.0 | (0,0) (2,0) | (0,1) (2,1) |
| `scopeAiming` | 0.01 | (0,0) (0.01,0) | (0,1) (0.01,1) |
| `scopeFire` | 0.15 | (0,0) (0.15,0) | (0,1) (0.15,1) |

**There is no `scopeStopAiming` sequence in any of the six files.** The five
above are what STS actually ships.

Because the scaffold is entirely index-driven and otherwise identical
everywhere, the converter **synthesises** it rather than copying it from a
reference file; copying would mean shipping a reference NIF and rewriting all
its block references anyway.

## A4. ScopeFade:0 geometry

**Identical in all six files** - a shared canonical asset. Only the node's
local translation and uniform scale differ per scope.

| property | value |
|---|---|
| vertices / triangles / indices | 48 / 48 / 144 |
| vertex descriptor | `0x0001B00000430205` |
| attributes | Vertex, UVs, Normals, Tangents (27) |
| vertex stride | **20 bytes** |
| dataSize | 1248 = 48x20 + 48x6 |
| full precision | no (half-float positions) |
| segments | 24, 15-degree steps |
| outer radius | 0.98730 (quantised spread 0.98677-0.98730) |
| inner radius | 0.49072 (quantised spread 0.49072-0.49097) |
| inner:outer ratio | **0.4970** (extreme-bucket comparison gives 0.4973) |
| plane / facing | local XZ, thickness 4.8e-5; faces local -Y (all 48 triangles wind negative against +Y, vertex normals (0,-1,0)) |
| bounds | centre (0,0,0), radius 0.98707 |

This confirms MagnaScope's stated requirement exactly, including the ~0.497
ratio it measured independently.

Vertex ordering is not the obvious one and matters for an exact-replay path:

```
index 0  = outer ring, segment 0      index 3    = inner ring, segment 0
index 1  = inner ring, segment 1      index 2    = outer ring, segment 1
index 2k = inner ring, segment k      index 2k+1 = outer ring, segment k   (k >= 2)
```

Triangles for segment k = 1..24 (wrapping at 24 to segment 0):

```
(outer[k-1], inner[k],   outer[k])
(outer[k-1], inner[k-1], inner[k])
```

which reproduces the file's list exactly, including the wrap pair `(47,3,0)`
and `(47,46,3)`.

UVs map to a ring centred at `(0.5012, 0.49898)`, UV outer radius 0.4427, UV
inner radius 0.2747, with the UV angle running *backwards* from the geometric
angle at a +97.5 degree offset (90 plus half a segment). The UV inner:outer
ratio is 0.6205, deliberately different from the geometric 0.497 - the
`ScopeRadiusCircle` texture's fade gradient depends on it, so it is reproduced
rather than recomputed.

Shader, identical in all six:

* `BSEffectShaderProperty`, Name `Materials\Scope\LensFadeEffect.BGSM.BGEM`
* Source Texture `textures\Scopes\ScopeRadiusCircle.dds`, all others empty
* FO4 flags1 `0xA0000000` (ZBuffer_Test + External_Emittance), flags2 0
* `lightingInfluence` 128, `textureClampMode` 3, `baseColor` (1,1,1,1),
  `baseColorScale` 1, `softFalloffDepth` 100, all four falloff values 0
* `NiAlphaProperty` flags `0x10ED`, threshold 64

Per-file node transforms (local, under `ScopeViewParts`; rotation identity in
every case):

| file | translation | scale | world outer radius |
|---|---|---|---|
| AcogSTS | (-0.010, 33.300, 8.540) | 0.90 | 0.888 |
| SPRAcogSTS | (-0.010, 33.300, 8.540) | 0.90 | 0.888 |
| AimpointSTS | (-0.010, 35.120, 8.260) | 0.92 | 0.908 |
| ElcanSpectreSTS | (-0.010, 32.820, 8.210) | 1.00 | 0.987 |
| SPROpticalSightSTS | (-0.030, 34.660, 7.415) | 0.75 | 0.740 |
| SpecialOpticSTS_1 | (0.310, 13.700, 7.700) | 1.50 | 1.481 |

`ScopeFade:0` sits on the **ocular (rear, eye-side) lens**, not the reticle
plane. In AcogSTS its world position `(-0.005, 14.417, 10.384)` coincides with
the rear lens mesh `SoftAlpha1` at `(-0.001, 14.477, 10.387)`, in-plane radius
0.9425 - the fade is 0.94x that. Across the corpus the ratio runs 0.88 to 0.98.
**The exact figure is author-tuned and not derivable.**

## A5. Glass, Reticle, Dot and the TextureLoader trick

**There is no shape called `Glass:0` in any reference file.** Lens meshes keep
scope-specific names (`CompM4_Lens001_STS`, `ELCAN_LP_001:Lens`,
`Glass1_STS` / `Glass2_STS` / `Glass2`, `SoftAlpha1`). So `Glass:0` is not part
of the STS spec, and `ScopeFade:0` is definitively a **different object** from
the lens, not a renamed one.

`Reticle:0`, `Dot:0` and `TextureLoader:0` are also shared canonical assets: in
every file that has them, `Reticle:0` and `TextureLoader:0` are the same
9-vertex / 8-triangle mesh with identical local bounds (min
`(-1.07129, -0.00766, -0.99902)`, max `(1.07129, 0.00766, 0.99902)`), and
`Dot:0` is that mesh at about a fifth the size. Only the node transforms differ.

| shape | shader Name (material) | Source Texture | Normal Texture |
|---|---|---|---|
| `Reticle:0` | `Materials\Scope\ReticleCrossCustom.BGSM.BGEM` (does not exist) | the reticle DDS | empty |
| `Dot:0` | `Materials\Scope\ReticleDotCustom.BGSM.BGEM` (does not exist) | the dot DDS | empty |
| `TextureLoader:0` | empty string | the reticle DDS | the dot DDS |

`TextureLoader:0` has **node scale 0**, so it renders nothing. Its only job is
to make the engine resident-load those two textures, so the deliberately
unloadable Custom.BGSM.BGEM materials on `Reticle:0` and `Dot:0` fall back to
their own Source Texture. Naming a fake material rather than none leaves a
material-swap target in place, which is how STS implements reticle
customisation. Confirmed exactly as documented.

Other observed values (AcogSTS): `Reticle:0` FO4 flags1 `0x80000040`
(ZBuffer_Test + Use_Falloff), `baseColorScale` 0.1, `lightingInfluence` 255,
alpha threshold 32. `Dot:0` `baseColorScale` 6.5, flags2 ZBuffer_Write, alpha
threshold 32. `TextureLoader:0` `baseColorScale` 0, `lightingInfluence` 255,
alpha threshold 0.

## A6. Library and format notes

* **Unknown blocks.** All six reference files and all five conversion inputs
  contain `BSConnectPoint::Parents` / `BSConnectPoint::Children`, which
  NiflySharp 1.0.0 parses as opaque `NiUnknown` blobs. A load/save round trip
  of all five conversion inputs is **byte identical**, so they survive
  rewriting untouched.
* **NiflySharp dataSize bug.** `BSTriShape.CalcDataSizes` computes
  `vertexSize * vertexCount + triangleCount`. Every shape in every real file
  stores `vertexSize * vertexCount + 6 * triangleCount` (1248 for ScopeFade,
  228 for the 9-vertex reticle). The converter writes the correct value for
  generated geometry and asserts it.
* **NiflySharp BSTriShape constructor** leaves the `Skinned` vertex attribute
  set, giving a 32-byte stride. It must be forced back to
  Vertex+UVs+Normals+Tangents to reach the reference 20-byte stride.
* **NiflySharp `BSEffectShaderProperty`** parses all its texture paths but
  exposes none of them publicly in 1.0.0; the converter reads and writes the
  backing fields by reflection.
* **Transform convention, verified not assumed.** `Matrix33` documents its
  fields as "Member row,column", and a runtime probe confirmed NiflySharp
  copies `M11/M12/M13` into `MatTransform.Rotation.Rows[0]`, so `Rows[i]` is
  matrix row i. nifly applies a `Matrix3` to a vector as
  `(rows[0].v, rows[1].v, rows[2].v)`. A NIF local-to-parent transform is
  therefore `p_parent = translation + scale * (rotation * p_local)`, giving
  composition `(t1 + s1*R1*t2, R1*R2, s1*s2)` and inverse
  `(-(1/s)*R^T*t, R^T, 1/s)`. That is what `Transform.cs` implements.

