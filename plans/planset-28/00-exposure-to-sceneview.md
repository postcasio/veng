# Plan 00 — move Exposure to SceneView (per-frame tonemap exposure)

**Goal:** relocate `Exposure` from `SceneRendererSettings` (a `Configure`-recompile
topology knob) to `SceneView` (a per-frame value the tonemap reads each `Execute`), so
exposure tunes live without a recompile. Independent of the bloom rework — it touches only
the tonemap read path and the one exposure test — and runs in parallel with Plan 01. It is
the prerequisite for Plan 04's live exposure slider.

## Why it is its own plan

`Exposure` has nothing to do with the bloom pyramid; it rides into this planset only so
Plan 04 can expose a live slider. Folding it into Plan 01 would load an unrelated knob move
(plus a tonemap-path edit and a test rewrite) onto the single golden-regenerating commit. Split
out, Plan 01 stays purely bloom, and this change is independently verifiable: it moves no
golden.

## What lands

- **`Exposure` moves from `SceneRendererSettings` to `SceneView`.** Its doc already flags it
  recompile-safe and notes it lives on `Settings` only "to exercise the Settings surface"
  ([SceneRenderer.h:153](../../engine/include/Veng/Renderer/SceneRenderer.h:153)); making it a
  true per-frame `SceneView` value lets the tonemap read it each `Execute` without a
  `Configure`. Re-cut its doc comment to the per-frame contract.
- **The tonemap pass reads `Exposure` from the view**, not the stored settings — the one
  producer-side change.
- **The example adds `.Exposure = ...` to its per-frame `view{}`** initializer
  ([main.cpp:186](../../examples/hello-triangle/main.cpp:186)); the literal value is unchanged
  here (Plan 04 wires it to a UI member).

## The exposure test rewrite

The existing exposure `gpu` test ([scene_renderer.cpp:1230](../../tests/gpu/scene_renderer.cpp:1230))
is built on `Exposure` being a `Configure` knob — it renders with `.Settings = {.Exposure =
0.25f}`, then **`Configure({.Exposure = 4.0f})`** and asserts the center texel brightens
([scene_renderer.cpp:1266](../../tests/gpu/scene_renderer.cpp:1266),
[:1277](../../tests/gpu/scene_renderer.cpp:1277)). With `Exposure` per-frame this structure is
wrong: rewrite it to render two frames with `SceneView.Exposure = 0.25f` then `= 4.0f` and
**no `Configure` between them**, asserting the result brightens — which now *also* proves
exposure tunes without a recompile.

## Decisions

1. **Per-frame, no recompile.** Exposure is a continuous tonemap scalar; driving it through
   `SceneView` matches every other per-frame value and removes a spurious recompile on every
   exposure drag-frame.

2. **The only knob-location change in the planset.** The bloom per-frame knobs already live
   on `SceneView`; this plan moves the one remaining tonemap knob that belonged there.

## Files

| File | Change |
|---|---|
| `engine/include/Veng/Renderer/SceneRenderer.h` | Move `Exposure` from `SceneRendererSettings` to `SceneView`; re-cut its doc comment to the per-frame contract. |
| `engine/src/Renderer/SceneRenderer.cpp` | The tonemap pass reads `Exposure` from the view instead of the stored settings. |
| `tests/gpu/scene_renderer.cpp` | Rewrite the exposure test from a `Configure`-bracketed pair to a per-frame `SceneView.Exposure` pair asserting brightening with **no** `Configure`. |
| `examples/hello-triangle/main.cpp` | Add `.Exposure` to the per-frame `view{}` initializer (literal unchanged; Plan 04 wires the member). |

## Verification

- Clean build; `ctest` green across the bands.
- `gpu`: the rewritten exposure test brightens with a higher per-frame `SceneView.Exposure`
  and calls **no** `Configure` between the two renders (proving live, recompile-free tuning).
  Audit every other caller setting `Settings.Exposure` (the editor settings inspector, any
  serialized `SceneRendererSettings`) and migrate it in the same pass; confirm nothing
  persists an Exposure field that no longer exists on `SceneRendererSettings`.
- `smoke_golden` does **not** move (the default scene's exposure is unchanged).

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
