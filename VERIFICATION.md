# MagnaScope staged in-game verification

MagnaScope defaults to `VerificationStage=0`. Do not skip stages. Change the
value in `F4SE/Plugins/MagnaScope.ini`, restart Fallout 4, and complete the
current stage before advancing.

Each stage must be tested with:

- one automatic STS weapon;
- one weapon selected by an explicit FTS JSON profile;
- at least ten aim-in/aim-out cycles per weapon;
- an equip swap while at the hip and while aimed;
- one save and reload after returning to the hip.

Stop on any crash, black lens, stuck input, persistent FOV/camera change, or
new error in `MagnaScope.log`. Do not advance to the next stage.

## Stage 0: profile detection only

Expected:

- the log reports `verification stage=0`;
- the log reports that MagnaScope DX11 hooks are disabled;
- automatic STS and explicit FTS profiles are selected;
- aiming behaves exactly as it does without MagnaScope;
- no FOV, camera, node-culling, projection, or rendering work occurs.

This stage proves profile and attachment identity handling without touching
the crash-site projection, weapon zoom data, or D3D.

## Stage 1: verified projection

Set `VerificationStage=1`.

Expected:

- the log names the selected STS aperture geometry, normally `ScopeFade:0`;
- the aperture comes from the live `ScopeViewParts` subtree;
- the projected pixel coordinates and positive depth are finite;
- the projected center may be off-center and should match the STS optic's
  existing alignment;
- no zoom/camera override is applied;
- DX11 hooks remain disabled;
- aiming still looks unchanged.

This stage projects the authored aperture bound center with the original FTS
first-person camera convention. Negative camera-space Z is forward. It does
not use the worldspace HUD projection helper. The removed hard-coded target
`A37940` must never appear as a callable relocation.

## Stage 2: override lifecycle

Set `VerificationStage=2`.

Expected:

- the log reports the selected-profile override lifecycle;
- a configured FOV/camera override is already active when aim-in begins;
- changing weapons restores the previous weapon's authored zoom data;
- saving and loading does not persist a temporary override;
- DX11 hooks remain disabled.

This stage proves weapon-data ownership and restoration independently of the
renderer.

## Stage 3a: framework Present source capture

Set `VerificationStage=3`.

Expected:

- the log reports that MagnaScope-owned DX11 hooks are disabled;
- the F4SE Menu Framework before-render callback dispatches;
- ADS produces `Verification stage 3a completed framework-present source capture`;
- the reported dimensions match the displayed back buffer;
- the readback reports a center hash and a nonzero byte count;
- no scope composite or optical effect is visible by design;
- aiming retains the exact Stage 2b FOV and camera behavior;
- no black frame, black lens, flicker, UI corruption, input failure, or crash
  occurs.

This stage copies the displayed back buffer without installing MagnaScope's
Present, ResizeBuffers, DrawIndexed, or TAA hooks. It creates no shader,
constant buffer, shader-resource view, or render-target view. It performs no
scope draw and binds no game render target.

## Stage 3b: TAA source capture

Do not begin this stage until Stage 3a passes. Keep
`VerificationStage=3`, set `TAACapture=1`, and restart Fallout 4.

Expected with `sAntiAliasing=TAA`:

- the guarded OG TAA callback installs without replacing another DLL's hook;
- the log reports `Stage 3b TAA callback observed: active=true`;
- ADS produces `Verification stage 3 TAA source capture completed`;
- the reported dimensions, format, sample count, center hash, and nonzero byte
  count describe the bound TAA output;
- the framework Present capture does not also run during the same ADS frame;
- no shader, render-target view, or draw is created by Stage 3;
- visuals remain exactly the same as Stage 2b.

If TAA is inactive or an upscaler owns antialiasing, the log reports that
state and the already verified framework Present capture remains active.
This fallback is a valid safety result, but it does not verify the TAA source.

## Stage 4: full composite

Set `VerificationStage=4`.

Expected:

- magnification is clearly visible only inside the lens;
- the surrounding view remains unzoomed when the full-screen FOV override is
  configured to 1.0;
- eye-box shadow, vignette, parallax, chromatic aberration, and fisheye
  controls are visibly functional;
- source and target probe hashes differ inside the lens;
- pixels outside the lens remain unchanged;
- automatic STS and explicit FTS JSON paths both work;
- TAA and non-TAA anchors are tested separately;
- ENB/upscaler/frame-generation combinations are tested one at a time.

Only Stage 4 passing the complete matrix qualifies the renderer as verified.
