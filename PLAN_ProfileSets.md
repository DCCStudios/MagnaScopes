# MagnaScope — Profile Sets, Secondary Sights, Reticle Switching, API

Plan for the next feature block. Written against the tree at commit `e58aeac`.

Five requested features:

1. **Magnification variants** — per weapon/OMOD, several profiles keyed by magnification, opt-in, scroll-wheel switchable in ADS, stepped or continuously blended.
2. **Secondary sight** — offset irons / piggyback optic, "new zoom data with a lerp".
3. **Reticle switching** — per-profile folder of reticle textures, hotkey-cycled.
4. **API** — expose equipped-OMOD settings and the target NIF's node tree, readable and writable.
5. **Session persistence** — everything above survives in the co-save.

---

## 0. Facts this plan is built on

Verified in the tree, not assumed. Each design decision below leans on one of these.

| Fact | Where |
|---|---|
| Profiles are keyed by `(sourcePlugin, sourceFormID, omodKey)` in one `std::map` | [ScopeProfile.h:312](src/ScopeProfile.h#L312) |
| One JSON file per weapon, `Scopes: { <omodKey>: {...} }`, merged on write | [ScopeProfile.cpp:926-974](src/ScopeProfile.cpp#L926-L974) |
| Path is `Data\F4SE\Plugins\MagnaScope\Auto\<plugin>_<formID>.json` | [ScopeProfile.cpp:778](src/ScopeProfile.cpp#L778) |
| Mouse wheel = mouse button IDs 8/9, routed to `AdjustZoomDelta(±0.1)` | [main.cpp:3150-3160](src/main.cpp#L3150-L3160) |
| `gameZoomDelta` is clamped to `[minZoom,maxZoom]`, and **edit mode already substitutes different bounds** | [hooking.cpp:5313-5328](src/hooking.cpp#L5313-L5328) |
| The editor preview snapshot is the *existing* mechanism for "override every profile value live, without touching the profile" | [hooking.cpp:5334](src/hooking.cpp#L5334), [main.cpp:3466-3500](src/main.cpp#L3466-L3500) |
| `PublishEditorPreview` takes **34 positional parameters** | [ImGuiImpl.h:334-369](src/ImGuiImpl.h#L334-L369) |
| `ApplySelectedEditorPreview` already writes zoom data to the live form every editor frame, and the user tunes camera offsets live — so **BGSZoomData is re-read continuously by the camera** | [main.cpp:2952](src/main.cpp#L2952) |
| Shader constant buffer is exactly 208 bytes, asserted, mirrored by hand in `Triangle.hlsli`; the last three spare floats were consumed in `e58aeac` | [hooking.h:218](src/hooking.h#L218), [Triangle.hlsli:91-99](src/HLSL/Triangle.hlsli#L91-L99) |
| The reticle composite binds only `t4`/`t5` and is gated on `mAutomaticSTSReticleLayerReady` — **no capture, no composite** | [hooking.cpp:4945](src/hooking.cpp#L4945) |
| The authored reticle's colour is suppressed in the scene only *after* an exact ScopeFade replacement succeeded | [hooking.cpp:6179](src/hooking.cpp#L6179) |
| `kPreSaveGame` detaches zoom overrides; `kPostSaveGame` reattaches | [main.cpp:4875-4880](src/main.cpp#L4875-L4880) |
| F4SE serialization interface is available in this CommonLibF4 checkout, unused so far | `lib/commonlibf4/include/F4SE/Interfaces.h:262` |

---

## 1. The central architectural decision

The naive reading of "profiles per magnification" is: make `ScopeProfile` a container of profiles, and make `currentData` point at whichever is active. That is wrong here, and expensively so — `currentData` is read from both threads, is compared by identity on save, is written through by `*currentData = *pendingSave`, and is the anchor for the delete/forget/reselect lifecycle. Making it swap under the renderer would put a moving target under every one of those.

**Instead: variants resolve through the editor-preview channel.**

That channel already exists and already does exactly this job. Every value a variant needs to change is already a value the editor previews live, delivered as one plain-data snapshot, consumed at one point in `hooking.cpp` for shader constants and one point in `main.cpp` for zoom data, and gated on a selection revision that prevents a stale snapshot reaching the wrong weapon. Adding a second *producer* to that channel is a far smaller change than making the profile object polymorphic, and it inherits the revision gating for free.

So:

```
                       ┌─ edit mode on  → editor snapshot  (wins)
LiveOverlay  ←─────────┤
                       └─ edit mode off → variant resolver + sight blend
```

`ScopeProfile` stays the authored, saved, identity-bearing object. It gains variant *data*, never variant *behaviour*.

**Prerequisite refactor (Phase 0):** `PublishEditorPreview`'s 34 positional floats must become a struct parameter before anything is added to it. Two producers × 40-odd positional floats is a defect generator. This is mechanical, compiles or doesn't, and ships on its own.

---

## 2. Data model

### 2.1 Variants

```cpp
// ScopeProfile.h
struct ProfileVariant
{
    // The key. Sorted ascending; duplicates collapse on load.
    float magnification = 1.0F;
    std::string label;              // optional, editor display only
    ShaderData shaderData;          // full copy
    ZoomDataOverwrite zoomDataOverwrite;
};

struct VariantSet
{
    bool enabled = false;           // opt-in, per the request
    bool continuous = false;        // false = stepped, true = blended
    float stepSeconds = 0.12F;      // ease time for a stepped change
    std::vector<ProfileVariant> variants;
};
```

Added to `ScopeProfile` as `VariantSet variants;`. Absent key → `enabled=false` → byte-identical behaviour to today. Existing profiles load unchanged.

**Only numerics vary per variant.** `apertureSurface`, `reticleSurface`, `legacyMode`, `UsingSTS`, `IsCircle`, and the bool flags stay on the base profile and are *not* part of a variant, even though they live inside `ShaderData`. Two reasons: they are not lerpable, and changing `apertureSurface` mid-scroll would re-trigger aperture discovery and geometry re-selection every frame of a blend. The resolver overwrites the resolved copy's non-numeric fields from the base after lerping. Document this in the editor UI as "structural settings are shared across magnifications".

`ShaderData` is copied wholesale into the variant anyway, rather than defining a narrow lerpable subset, because a subset would need maintaining in lockstep with `ShaderData` forever and would silently drop any field added later. Copying whole and overwriting the non-numerics afterward fails *safe* when a field is added.

### 2.2 Secondary sights

```cpp
struct SecondarySight
{
    std::string name = "Iron Sights";
    ZoomDataOverwrite zoomData;     // absolute, same convention as the base
    bool suppressOptics = true;     // fade the aperture/magnification out
    float transitionSeconds = 0.18F;
};
```

`std::vector<SecondarySight> secondarySights;` on `ScopeProfile`. Flat by design — no magnification variants, per the request.

### 2.3 Reticles

```cpp
// Saved:
std::string selectedReticleFile;    // "" = authored 3D reticle mesh
float customReticleScale = 1.0F;    // in aperture radii
// Not saved — rebuilt from disk on selection:
std::vector<std::string> discoveredReticles;
```

---

## 3. On-disk layout

Requested: a folder per weapon EditorID, a folder per OMOD profile inside it, a `reticles` folder inside that.

```
Data\F4SE\Plugins\MagnaScope\Auto\
  10mm Pistol [Fallout4.esm_0004822B]\
    Reflex [STS.esp_0001A3F1]\
      profile.json
      reticles\
        chevron.dds
        german4.png
```

Three constraints shape this:

- **EditorIDs are not reliably available at runtime in Fallout 4.** `TESForm::GetFormEditorID()` returns `""` in the base class and weapons do not generally override it. The folder name is therefore *best-effort display*: `GetFormEditorID()` if non-empty, else `TESFullName::GetFullName()`, else the plugin stem — always suffixed with `[<plugin>_<localFormID>]` for uniqueness.
- **`omodKey` contains `:` and can join several identities.** It cannot be a directory name verbatim. The folder gets a sanitised label; `profile.json` carries the authoritative `omodKey` string in its body.
- **Therefore lookup keys come from file *contents*, never from paths.** The directory walk finds `profile.json` files; the map key is built from what's inside. Renaming a folder by hand cannot break profile matching — it only changes where the reticles for it live, which the profile records by relative path.

**Migration.** On startup, existing `Auto\<plugin>_<formID>.json` files are read exactly as today (that reader stays). When such a profile is next *saved*, it is written to the new layout and the old file is left in place, untouched. A one-way, save-triggered, non-destructive migration. The reader prefers the new layout when both exist for the same key, and logs when it shadows an old file. No bulk rewrite on startup — the config-clobbering incident of 8/10 is the reason not to touch files the user did not ask to have touched.

---

## 4. Runtime resolution

New `src/ScopeResolver.h/.cpp`, game-thread only, one call per `HookedUpdate` tick:

```cpp
struct SightSessionState      // per (plugin, formID, omodKey), lives in a map
{
    float  variantPosition = 0.0F;   // continuous index into variants[]
    float  variantTarget   = 0.0F;
    int    secondaryIndex  = -1;     // -1 = primary optic
    float  sightBlend      = 0.0F;   // 0 = primary, 1 = secondary
    int    reticleIndex    = -1;     // -1 = authored mesh
};
```

Per tick:

1. Ease `variantPosition → variantTarget` over `stepSeconds` (stepped: target is integral; continuous: target *is* the position, set directly by scroll).
2. Ease `sightBlend` toward `secondaryIndex >= 0 ? 1 : 0` over `transitionSeconds`.
3. Build the resolved overlay: lerp `variants[floor(p)]`→`variants[ceil(p)]` by `frac(p)`; overwrite non-numerics from base; if `sightBlend > 0`, lerp the resulting `ZoomDataOverwrite` toward the secondary's, and scale the aperture activation by `1 - sightBlend` when `suppressOptics`.
4. Publish it into the overlay channel — the same call the editor uses.
5. Write the zoom half through `ApplySelectedEditorPreview`'s path.

**Aperture fade reuse.** Suppressing optics for a secondary sight multiplies the existing `SCOPE_FADE_ACTIVATION` rather than adding a new kill switch. That constant already fades the whole optical composite in and out of ADS and is consumed identically by the scene replay and the reticle layer, so both go with it and cannot desynchronise.

**Magnification.** A variant's `magnification` sets `gameZoomDelta` directly (bypassing `AdjustZoomDelta` accumulation) and is clamped to that variant's own `[minZoom,maxZoom]`. When variants are enabled, free-scroll zoom is off — the wheel selects variants instead. That is the whole point of "set zoom levels".

**Save-window gate.** The resolver must not write zoom data between `kPreSaveGame` and `kPostSaveGame`; `DetachIsolatedZoomForSave` exists precisely to get MagnaScope's values out of the form before it is serialised, and a resolver tick in that window would put them straight back. One `savingInProgress` flag, checked at step 5.

---

## 5. Input routing

One configurable hotkey — `opticsKey`, stored beside `nvKey`/`guiKey` in `MagnaScopeConfig.json`, bound in the editor with the existing key picker.

```
key down                 → record time, clear consumedByScroll
wheel while key held     → consumedByScroll = true; cycle secondaryIndex
                           through {-1, 0 .. secondarySights.size()-1}
wheel, key not held      → variants enabled ? step/blend variant
                                            : AdjustZoomDelta (unchanged)
key up, held < 250ms
   and !consumedByScroll → cycle reticleIndex through
                           {-1, 0 .. discoveredReticles.size()-1}
key up, otherwise        → nothing
```

Acting on *release* is what makes tap and hold distinguishable without delaying the tap, and it is what the request describes. All of it is gated on the same condition the existing wheel handling uses — render state active and no blocking window — plus "in ADS", which the wheel path does not currently require but this does.

While the hotkey is held, the wheel must **not** also reach `AdjustZoomDelta` or the variant stepper. The router is an if/else chain for that reason.

---

## 6. Reticle switching

The private reticle layer is the right seam: everything downstream of it — scaling, offset, exit-pupil shadow, dual-source composite — is already correct and reticle-source-agnostic.

**Composite change.** `ReticleLayer_PS.hlsl` gains a branch on a new constant `SCOPE_CUSTOM_RETICLE`:

- `0` — sample the captured layer, exactly as today.
- `1` — sample a bound texture at `t6`, anchored on the **lens centre** (not the authored reticle centre, which may not exist), sized in aperture radii by `customReticleScale × SCOPE_LENS_RADIUS`. Emit `B = rgb·a`, `T = 1 - a`, which is the same `ONE/SRC1_COLOR` contract the capture path produces.

**Two gaps this exposes, both real:**

1. **The composite only runs when a capture happened** (`mAutomaticSTSReticleLayerReady`). A scope with no authored reticle draw — or a custom reticle where the capture is skipped — never composites. The composite needs a second entry condition: *a custom reticle is selected and the aperture replay ran this frame*.
2. **Colour suppression and capture are interleaved** at [hooking.cpp:6179-6260](src/hooking.cpp#L6179-L6260). With a custom reticle the authored mesh must still be suppressed (or it shows through underneath), but the two capture passes are wasted. Skipping capture while keeping suppression is a small branch, but it must be written deliberately, not fallen into.

**Constant buffer.** `ConstBufferData` is full at 208 bytes and its layout is mirrored by hand in `Triangle.hlsli`. Adding `SCOPE_CUSTOM_RETICLE` and `SCOPE_CUSTOM_RETICLE_SCALE` means one new 16-byte row (4 floats, 2 spare), and the struct, the `static_assert`, and the HLSL `cbuffer` must change in the same commit. Drift here is silent constant corruption, not a compile error — the assert only catches the C++ side.

**Loading.** `DDSTextureLoader11` and `WICTextureLoader11` are both already in the tree, so `.dds` and `.png` both work. Textures load on the render thread through the existing `bChangeAimTexture` request flag pattern. One texture is resident at a time; switching sets the flag.

### Why not STS's own reticle-swap mechanism

STS already has in-game reticle customisation, and it works differently: the reticle's `BSEffectShaderProperty` points at a deliberately **non-existent** material (`Materials\Scope\ReticleCrossCustom.BGSM.BGEM`), so the engine falls back to the `Source Texture` named in the shader property — but only if that texture is already resident, which is what the nonsense `TextureLoader:0` node under `ScopeViewParts` exists to guarantee. Naming a fake material rather than none is what keeps an engine **Material Swap** viable, and the material swap is the actual switching mechanism.

Driving that from MagnaScope would mean issuing material swaps against the equipped instance. It is the more "native" answer and it would survive into the unmagnified hip model too. It is rejected for one decisive reason: **it only works on scopes authored with the TextureLoader trick and the fake-material naming**, and the user's own assessment is that community STS scopes are not consistent about this. A reticle switcher that silently does nothing on half the corpus is worse than no switcher.

Compositing in the reticle layer works on every scope MagnaScope can already magnify, including ones with no authored reticle at all, because it does not depend on how the mesh was authored. The cost is that the custom reticle exists only inside the magnified optic — which is exactly where it is wanted.

Worth revisiting if the NIF converter (in progress) standardises the TextureLoader/fake-material setup on converted scopes: for *those*, both paths would be available.

---

## 7. Persistence split

Three kinds of data, three homes. The split is not arbitrary — it follows from *what forks with a save game*.

| Data | Home | Why |
|---|---|---|
| Variants, secondary sights, optical tuning, reticle scale, `defaultVariantId`, `defaultReticleFile` | `profile.json` | Authored. Shippable in a patch, shareable, the same for every character. |
| Active variant, active reticle, active secondary sight | Co-save | Per-character session state. Must fork when the save forks. |
| Reticle textures | `reticles\` beside the profile | Loose user-droppable assets. |

Session state must **not** go in a sidecar JSON. Two failures follow immediately if it does: loading a save from before an optic swap restores the wrong sight, and two characters carrying the same weapon share one selection. The co-save gets both right by construction.

The authored defaults exist so a fresh character on a scope with no co-save entry still starts where the profile author intended — 1x, chevron reticle — rather than at index zero. The co-save overrides them the moment the player touches anything.

## 8. Co-save

`SetUniqueID('MGSC')`, one record:

- `'SSTA'` v1 — session state. `u32 count`, then per entry: length-prefixed `sourcePlugin`, `u32 sourceFormID`, length-prefixed `omodKey`, `f32 variantPosition`, `i32 secondaryIndex`, `i32 reticleIndex`.

Notably **no `ResolveFormID` calls are needed**: the key is a plugin *filename* plus a *local* form ID, which is already load-order independent. That is a property worth keeping — it means a co-save survives load-order changes that would break a raw-FormID scheme.

`SetRevertCallback` clears the map. `reticleIndex` is re-validated against the discovered file list on load, since the user may have deleted a texture between sessions; an out-of-range index falls back to `-1` (authored mesh) rather than clamping into someone else's reticle.

Load and save callbacks run on the game thread during the engine's save/load, which is also where the session map is mutated — no locking required, but it must be asserted rather than assumed.

---

## 9. API

**The requirement is narrower than "expose the node tree".** From the request thread, the actual ask is: an external caller wants to hand MagnaScope a shape — *"just make it take `BSTriShape*`, or whatever you pass to your mod after your helper function searches for `ScopeFade:0`"* — and have the plugin retarget onto it, plus trigger the sight swap. The use case is a **double-pip optic**: two sights on one NIF, because putting two zoom-data-bearing attachments on one weapon misbehaves in the engine, so it has to be a single mesh with two apertures.

That is not a node-editing API. It is *"let an external caller override which shape MagnaScope treats as the aperture and reticle, and trigger the secondary-sight swap."* Both halves already exist internally: `apertureSurface`/`reticleSurface` are pinnable by name today, and the swap is Phase 6.

```cpp
struct MagnaScopeInterfaceV1
{
    std::uint32_t version;      // 1
    // Retarget. Shape may be null to clear.
    bool (*SetApertureOverride)(RE::BSTriShape* shape);
    bool (*SetReticleOverride)(RE::BSTriShape* shape);
    bool (*SetApertureOverrideByName)(const char* nodeName);
    bool (*SetReticleOverrideByName)(const char* nodeName);
    // Sight swap. index -1 = primary optic.
    bool (*TriggerSightSwap)(std::int32_t sightIndex);
    bool (*ClearOverrides)();
    // Read-only discovery: how a caller finds the shape to pass.
    std::uint32_t (*SnapshotNodeTree)(MagnaScopeNodeV1* out, std::uint32_t max);
    bool (*GetEquippedScope)(MagnaScopeScopeInfoV1* out);
};
```

Three rules, non-negotiable:

- **Pointers are resolved on receipt, never retained.** A passed `BSTriShape*` is converted to its node name inside the call and only the name is stored. The plugin therefore cannot outlive the caller's pointer, and a 3D rebuild between the call and the next frame resolves harmlessly to "shape not found" instead of a use-after-free.
- **Setters enqueue.** A command queue drained in `HookedUpdate`. Callers may be on any thread; engine node data may only be touched on the game thread.
- **Getters return copies from a published snapshot.** No `NiAVObject*` crosses the boundary outward, ever.

The node-tree snapshot is a flattened array (name, parent index, local transform, cull flag) of `player->firstPerson3D`, rebuilt when the equipped instance changes and on explicit request. It is the discovery mechanism — a caller walks it, finds its second aperture, passes the name back.

**Papyrus** (`RegisterFuncs` already exists) mirrors the same operations for script-driven callers, plus reads of the active variant/reticle/sight. Papyrus is game-thread by construction.

**Deferred to a v2 interface, deliberately:** arbitrary node transform and cull writes. Not because they are hard, but because they are not what was asked for, and their failure modes (3D rebuilt on cell change, race change, and `bRefreshChar`, invalidating any restore map; two consumers writing one node with no arbitration) cost more to get right than the requested feature does. If a caller later needs them, that is a stated-contract v2 with an overlay-and-reapply model, not a v1 default.

---

## 10. Implementation order

Each phase compiles, deploys, and is testable in isolation. No phase depends on a later one.

| # | Phase | Ships |
|---|---|---|
| 0 | `PublishEditorPreview` → struct parameter | Nothing user-visible; unblocks everything |
| 1 | Disk layout + migration-on-save, contents-derived keys | New folders appear as profiles are saved |
| 2 | Overlay channel: second producer, resolver skeleton, save-window gate | Still identical behaviour; resolver publishes the base profile |
| 3 | Variants — data, editor UI (variant selector, add/remove/duplicate), resolver lerp | Feature 1, stepped only |
| 4 | Continuous blend + `stepSeconds` easing | Feature 1 complete |
| 5 | Hotkey router + `opticsKey` binding | Input plumbing, no consumers yet |
| 6 | Secondary sights — data, editor, blend, activation suppression | Feature 2 |
| 7 | Reticle discovery, constant-buffer row, shader branch, composite entry condition, texture load | Feature 3 |
| 8 | Co-save | Feature 5 |
| 9 | Papyrus + C ABI + node snapshot/command queue | Feature 4 |

Phases 7 and 9 are the two with genuine unknowns. Everything before them is mechanical.

---

# Adversarial review

Reviewing the above as if someone else wrote it. Findings ranked by how much damage they do if they survive into implementation.

### A1 — The overlay channel is gated on `selectionRevision`, and gameplay has no editor to bump it. **Blocking.**

The plan leans on the editor-preview channel's revision gating as a free correctness property. Look at what that gating actually does: [main.cpp:3478](src/main.cpp#L3478) applies the preview only when `editorPreview.selectionRevision == zoomSelectionRevision`, and `zoomSelectionRevision` is incremented in `InitCurrentScopeData` on every reselection. The editor republishes on every frame it draws, so it re-syncs immediately.

The resolver publishes from the game thread *inside* `HookedUpdate`, in the same tick sequence that increments the revision. If it publishes before `InitCurrentScopeData` runs, its snapshot is stale for one tick and gets dropped — which for a resolver means one tick of the *base* profile's values leaking through at the wrong magnification. On a stepped 1x↔8x variant set that is a visible one-frame flash.

**Fix:** the resolver must run *after* profile selection within the tick, and must read `zoomSelectionRevision` at publish time rather than caching it. It also must not publish at all on a tick where selection changed — hold the previous frame's resolved values instead, and resynchronise on the next tick. Add this ordering constraint explicitly to Phase 2.

### A2 — "Edit mode wins" makes the editor unable to author variants. **Blocking, design error.**

Section 1 says the editor snapshot wins over the resolver. But the editor is *how variants get authored* — the user selects variant 3, tunes it, saves. If edit mode suppresses the resolver entirely, the editor previews at the base profile's magnification and zoom data, so the user is tuning variant 3 while looking through variant 0. Every variant after the first would be authored blind.

**Fix:** it is not a precedence relationship, it is a composition. The resolver always runs and establishes *which variant is active*; the editor overlays its unsaved values on top of that variant. Concretely: `BuildEditedProfile` takes a base — it must take *the active variant* as its base, not `*currentData`. And on save, the edited values write back into `variants[activeIndex]`, not into `shaderData`. This ripples into `RequestProfileSave`/`ConsumeProfileSave`, which currently assign a whole `ScopeProfile`.

That last part is the real cost: the save path's `*currentData = *pendingSave` becomes wrong when variants are enabled, because it would overwrite the entire variant vector with whatever the editor's snapshot happened to contain. The save payload needs to carry *which variant it edited*, and the apply needs to be a targeted write. Phase 3 grows accordingly.

### A3 — Variants and `zoomSpread`/`AdjustZoomDelta` are two zoom systems on one variable. **High.**

`gameZoomDelta` is clamped every frame at [hooking.cpp:5326](src/hooking.cpp#L5326) to the *current profile's* `[minZoom,maxZoom]`. The resolver setting `gameZoomDelta` from a variant's magnification will fight that clamp whenever the variant's magnification lies outside the resolved `minZoom..maxZoom` — and the resolved bounds are themselves lerped between variants, so during a blend the clamp is moving too.

The plan hand-waves this as "clamped to that variant's own bounds". That is not sufficient: during a continuous blend between a 2x variant and an 8x variant, the interpolated bounds and the interpolated magnification are both moving, and nothing guarantees the latter stays inside the former.

**Fix:** when `variants.enabled`, the resolver **owns** magnification outright — it writes `gameZoomDelta` and the clamp range is set to `[magnification, magnification]` for that frame. Free-scroll headroom (`zoomSpread`) is a *non-variant* feature and the two are mutually exclusive. Say so in the UI, and disable the min/max sliders when variants are on, or they read as controls that do nothing.

### A4 — Live `fovMult` writes during a blend are unproven at 60 Hz. **High, needs an in-game probe before Phase 4.**

Section 0 claims BGSZoomData is re-read continuously because the editor previews camera offsets live. That claim is sound for `cameraOffset` — the user has been dialling those and watching them move. It is *weaker* for `fovMult`, because the camera's `fovAdjustCurrent` is an interpolated value ([hooking.cpp:5309](src/hooking.cpp#L5309) reads `pcam->fovAdjustCurrent`, not the raw mult), and an engine-side interpolator fed a per-frame-changing target may lag, ratchet, or reset the transition.

The editor changes `fovMult` in discrete jumps when a slider moves; a continuous blend changes it every frame for 120ms. Those are not the same test.

**Fix:** before building Phase 4, do a throwaway probe — ramp `fovMult` from 1.0 to 2.0 over one second on the game thread and log `pcam->fovAdjustCurrent` each tick. If it tracks, continuous blending of FOV is viable. If it lags or stutters, continuous mode blends the *lens* magnification only and snaps `fovMult` at the midpoint, which is still a usable feature and worth knowing before the UI promises otherwise. Stepped mode (Phase 3) is unaffected either way and should ship first regardless.

### A5 — Variant identity is a float. **Medium, but corrupts data if ignored.**

`ProfileVariant::magnification` as the key, "duplicates collapse on load", JSON round-trip through `float`. Two variants authored at 4.0 and 4.0000001 are distinct in the vector, identical to the user, and their collapse order depends on sort stability. Worse, the co-save stores `variantPosition` as a float *index*, so inserting a variant shifts every saved position by one — a co-save from before the insert silently selects the wrong optic.

**Fix:** give each variant a stable `std::uint32_t id`, assigned on creation, never reused within a profile. Magnification stays the sort key and the display value; `id` is what the co-save stores and what the editor's save payload targets. `variantPosition` remains a float for blending but is resolved to an id on save and back to a position on load. This is cheap now and unfixable later without invalidating everyone's co-saves.

### A6 — Reticle discovery does filesystem I/O on the game thread. **Medium.**

Section 4 has the resolver, and Section 2.3 has `discoveredReticles` "rebuilt from disk on selection". Profile selection happens inside `HookedUpdate` — the main thread — and a directory enumeration on a cold cache, on a mechanical drive, under MO2's USVFS, is not a bounded operation. MO2's virtual filesystem in particular adds real latency to directory walks. A hitch on every weapon swap is exactly the kind of thing that gets a mod blamed for stutter.

**Fix:** enumerate once at `kGameDataReady` into an in-memory index of `profile folder → reticle files`, and refresh it only on explicit user request (an editor "Rescan" button). Users add reticle files between sessions, not mid-firefight.

### A7 — The API was designed against a paraphrase, not the request. **Resolved — see §9.**

The first draft answered "expose/change all the NodeTree data" with a broad arbitrary-write API, and then spent a finding worrying about its failure modes: "restore on unequip" needs a reliable unequip signal for first-person 3D, but 3D is also rebuilt on cell change, race change, and `bRefreshChar`, each of which leaves a restore map keyed to freed pointers; and two consumers writing one node have no arbitration.

Both worries were real. Neither needed solving, because the actual request was narrower: hand the plugin a `BSTriShape*` and have it retarget its aperture/reticle onto that shape, plus trigger the sight swap, in service of a double-pip optic on a single NIF. That is a retarget API, not a node-editing API, and §9 has been rewritten accordingly.

The lesson generalises past this feature: **the broad reading cost a whole subsystem's worth of design and every hazard in it was self-inflicted.** Where a request is paraphrased into a capability, check the paraphrase against the source before designing to it.

Residual, still true: the read-only node snapshot stays (it is how a caller discovers the shape to pass), and passed pointers must be resolved to names on receipt rather than retained.

### A8 — Phase 1 migrates on save; Phase 8's co-save keys off `omodKey`; nothing pins the two together. **Low, but it is a silent-loss class.**

The disk migration writes to a new path. The co-save keys off `(plugin, formID, omodKey)`. Those agree today. But `omodKey` is derived by `GetEquippedAttachmentKey` from *scope-token-matched* OMOD names ([main.cpp:2775](src/main.cpp#L2775)), with a fallback to all attachments — meaning a weapon mod that renames its attachment points changes the key, and the profile *and* the co-save entry both orphan simultaneously and silently.

**Fix:** not solvable in general, but detectable. Log at info when a profile is synthesized fresh for a weapon that already has *some* saved profile under a different `omodKey`. It costs nothing and turns "my settings vanished" into a diagnosable line in the log.

### A9 — `stepSeconds` easing and the reticle's fixed-to-the-weapon guarantee. **Low, but check it.**

Easing magnification over 120ms means `SCOPE_FADE_MAGNIFICATION` changes every frame during a step. The bore-axis pivot shipped in `e58aeac` was chosen specifically because the residual is magnification-independent — screen centre is a fixed point of the pivot algebra. That property should hold under a moving magnification, which is exactly what makes the easing safe.

It is worth stating as a *check*, not an assumption: during a step, the alignment crosshair must stay welded to the point of impact. If it drifts during the ease and settles afterward, the pivot is not doing what the commit message claims and that is a finding about `e58aeac`, not about this feature.

### A10 — Nine phases with no user checkpoint. **Process.**

The plan reads as a single implementation pass. Phases 3, 6, and 7 each change what the optic does in ADS, and this project's history is that optical changes are validated by the user in game, on live unsaved editor values, and are frequently wrong on the first attempt in ways no amount of reasoning caught — the aim-offset solve went through three measured-and-rejected methods.

**Fix:** treat Phases 3, 6, and 7 as hard stops for in-game testing before continuing. Phases 0–2 can go in one pass; 8 and 9 touch nothing optical and can go in one pass. That is three delivery points, not nine.

---

## Plan changes forced by the review

1. **Phase 2 gains an ordering constraint** — resolver runs after selection, reads the revision at publish time, holds previous values on a selection-change tick. *(A1)*
2. **Phase 3 grows** — the editor edits the *active variant*, `BuildEditedProfile` rebases, and the save payload carries a variant id and applies as a targeted write. *(A2, A5)*
3. **Phase 3 gains variant ids** and the co-save stores ids, not indices. *(A5)*
4. **The resolver owns magnification outright when variants are on**, and `minZoom`/`maxZoom`/`zoomSpread` are disabled in the UI. *(A3)*
5. **A new Phase 3.5: the `fovMult` ramp probe**, gating whether Phase 4 blends FOV or snaps it. *(A4)*
6. **Reticle discovery moves to `kGameDataReady` + explicit rescan.** *(A6)*
7. **Phase 9 becomes a retarget API** — aperture/reticle override by shape or name, sight-swap trigger, read-only node snapshot for discovery. Pointers resolved to names on receipt. Arbitrary node writes deferred to a stated-contract v2. *(A7)*
8. **Orphan detection log line** when a fresh profile is synthesized for a weapon that has other saved profiles. *(A8)*
9. **Phase 3 acceptance test** includes crosshair stability during a magnification ease. *(A9)*
10. **Three delivery points, not one** — (0,1,2), (3,3.5,4,5,6,7 with stops after 3, 6, 7), (8,9). *(A10)*
11. **Session state goes in the co-save, not a sidecar JSON**, with authored `defaultVariantId`/`defaultReticleFile` in the profile for fresh characters. *(§7)*
12. **Reticle switching composites in the shader rather than driving STS material swaps**, because the material-swap route only works on scopes authored with the TextureLoader/fake-material setup and the corpus is not consistent about it. *(§6)*
