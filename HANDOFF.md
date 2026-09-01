# MagnaScope handoff

Written 2026-08-04, at the end of a session that ran out of runway on repeated
build interruptions. Everything described as "uncommitted" sits on top of
`831f376`.

This document is written for someone with no prior context. It covers what the
project is trying to do, how to work on it without repeating expensive
mistakes, where the code lives, what the current investigation found, and what
to do next.

---

# 1. What MagnaScope is

An F4SE plugin for **Fallout 4 1.10.163 only** that adds optical scope effects
to first-person weapons already set up for **See Through Scopes (STS)**. It
detects STS's scene-graph conventions at runtime, so no NIF or ESP patch is
required from weapon authors or users.

The target look is **STALKER's 3D Shader Scopes** — a real optical instrument
rather than a zoom overlay:

- magnification of only what is visible through the aperture, with the
  surrounding view left unzoomed;
- a scope shadow / exit pupil that darkens the image as the eye leaves the
  optical axis, producing a moving crescent rather than a uniform dimming;
- eye-box behaviour: the lens opening shifts and vignettes as the head moves
  relative to the optic;
- optical depth, tube parallax, breathing motion, fisheye, edge refraction,
  chromatic aberration, and image cleanup.

It is a **screen-space effect, not a second world camera.** The finished frame
is resampled through the live aperture.

## The optical vocabulary

Terms recur throughout the code; getting them confused has caused real bugs.

- **Aperture** — the opening you look through. Published as a world centre and
  radius, projected to screen pixels each frame.
- **Exit pupil** — the small disc of light the eyepiece projects toward your
  eye. Moving off it darkens the image from one side (the crescent).
- **Eye box** — the volume within which your eye still sees a full image.
- **Eye relief** — distance from eye to eyepiece. Multiplies pupil
  displacement.
- **Reticle** — the aim mark. It is an **independent layer**, isolated from the
  optical scene and composited afterward, so it never gets magnified or
  distorted with the image.
- **ScopeFade** — STS's authored aperture mesh. A standardised 24-segment
  annulus: 48 vertices, 48 triangles, 144 indices, stride 20.

---

# 2. Philosophy — how to work on this codebase

This section matters more than the architecture map. The recurring failures on
this project have not been coding errors; they have been **reasoning errors
that looked correct**.

## 2.1 Measure before you change. Then measure again.

The single most expensive pattern in this project's history is: form a
plausible theory, implement it, ship it, discover it was wrong, repeat. Several
times a value was tuned upward twice before anyone measured whether the input
signal existed at all — it didn't, and both changes were wasted.

Before changing a constant, **log the quantity it operates on.** Before
claiming a path executes, **log that it executed.** The codebase has extensive
one-shot and rate-limited logging for exactly this reason; add to it freely.

A recent example, in this session: a probe was written to answer "is the CPU
vertex shadow copy available?" It answered yes — and simultaneously revealed
that the probe's own bounds check was wrong, because the check printed the
numbers it was comparing. A check that only said "failed" would have taught
nothing.

## 2.2 A WARP harness passing is not proof of correct optics

`tests/` contains WARP-device shader harnesses (`AutoSTSShaderHarness`,
`ScopeGeometryFillShaderHarness`, `ReticleLayerShaderHarness`,
`DrawTimeEyeBoxHarness`) and Python static contract verifiers
(`VerifySafetyContracts.py`, `VerifyLegacyStage5cContracts.py`).

They are valuable and should be kept green. They are **not** evidence the scope
looks right. `VERIFICATION.md` is explicit about this, and it has been
demonstrated repeatedly: a constant buffer bound in the harness but not in the
game meant an entire control was reading garbage in-game while every test
passed.

Two harness anti-patterns that have bitten this project:

- **Fixture leakage.** One test left a control at its maximum; a later test
  then measured the opposite of what it intended. Reset state explicitly.
- **Near-vacuous assertions.** A test measured 30 against a threshold of 60 and
  passed, because a forgiving eye-box radius kept both samples lit. Tighten
  parameters until the test can actually fail.

## 2.3 Sign and direction errors are only settled by the rendered result

Lens lag was derived in comments twice, both ways, each derivation internally
consistent. Only the rendered image resolved it. If a change has a
direction — a sign, a transform, an inverse — expect that reasoning alone will
not settle it, and plan a visual check.

Related, and the most instructive bug in the project's history: the
**"squashed slit."** A published aperture basis was applied as a matrix to
normalise lens coordinates. Its columns foreshorten independently, so the lens
collapsed into a narrow vertical band when yawing and a wide horizontal band
when pitching. The fix was to use a coordinate that is circular by
construction. **The basis is safe to use forward (lens → pixels), which is how
it is defined; inverting it is where the danger lives.**

## 2.4 Prefer coordinates that are correct by construction

The geometry shader's centre-fan apex is `2*inner - outer` **exactly**, and
`ScopeGeometryFill_GS.hlsl:108-135` carries a full algebraic derivation of why.
Substituting any other centre — including a "better" one published by the game
thread — gives the fabricated fan a different projective frame from its parent
wedge and turns the shared edge into a visible faceted circle. **Do not
"improve" it.**

The same lesson in another form: a per-triangle validity predicate and a
screen-centre axis term divided by the solved radius each gave adjacent wedges
their own disc, producing 24 radial spikes in the mask. Anything added to a
lens coordinate must be **uniform across the draw or vary smoothly with it.**

## 2.5 Never disrupt the player's aim

A hard product constraint, stated directly by the user:

> "we should not be disrupting the player's aim (which is the reticle), it
> should always be at the center of the optic and the magnified part lags
> behind it when the player moves the camera around."

An earlier lens-lag implementation translated the **sampled region**, which
showed the player a part of the world they were not pointing at. It felt
disorienting and was wrong in principle. The correct mechanism displaces the
**exit pupil** — the visible opening moves; the magnified content does not.

Concretely, in `ScopeGeometryMagnify_PS.hlsl`: nothing eye-derived may touch
`sampleDelta`. Only fisheye, refraction and breathing may. `physicalEyeTravel`
was deliberately moved down beside the pupil work so it cannot be casually
rewired into sampling.

## 2.6 Distinguish "not this frame" from "not ever"

A composite guard returned `false` whenever exact replay was not ready. That is
correct for a transient failure — a lost capture must keep the ordinary STS
draw rather than flashing a fullscreen circle. It is catastrophic for a
permanent condition: a non-annulus aperture waits forever, and the scope shows
nothing at all.

The fix was an explicit flag,
`automaticSTSApertureSupportsExactReplay`. Whenever a guard blocks an effect,
ask whether the blocking condition can ever clear.

## 2.7 Hidden controls are worse than useless controls

Circle Position and Circle Size were hidden on automatic profiles on the
reasoning that the geometry replay owns the lens. That stops being true the
moment a non-annulus aperture is chosen — so they were hidden on exactly the
scopes that needed them. Worse, the aperture dropdown sat inside the same
condition, so choosing a non-annulus shape would have hidden the control that
made the choice, with no way back.

The user's standing rule is simple: **if a control does nothing, remove it; if
it does something, show it.**

## 2.8 Comment the reasoning, not the mechanics

The existing comment style is unusually heavy and it is deliberate. Comments
explain *why a wrong-looking thing is right*, record what was tried and failed,
and pin invariants against future "improvement." Match it. A comment that says
what the next line does adds nothing; a comment that says "this apex is not an
approximation and must not be replaced" has already saved the project once.

## 2.9 Report honestly

Log evidence beats recollection. Several claims in past sessions were asserted
confidently and were wrong — that the overlay masked "from the same published
centre and radius" (only the centre was true), that exact replay "participates
in depth" (there is no depth test at all). When you state something about
runtime behaviour, cite the line or the log that shows it.

---

# 3. Architecture map

## 3.1 Build and deploy

```powershell
$env:MAGNASCOPE_MO2_MODS_PATH = "F:\Modlists\LoreOut\mods"
xmake build MagnaScope
```

The env var must be set **per PowerShell session**; the build script uses it to
auto-deploy to `F:\Modlists\LoreOut\mods\MagnaScope`. If deployment reports
`Not access because it is busy`, Fallout has the DLL open and must be closed.

Offline tests:

```powershell
xmake build AutoSTSShaderTest ScopeGeometryFillShaderTest ReticleLayerShaderTest DrawTimeEyeBoxTest
```

Game log: `C:\Users\rober\Documents\My Games\Fallout4\F4SE\MagnaScope.log`

## 3.2 Source layout

| file | role |
|---|---|
| `src/main.cpp` | Game-thread work: aperture discovery, scene-graph traversal, eye-box tracking, projection, profile lifecycle. |
| `src/hooking.cpp` / `.h` | Render-thread work: D3D11 hooks, draw classification, capture and replay, composite. The largest and most delicate file. |
| `src/ImGuiImpl.cpp` / `.h` | F4SE Menu Framework editor and popout. |
| `src/ScopeProfile.h` / `.cpp` | Per-scope settings, serialisation. |
| `src/Settings.h` | Staged verification gates (`AllowsProjection`, `AllowsGeometryMagnification`, `AllowsComposite`, …) driven by `MagnaScope.ini`. |
| `src/EyeBoxRecentering.h` | Lag/recentre response maths. |
| `src/HLSL/` | Shaders — see below. |
| `tests/` | WARP harnesses, Python contract verifiers, `NifInspector`. |

## 3.3 The shaders

| shader | role |
|---|---|
| `ScopeGeometryMagnify_PS.hlsl` | **The main optical shader.** Runs on the replayed aperture geometry. Magnification, shadow/exit pupil, parallax, breathing, fisheye, refraction, cleanup. |
| `ScopeGeometryFill_GS.hlsl` | Fabricates the annulus centre fan and publishes lens coordinates from primitive order. **The one piece that requires 48/48 topology.** |
| `ReticleLayer_PS.hlsl` | The isolated reticle composite. Mirrors the shadow maths so the reticle vanishes inside a genuine crescent, but never translates. |
| `ScopeShadow.hlsli` | Shared shadow/exit-pupil contract used by both of the above. Holds the soft-limit invariant. |
| `Triangle.hlsli` | Constant-buffer layout for b4. |
| `AutoSTS_PS.hlsl` | The screen-space fallback path — a flat circle composited late. |

Constant buffers: **b4** `ResolutionConstantData` (208 bytes), **b5**
`ScopeEffectData` (368 bytes / 92 floats). Both must be bound at every draw
site. A past bug bound only b4 in the replay path, so every control living in
b5 — shadow depth, image stillness, axial breathing, aperture scale — read
whatever the game happened to leave there. Shadow depth reading zero collapsed
the exit pupil to nothing, which is why no eye-box setting produced a crescent.

## 3.4 The render pipeline, end to end

1. **Game thread** (`main.cpp`, in the player-update hook, *after*
   `callOriginal()` so the first-person rig is current):
   - find the aperture in the scene graph;
   - publish its draw identity (buffer pointers, index count, stride, byte
     suballocation offsets) for the render thread;
   - project its world centre and radius to screen pixels;
   - sample eye-box translation and rotation, publish via a seqlock.

2. **Render thread**, `DrawIndexed` / `DrawIndexedInstanced` hooks
   (`hooking.cpp`): every draw is compared against the published identities.
   On a match for the aperture, the draw is **captured** (vertex shader, input
   layout, VS constant buffers b1/b2/b12, draw arguments) and then **suppressed**
   by forwarding a zero-count draw, so authored glass never enters the source
   image and cannot be magnified recursively.

3. **Composite**, at the TAA anchor or at Present: the finished frame is copied
   into a private texture, and the captured aperture draw is **replayed** with
   MagnaScope's geometry and pixel shaders against that coherent late-frame
   colour source. The reticle layer is composited afterward.

Two important properties of step 3, both verified in code:

- **There is no depth test.** `SetupCommonRenderState` sets
  `tempDSD.DepthEnable = false` (`hooking.cpp:3865`) and at Present binds no
  depth-stencil view (`hooking.cpp:3884`). Containment comes entirely from the
  mesh's rasterized silhouette plus the shader mask.
- **Source and destination share one colour encoding.** An earlier design
  sampled a pre-first-person snapshot taken before tone mapping into a
  display-encoded target, which made the whole optical image uniformly darker
  than the surrounding scene regardless of any control.

## 3.5 The D3D11 hooks

Installed with MinHook: `DrawIndexed` (vtable slot 12), `DrawIndexedInstanced`
(slot 20), `Present` (swapchain slot 8), `ResizeBuffers` (slot 13).

**Critical:** MinHook patches a function's *prologue*, not the vtable slot.
Multiple modules in the user's stack (`d3d11.dll`, `ShaderEngineCL.dll`,
frame-generation proxies) supply different implementations, and the vtable slot
alternates between them frame to frame. Rebinding to whichever is current burns
its rebind budget and then sits on a coin flip — which is what made the scope
intermittently fail to appear. See §5.3; this is not solved.

---

# 4. The current investigation

## 4.1 The problem

MagnaScope's exact optical path requires STS's authored `ScopeFade` mesh. Many
third-party optics ship no `ScopeFade` — they have a `ScopeAiming` NiNode with
`ScopeViewParts` and named glass elements beneath it (`specter_lens_rear`,
`hamr_lens_rear`, `LenseRearSTS`). On those scopes the effect either did not
appear or appeared wrong.

Work already landed for this:

- **Aperture discovery** (`ac73932`): walk `ScopeAiming`'s subtree, collect
  every renderable `BSTriShape` with valid bounds, rank them (`ScopeFade` name,
  then inside `ScopeViewParts`, then the rest), tie-break toward lens-named,
  rear-named, and smaller shapes. Verified by replaying the ranking over
  `NifInspector` dumps of eight real meshes; every one selects the surface a
  person would pick by hand.
- **Name matching by prefix.** `GetObjectByName` is exact-match and missed
  every authored suffix — `ScopeAiming:78` is not `ScopeAiming`. This alone
  rejected whole scopes before any aperture search ran.
- **An editor dropdown** to pin the aperture by name, saved to the profile.
- **A screen-space fallback** (`a88b9c8`, `831f376`) so a non-annulus choice
  magnifies through `AutoSTS_PS.hlsl` instead of showing nothing.

## 4.2 Why the fallback is unsatisfying

The screen-space overlay is a flat, always-circular, screen-aligned disc. It
does not foreshorten off-axis, does not roll with the weapon, and is composited
late so it can paint over geometry in front of the scope. The user's goal is
that **any selected mesh produces the same projection as a real `ScopeFade`.**

## 4.3 What was established about making exact replay universal

Verified by reading the code, not assumed:

**The replay machinery is already topology-agnostic.**

- Draw matching (`hooking.cpp:4986-5024`) compares vertex-buffer pointer,
  index-buffer pointer, index count, stride, and byte suballocation. Opaque
  identity only.
- Capture (`hooking.cpp:2294`) snapshots the game's vertex shader, input
  layout, VS constant buffers b1/b2/b12, and the draw arguments.
- Replay (`hooking.cpp:2433`) re-issues `DrawIndexed`.

None of it knows about 48 vertices. The gate is a single deliberate omission in
`main.cpp` (`PublishAutomaticSTSGeometry` is passed `nullptr` instead of the
render surface unless the shape is an annulus).

**The only genuine dependency is the lens coordinate.**
`ScopeGeometryFill_GS.hlsl:72-85` manufactures it from `SV_PrimitiveID`
assuming 24 wedges (`primitiveID >> 1`, `% 24`, `sin/cos`). On any other mesh
those coordinates are fiction. That is the entire reason for the 48/48
requirement.

## 4.4 The chosen direction

Rather than replaying whatever mesh is selected, **synthesize a
ScopeFade-equivalent disc at that mesh's position, orientation and size.** The
selected mesh becomes purely a **locator**.

Implementation sketch, using data already published:

- The CPU already computes the aperture centre in pixels plus
  `lensBasisXX/XY/ZX/ZY` — the pixel displacement produced by one aperture
  radius along the optic's local X and Z (`hooking.h:275-281`). That basis
  already encodes foreshortening and roll.
- Build the 24-segment ring **directly in clip space on the CPU** each frame:
  `pixel = centre + d.x * basisX + d.y * basisZ`, convert to NDC, upload to a
  dynamic vertex buffer in our own format — position plus an authored lens
  coordinate.
- Draw with a trivial pass-through vertex shader.

Use the basis **forward** (lens → pixels). See §2.3.

**What this deletes:**

- The 48/48 requirement.
- The geometry shader's primitive-order coordinate derivation, and with it the
  `2*inner - outer` apex. That apex exists only because the shader had to
  *infer* a coordinate it did not know; with an authored coordinate, a real
  centre vertex at exactly `(0,0)` is correct and the faceting it guards
  against cannot occur.
- The captured vertex shader, input layout, and b1/b2/b12 copies
  (`hooking.cpp:2369-2412`) for placement purposes.

**What it costs:**

- **Containment becomes the mask's job alone.** Today ScopeFade's silhouette is
  a hard backstop; a synthesized disc can paint over the scope body if its
  radius is wrong. This makes the aperture radius the critical per-scope value
  — which is what §4.5 addresses.
- The draw classifier is still needed to **suppress** the authored ScopeFade
  draw (`hooking.cpp:4662`). That stays ScopeFade-specific: for a real lens
  element the mesh should keep drawing normally.
- A matched draw is currently proof the scope rendered this frame.
  Synthesizing removes that proof; gating falls to the ADS/activation state.

Build it as a **new draw path alongside** the existing replay, so ScopeFade
scopes can be A/B'd before anything is removed.

## 4.5 Vertex measurement replaces the size heuristics

Everything the optical path knows about aperture size is currently inferred
from names and bounding spheres: a shape called `Glass` wins if it is 1.35×
wider than the fade plane, the aiming housing is taken at 0.82 of its sphere,
and the result is clamped between 1.5× and 4× — with a default of
`planeRadius * 3.0`. Those constants were fitted to `ScopeFade` and have no
meaning on an arbitrary lens element.

The vertices are the measurement those constants approximate. One pass yields:

- **True optical radius** — max distance from the centroid in the lens plane.
- **Optical centre** — the centroid, rather than the bounding-sphere centre.
- **Optical axis** — the thinnest extent axis, rather than assuming local X/Z
  because that is how STS authors `ScopeFade`.
- **Hole detection** — minimum radius from the centroid, answering the annulus
  question by measurement rather than by vertex count.

Caveat worth keeping: "a lens is sized to fit the housing" is a good rule but
not a guarantee — a rear element sits behind the aperture and can be smaller
than the visible opening. Expect to still want a per-scope scale multiplier,
just starting from a measurement rather than from `3.0`.

---

# 5. Current state

## 5.1 Uncommitted work

`git status` shows `M src/main.cpp` on top of `831f376`. It adds a **read-only
diagnostic probe**, plus a fix to a bug in that probe:

- `DecodeHalfFloat` (line 1362) — half-precision decode, written out rather
  than pulled from DirectXMath so it can be replayed by a host-side test.
- `ApertureVertexMeasurement` (line 1410) / `MeasureApertureVertices`
  (line 1443) — reads a shape's CPU vertex shadow copy via
  `BSGeometry::rendererData` → `BSGraphics::TriShape` → `vertexBuffer`, and
  reports centroid, per-axis half-extents, thinnest axis, outer radius, inner
  radius, world scale, and the bounding-sphere radius for comparison.
- Called from `PublishApertureCandidates` (line 1620), so it fires once per
  scope change rather than per frame, and probes **every** candidate — a scope
  where the chosen shape has no shadow copy but a sibling does is a different
  problem from none of them having it.

Nothing consumes the measurement. It is diagnostic only.

**The fix has never been compiled.** Four build attempts were interrupted
before the command ran, so there is no compiler output — the change is
unverified, not known-broken.

The deployed DLL is stale:

| file | last written |
|---|---|
| `Compile\F4SE\Plugins\MagnaScope.dll` | 3:01:33 PM |
| `F:\Modlists\LoreOut\mods\MagnaScope\F4SE\Plugins\MagnaScope.dll` | 3:01:33 PM |

The log analysed below was produced at 3:08–3:10 PM — i.e. **by that 3:01:33
build**, which has the probe with the broken bounds check. A successful rebuild
will move those timestamps; if it doesn't, the build didn't run.

### The bug that was fixed but not built

`BSGraphics::Buffer::dataOffset` is a byte offset into the pooled **GPU**
buffer — it is what the draw classifier matches against `IASetVertexBuffers`.
It must **not** be applied to the CPU `data` pointer, which addresses the
shape's own copy. The first version added it, so every span check compared
~63,000,000 against ~1 KB and rejected every shape with
`vertex span exceeds reported buffer size`. Fixed at `main.cpp:1487-1509`.

The check firing was fortunate: reading `data + 63586304` would have been a
wild pointer.

## 5.2 Log findings — CPU vertex data is available

This was the open question the probe existed to answer, and the answer is
**yes**.

`dataPointer=true, invalidCpuData=false` on **every shape on all four weapons
tested**, including the 13,589-vertex `specter` body. `dataSize` equalled
`numVertices * stride` exactly on every one of ~20 shapes, which is what
confirms `data` addresses the shape's own allocation rather than a pool.

`vertexDesc` is byte-identical everywhere: `0x0001B00000430205` → stride 20,
flags `0x1B`, no `VF_FULLPREC`. Positions are half-precision in a
`half4 position + half2 UV + 4B normal + 4B tangent` layout, which is the
branch `MeasureApertureVertices` already takes.

**§4.5 is unblocked.**

## 5.3 Log findings — the draw hook is the priority-#1 bug

The user has stated this is priority #1: *"I need you to permanently fix it
sometimes not showing up at all."*

```
15:08:10.433 [I] Installed DrawIndexedHook at d3d11.Dll+0x1545A0
15:08:10.557 [E] Failed to enable DrawIndexedHook implementation at d3d11.Dll+0x1545A0
15:08:14.963 [I] Hooked DrawIndexedHook implementation at ShaderEngineCL.dll+0x36580
15:08:24.586 [E] DrawIndexedHook saw a third implementation at d3d11.Dll+0x154B70;
                 only two can be hooked and draws through this one will be missed
```

Two distinct problems:

1. **The primary target never enabled**, so only one of the two slots is live.
2. **There is a third implementation.** The dual-hook design in `48072c4`
   assumed exactly two, and that assumption is now measurably false.

The addresses are not stable across runs either — an earlier session saw the
pair `d3d11+0x1545A0` / `ShaderEngineCL+0x67520`; this run saw
`ShaderEngineCL+0x36580` plus a third at `d3d11+0x154B70`. A fixed
primary/alternate pair cannot cover that. It wants a **small table of bound
targets with no fixed arity**, and the enable failure needs diagnosing rather
than being silently tolerated.

This fits the reported intermittency exactly: with one live hook out of three,
whether the scope appears depends on which implementation the frame goes
through. The scope did work during this session — `ScopeFade=DI:1` on 193
telemetry lines, exact replay ran at 15:08:36.168 — so this is a partial
failure, not a total one.

## 5.4 Log findings — `worldBound.fRadius` is transiently exactly 1.0000

26 log lines, in bursts where *every* candidate reads `1.0000` at once,
sandwiched between bursts reading real values:

```
15:09:23.450  ScopeFade:0  boundRadius=1.0663
15:09:24.004  ScopeFade:0  boundRadius=1.0000   <- every shape reads 1.0000
15:09:24.045  ScopeFade:0  boundRadius=1.0663
```

That field underpins `apertureRadius = planeRadius * 3.0`, the 1.35× extent
rejection, and the 1.5–4× clamp, and it is published as the projection radius.

It also makes **the candidate order flip between frames**. The last tie-break
is `left.radius < right.radius`; when every radius collapses to 1.0 the
comparison ties, `stable_sort` falls back to tree-walk order, and the list
reorders. Compare the 15:09:24.004 burst against the two either side —
`ScopeAiming:78` moves from 4th to last and back. `ScopeFade` outranks
everything structurally so today's automatic pick is unaffected, but any future
rank change would expose this.

This is further argument for §4.5: model-space vertex extents are static data,
not a per-frame-updated bound.

## 5.5 Log findings — confirmed working

The screen-space fallback engages as intended. At 15:08:54 the user selected
`ScopeAiming:78`; the log shows the non-annulus warning followed by
`Selected aperture cannot drive the exact replay; magnifying through the
screen-space path instead`, rather than the scope showing nothing.

---

# 6. What to do next

In this order.

1. **Build, deploy, get a fresh log.** Nothing else can be trusted until the
   bounds fix is actually running. The number to look for is
   `boundOverMeasured` — bounding-sphere radius over measured radius. On
   `ScopeFade` it should land near the hardcoded `3.0`. If it doesn't, that
   constant has been wrong all along and the log says by how much. Also sanity-
   check `opticalAxis=localY` (or whichever) against the mesh, and confirm
   `ScopeFade` reports a hole while `specter_lens_rear` does not.

2. **Fix the draw hooks.** By the user's own priority ordering this outranks
   the aperture work. Replace primary/alternate with a bounded table of bound
   targets; diagnose why MinHook's enable failed on `d3d11+0x1545A0` rather
   than tolerating it; keep the "saw another implementation" diagnostic but
   make it non-fatal.

3. **Consume the measurement.** Replace the aperture-radius heuristics with the
   measured radius, centroid, and axis. Expect to add a per-scope scale
   multiplier. Fix `worldBound.fRadius` reliance in the candidate tie-break
   while here.

4. **Synthesize the aperture disc** (§4.4), as a new path alongside the
   existing replay.

## Also outstanding, lower priority

- **Blur-while-firing test.** Set Lens Lag to 0 and fire. If the image still
  blurs, it is the upscaler or Fallout's own motion blur being magnified 4×,
  not anything MagnaScope does.
- **In-game verification** of the strafe-lag fix (`62398dc`, which switched the
  translation impulse to `PlayerCharacter::GetPosition()` because the
  first-person scene graph is not positioned in world space) and of the
  aperture dropdown behaviour.
- **Foreign meshes in the candidate list.** The Panzer/Grau list contains both
  `attachment_vm_lm_mgolf36_receiver_*` and
  `attachment_vm_ar_sierra552_receiver_*`. Whether that is one weapon's
  legitimate parts or stale attachment subtrees under `ScopeAiming` was not
  determined. `ScopeFade` outranks them so selection is unaffected, but the
  dropdown shows noise.

---

# 7. Working conventions and traps

## 7.1 Git

- **Do not commit or push unless explicitly told to.** This is a standing
  instruction from the user.
- **Never `git add -A` in this repository.** It has twice swept the user's
  `.nif` test meshes into commits. Eight files under
  `tests/NifInspector/meshes` are tracked as a result; a revert was offered and
  not answered. Stage explicit paths.

## 7.2 Documentation

The user does not want project documentation updated as a matter of course:
*"You don't need to update the document, just always give me a summary of the
changes you've made."* Summarise in chat instead.

## 7.3 PowerShell traps that have caused real damage

- A backtick-zero sequence inside a double-quoted string is a **NUL
  character**. This injected four nulls into `hooking.cpp`.
- `Set-Content -Encoding utf8` writes a **BOM**, which breaks `fxc` shader
  compilation.
- `Set-Content` with no `-Encoding` writes **UTF-16** on this system.
- Windows PowerShell 5.1: no `&&`, no `||`, no ternary, no null-coalescing.
- Prefer the Read/Edit/Write tools over shell text manipulation. Where an Edit
  fails on tab/indent mismatches, a small Python script is more reliable than
  fighting it.

## 7.4 Session hygiene

The user periodically asks to **kill headless instances** and **clear cache**.
Orphaned `claude.exe` processes are the ones whose command line has no
`--resume=<session-id>`; the current session's process must be left alone.

---

# 8. Commit history for context

| commit | what it did |
|---|---|
| `831f376` | Size the screen-space circle from the chosen aperture; un-hide Circle Position/Size on overlay scopes; move the aperture dropdown out of the replay-only gate. |
| `a88b9c8` | Magnify through the screen-space path when exact replay is impossible (added `automaticSTSApertureSupportsExactReplay`). |
| `ac73932` | Pick the glass, not the first shape under `ScopeAiming` (lens/rear/radius tie-breaks). |
| `62398dc` | Drive strafe lag from `PlayerCharacter::GetPosition()`. |
| `48072c4` | Dual draw-hook binding — the partial fix for the intermittent no-scope bug. Now known insufficient; see §5.3. |

---

# 9. StsConverter GUI: release audit (2026-08-30)

The See Through Scopes converter lives in `tests/StsConverter` (CLI and the
conversion library) and `tests/StsConverterGui` (WinForms front end, the
artifact that ships). This section records the audit done before the first
packaged release of the GUI, what it changed, and what it deliberately left
alone. Read it before touching either project.

## 9.1 What ships

| item | value |
|---|---|
| project | `tests/StsConverterGui/StsConverterGui.csproj`, `net8.0-windows`, WinForms, references `tests/StsConverter` |
| dependency | NuGet `Nifly` 1.0.0 (NiflySharp). Pure managed, no native libraries, so single-file publish is safe |
| publish | self-contained, single-file, win-x64, in-file compression on. 66 MB exe, 57.8 MB zipped. Without compression the exe is 146 MB |
| version | `0.1.0` (csproj `<Version>`); assembly title "STS Scope Converter" |
| package | `Package/StsConverterGui-0.1.0.zip` = `StsConverterGui.exe` + `README.md` (the release notes). `Package/` and `tests/**/publish/` are git-ignored |
| licence | NiflySharp is GPL-3.0 and is compiled into the exe, so the distributed GUI is GPL-3.0 regardless of the repo's MIT `LICENSE.txt` (which is still the unfilled template: `[year] [fullname]`). The zip ships the GPL text as `LICENSE-NiflySharp.txt`; the About dialog, the release notes and the converter README carry the credits (ousnius/NiflySharp, Miniball Apache-2.0, nifxml, See Through Scopes, NifInspector, NifSkope/Outfit Studio, Haru's M4 test sights, .NET 8) |

Publish command (the repo has no script for it; this is the whole recipe):

```
cd tests\StsConverterGui
dotnet publish -c Release -r win-x64 --self-contained true -p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true -p:EnableCompressionInSingleFile=true -p:DebugType=none -p:DebugSymbols=false -o publish
```

Do not add `PublishTrimmed`. `StsBuilder` reads and writes `BSEffectShaderProperty`
texture fields by reflection (`BlockReflection`), and trimming removes exactly
the members reflection needs.

Self-test (headless; constructs the window, loads real files, checks the grid
and asserts the shipped defaults):

```
StsConverterGui.exe --selftest <one or more .nif>
```

The release build was checked against four sights under the Haru M4 mod's
`meshes\Weapons\CyM4\sights\` folder in the MagnumOpus MO2 instance (build 0
warnings, self-test PASSED on both the bin and the published exe).

## 9.2 Findings

Severity is about what a user would have hit with the tool as it was.

| # | severity | finding | status |
|---|---|---|---|
| 1 | high | **The default settings produced the route that renders black in game.** `OptionsPanel` shipped with "keep original reticle material" unchecked, and `MainForm.AddFiles` auto-fills `MaterialsRoot` from `FileEntry.GuessMaterialsRoot` (any `Materials` folder within five parents of the mesh, which loose-file mods have). In `StsBuilder.CreateTextureLoader`, a materials root makes `TextureFromMaterial` return the BGEM texture, which sets `explicitReticleTexture`, which repoints the reticle material at the non-existent `ReticleCrossCustom` (line ~922). That is the missing-material route, and the 2026-08-15 in-game test recorded it as a solid black square. The only route confirmed working in game (`--keep-reticle-material`) was opt-in. | fixed: keep is the default radio; the per-file texture and materials inputs only reach the builder when the advanced block is open and keep is not selected; the self-test asserts the defaults |
| 2 | medium | Segment count was an editable numeric. Anything but 24 breaks MagnaScope's exact geometry-replay path, and the tooltip said so instead of preventing it. | fixed: locked to `ScopeFadeGeometry.CanonicalSegments`, shown as a fixed label |
| 3 | medium | A WinExe with no unhandled-exception handler dies silently: no console, no dialog, the window is just gone. Only the analyse and convert paths caught exceptions. | fixed: `Application.ThreadException` and `AppDomain.UnhandledException` show a dialog with the type, message and stack |
| 4 | medium | `tests/StsConverter/README.md` limitation 7 said "Nothing here has been tested in game", contradicting its own header, which records the in-game verdicts. | fixed (text) |
| 5 | low | README staleness not fixed: section A1 and the layout diagram call the reticle holder `Adjustments`, but `ConvertOptions.ReticleHolderName` defaults to `ReticleNode` after the 122-file corpus survey (see the comment on that property); the route table still calls `--materials` "preferred" although in game it renders black. The README is the CLI document and was left for its own pass. | open |
| 6 | low | `FileEntry.DotTexture` has no editor in the GUI and was still passed to the builder. | `BuildOptions` now passes `null` explicitly; the field remains for the CLI |
| 7 | low | `StsBuilder` line ~353 runs `NormalizeReticleRenderState` whenever `KeepReticleMaterial` is false, even when nothing was repointed. The comment above it says keep-material conversions are left untouched; the condition is "keep not requested", not "material repointed". Harmless with keep as the GUI default. CLI users converting without `--keep-reticle-material` and without any texture route still get their reticle render state rewritten to corpus values. | open, documented |
| 8 | low | The log appended by rebuilding the whole `TextBox.Text` per line, quadratic in batch size. | fixed: `AppendText` |
| 9 | low | Output folder defaults to `<input folder>\StsOutput`, inside the mod's mesh tree. Safe (the converter refuses to write over its input) but the user has to move the file to the original's relative path for it to override anything. | not changed; the options panel and the completion log now say so |
| 10 | info | The glass and reticle guesses (`ScopeAnalysis`) are name-driven: `glass`/`lens` +100, `reticle` +100, `crosshair` +80, `dot` +20, `parallax` -60, plus small triangle-count and flatness terms. A scope with unusual or non-English shape names lands in "Needs selection" and is held back rather than converted wrongly, which is the right failure. On an already-converted STS file the glass scorer picks the `_full` hip clone; harmless (those files need Force anyway) but it shows the scorer has no penalty for the clone suffix. | open, low value |
| 11 | info | Things the tool cannot do, restated because every release note must carry them: the aiming model is not cut down (the rear geometry stays in `ScopeAiming`; that is NifSkope or Outfit Studio work); the ScopeFade size is a heuristic (0.94 of the measured glass radius); the rearward axis is assumed to be -Y. | by design |

## 9.3 What the GUI rework changed

- Defaults are the in-game-verified configuration: keep the scope's own
  reticle, generate the canonical annulus, clone the model into the hip branch,
  no custom texture, no materials repoint.
- The reticle choice is two radios: keep (recommended) or an STS preset (with
  a note that the preset route is unverified in game). The custom
  missing-material route is not offered as a first-class choice at all.
- The grid shows File, Glass, Reticle, Dot, Status, Result. The "Reticle
  texture" and "Materials folder" columns and the Materials button exist only
  while "Show advanced options" is ticked, and their values are ignored while
  keep is selected.
- Advanced block, collapsed by default: fade mode and scale, geometry (fixed),
  hip duplicate, TextureLoader, flat ScopeViewParts, hip clone suffix, error
  tolerance, force, and a warning about the custom route.
- One action bar at the bottom: output folder, Browse, Open (opens the output
  folder in Explorer), Convert Selected, and an accent-coloured Convert All
  that becomes Cancel while a batch runs.
- A "Shapes..." button opens the per-shape report for the selected file
  (double-click still works).
- Segoe UI 9, flat header row, row height 26, an empty-state hint above the
  grid, a Consolas log. Window 1180x760, minimum 980x620 (was 1400x820 /
  1100x620).
- The self-test now also asserts the defaults, so a stray edit cannot ship
  the black-square route again without failing the smoke test.

## 9.4 What to do next on this tool

1. In-game pass on scopes that are not the five M4A1/MK18/RU556 sights. The
   release notes carry an explicit "not sure how well it works per scope"
   disclaimer for this reason.
2. Test the preset route in game once; it passes every structural check and
   would give users STS reticle swapping.
3. If the custom route is ever wanted, the fix is known: parse the BGEM blend
   fields (`blendState`, `blendFunc1/2`, `alphaTest`, `alphaTestRef`, NiAlpha
   enum values; the v1 header layout is in the memory notes and in
   `BgemReader`) and replicate them on the NIF shader instead of applying the
   STS blend recipe to a texture authored against the mod's own.
4. README refresh for the CLI (finding 5).
5. Cosmetic: an application icon; the Shapes report already prints the fade
   radius at 0.94, a live preview in the grid would save a NifSkope round trip.
