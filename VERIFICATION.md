# MagnaScope staged in-game verification

MagnaScope defaults to the production Stage 4 renderer. Diagnostic stages are
kept for controlled regression isolation. Change `VerificationStage` in
`F4SE/Plugins/MagnaScope.ini`, restart Fallout 4, and never infer visual
correctness from logs alone.

Every stage must use at least two automatic STS weapons with different scope
OMODs. Perform ten aim-in/aim-out cycles, fire semi-automatic and automatic
weapons while aimed, switch weapons and scope attachments, then save and reload
after returning to the hip. Stop on a crash, black lens, stuck input,
persistent FOV/camera mutation, or new render-state failure.

## Stage 0: profile detection only

Expected:

- automatic STS profiles are selected by weapon and equipped scope identity;
- the log reports that MagnaScope rendering hooks are disabled;
- aiming is visually identical to STS without MagnaScope;
- no FOV, camera, projection, culling, or rendering mutation occurs.

## Stage 1: verified STS geometry

Set `VerificationStage=1`.

Expected:

- the log names the live `ScopeFade` geometry beneath `ScopeAiming`;
- aperture and reticle projections are finite and follow the optic's authored
  alignment rather than assuming screen center;
- no zoom/camera override or Direct3D draw occurs;
- aiming remains visually unchanged.

## Stage 2: override lifecycle

Set `VerificationStage=2`.

Expected:

- authored FOV and camera values seed every new scope profile;
- an edited override is active before aim-in samples the sighted zoom data;
- live changes remain visible and do not snap back while editing;
- weapon/scope switches and save/load restore the authored values without
  affecting unrelated weapons.

## Stage 3: verified source anchors

Set `VerificationStage=3`.

With `TAACapture=0`, the F4SE Menu Framework pre-UI callback must capture the
displayed world frame without drawing. With `TAACapture=1` and TAA active, the
guarded OG callback must capture the TAA output. When TAA is unavailable, the
framework anchor remains the verified fallback. In either case there must be
no visual change, black frame, flicker, UI corruption, or render-state leak.

## Stage 4: full STS optical composite

Set `VerificationStage=4`, `GeometryProbe=1`, and
`GeometryMagnification=1`.

Expected:

- magnification is visible only through the authored `ScopeFade` aperture;
- the surrounding view remains unzoomed when the sighted FOV multiplier is 1;
- the lens and reticle remain aligned with off-center, inertia-driven STS
  geometry during aim transitions, recoil, firing, and weapon movement;
- at rest, the lens is centered and a complete dark rim is visible;
- Vignette Reach changes rim intrusion and Vignette Sharpness changes its edge;
- Eye Box Radius changes travel forgiveness, not the resting rim size;
- transient eye motion adds a directional crescent and scene/reticle parallax
  without settled drift, heading dependence, breathing, or snapping;
- reticle size, offset, magnification, parallax, and shadow participation work
  independently from scene magnification;
- firing never drops the aperture or resets magnification;
- profile save/reload uses `Data/F4SE/Plugins/MagnaScope/Auto` and never creates
  data in another framework's namespace;
- TAA and non-TAA anchors are tested separately, followed by the active ENB,
  upscaler, and frame-generation configuration.

Only visible success across this matrix qualifies the renderer as verified.
