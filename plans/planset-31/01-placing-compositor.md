# Plan 01 — the gather pass and the assembled composite

**Goal:** add a **gather (assembly) pass** that scissor-blits each `Presented` viewport's texture
into its region on one full-window linear-HDR target, and feed that single assembled target into the
existing `SwapChainCompositePass` **unchanged**. Today the composite takes a single `SceneSource` and
blits it full-window behind ImGui. Rather than teach the composite about N regions, the gather pass
assembles the N placements into the one target the composite already consumes: one placement covering
the window is the game's old behavior, zero placements is the editor (the gather clears), and N
quadrant placements is splitscreen — the same two-pass tail for all three. Independent of the
`Viewport` type.

## Why it is its own plan

Keeping placement in a dedicated gather pass isolates it from the composite, which already owns the
display-transfer encode, the ImGui blend, the `DisplayColorSpace`/paper-white/peak-nits resolution,
and the `SetSwapChainTarget` re-target. The composite's input contract — *one linear full-window
source in, ImGui over, encode once* — is left untouched, so the recent HDR/color-space work needs no
edits and cannot regress. Landing the gather pass before the `Application` integration (Plan 02) lets
Plan 02 drive it from the `Presented` set with no further composite change, and lets the editor
(Plan 03) run it with zero placements. Independent of Plan 00, so the two can land in parallel.

## What lands

- **A gather pass assembles placements into one target.** A `CompositePlacement { Ref<ImageView>
  Texture; ViewportRegion Region; }`; the gather pass scissor-blits each placement's texture into its
  region (sampled through set-0 bindless, the region's offset+extent as the scissor and the UV
  remap), in list order, onto an **owned full-window assembly target** in linear HDR (`RGBA16F`),
  clearing the area no placement covers. `SetPlacements(std::span<const CompositePlacement>)` rebinds
  the list each frame, since the `Presented` set and its regions change as viewports register/resize.

- **The composite consumes the assembly target, unchanged.** `SwapChainCompositePass` keeps its
  single-source input; its `SceneSource` is now the gather pass's assembly target rather than a raw
  `SceneRenderer` output. Its ImGui blend, the color-space/paper-white/peak resolution, the single
  display-transfer encode, and `SetSwapChainTarget` are all unchanged — the assembly target is linear
  HDR, so the encode still runs once over the assembled-plus-overlay result.

- **Zero placements = a cleared assembly target.** With an empty list the gather pass writes only the
  clear color; the composite then encodes + overlays ImGui over a clear — the editor's normal state,
  no special case and no "fake" background.

- **The one-viewport case is a 1:1 copy.** A single full-window placement is a straight
  (point-sampled, same-resolution, same-format) copy into the assembly target, so the assembled
  values are bit-identical to sampling the scene output directly — the golden does not move.

- **`MaxPresented` bounds the gather.** Up to a fixed `MaxPresented` placements are reserved;
  registering more `Presented` viewports than the bound is a fatal `VE_ASSERT` (Plan 02 enforces it
  at register time). Per frame the gather (re-)registers exactly K placement textures plus the
  assembly slot, and clears unused slots — so a K-placement frame declares exactly K bindless
  samples, no unbound descriptor.

## Decisions

1. **Assemble, then composite — don't generalize the composite.** Splitscreen makes "one background"
   wrong, but the fix is a gather step that produces the single target the composite already
   consumes, not a placement-aware composite. This keeps the composite's encode/ImGui/HDR contract
   untouched and isolates placement math where it can be tested on its own.

2. **Assemble in linear HDR.** The assembly target is `RGBA16F` so blitting the viewports' linear
   outputs and then encoding once is the same operation as today, just applied per-region; nothing is
   quantized before the encode, and the quadrant seams are exact.

3. **The one-viewport blit is a straight copy, so the golden holds.** A full-window placement copies
   1:1 (no filter, same resolution), so the assembled target holds the same values the composite
   sampled directly today — byte-identical output through Plan 02's migration.

4. **The sample keeps one full-window placement here.** hello-triangle still renders one scene and
   gathers it as a single window-covering placement (it gains the managed primary viewport only in
   Plan 02), so this plan moves no pixels. Zero- and N-placement paths are exercised by Plans 03
   and 04.

## Files

| File | Change |
|---|---|
| `engine/include/Veng/Renderer/GatherPass.h` *(new)* | `CompositePlacement`, `SetPlacements`, the owned assembly target, `Compile`/`Execute`; doc the place-each-into-its-region + zero/N contract and `MaxPresented`. |
| `engine/src/Renderer/GatherPass.cpp` *(new)* | Per-placement scissored blit + dynamic placement-slot registration; the empty-list clear path; the owned `RGBA16F` assembly target (beside the sibling `SceneRenderer.cpp` / `SwapChainCompositePass.cpp`, not under `Backend/`). |
| `engine/assets/core/shaders/` *(gather shader)* | A scissored fullscreen blit (region scissor + UV remap) sampling a placement texture into the assembly target. |
| `engine/include/Veng/Renderer/SwapChainCompositePass.h` | Doc that `SceneSource` is the gather pass's assembly target; otherwise unchanged. |
| `examples/hello-triangle/main.cpp` | Insert the gather pass; its single placement is the scene output, full-window; the composite reads the assembly target. Mechanical; still one full-window scene. |

## Verification

- Clean build; `ctest` green.
- `smoke_golden` does **not** move — the sample gathers one full-window placement (a 1:1 copy) and
  the composite encodes it exactly as before; the output is byte-identical.
- `validation_gate` green under `build-debug` — the dynamic gather registration leaves no unbound
  descriptor (the empty path declares no placement slot; a K-placement frame declares exactly K).
- The zero-placement (editor) and N-placement (splitscreen) paths are covered end-to-end by Plans 03
  and 04; here a build-time confirmation that an empty list clears the assembly target and a
  two-placement list scissors into the correct regions.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
