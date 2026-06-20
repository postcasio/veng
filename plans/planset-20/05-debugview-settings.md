# Plan 05 — CSM settings + debug-UI exposure

**Goal:** the authoring surface for CSM — the `SceneRendererSettings` knobs (`CascadeCount`,
`CascadeSplitLambda`, and the lowered per-cascade `ShadowResolution` default) driving the
recompile, and the example debug-UI exposure of the `DebugView::Cascades` arm (which lands in
Plan 04 with the selection logic it visualizes).

## What lands

### `DebugView::Cascades` exposure (the arm lands in Plan 04)

The `DebugView::Cascades` arm itself — tinting each fragment by its selected cascade — lands in
**Plan 04**, alongside the selection logic it reuses, so that plan's golden move is pinned by a
CSM-distinguishing assertion. This plan does not re-introduce it; it only **exposes** it in the
example debug UI (the `modeNames` combo, below) so the arm is selectable. The `Shadows` arm shows
the cascade **atlas**; its blit was repointed off bindless onto the dedicated-set bound-view seam
in Plan 03 (when the atlas moved), with an ordinary sampler visualizing raw depth.

### CSM settings ([SceneRenderer.h](../../engine/include/Veng/Renderer/SceneRenderer.h))

`SceneRendererSettings` gains:

```cpp
// The number of shadow cascades (clamped to [1, Renderer::MaxCascades]). It sizes
// the shadow atlas grid (Plan 03: min(Count,2)×ceil(Count/2) tiles) and recompiles.
// More cascades trade memory/fill for tighter near-camera texel density.
u32 CascadeCount = 4;

// The PSSM split blend: 0 = uniform splits (even world spacing), 1 = logarithmic
// (even perspective spacing, tighter near cascades). A recompile-safe value (it
// only re-derives the per-frame matrices) kept a setting for the authoring surface,
// like Exposure.
f32 CascadeSplitLambda = 0.85f;
```

`ShadowResolution` (already present) is now the **per-cascade tile** edge, and its **default
drops from 2048 to 1024** (set in this plan): a default 4-cascade atlas is then 2048² — the same
footprint as the prior single 2048 map, not a 4× regression. `CascadeCount` sizes the atlas grid,
so it (with `ShadowResolution`) is a `Configure`/`Resize` recompile knob; `CascadeSplitLambda`
only changes the per-frame `ComputeCascades` inputs, so it is recompile-safe (applied each
`Execute`), kept a setting for consistency with `Exposure`. The `Shadows` toggle (insert/remove
the pass) is unchanged.

`ComputeCascades` (Plan 02) already takes these via its `CascadeSettings`; `Execute` fills
that struct from the settings (Plan 03 wired the call — this plan adds the knobs feeding it).

## Decisions

1. **`Cascades` is a debug arm, not a per-frame overlay.** It re-wires the pass set through
   `Configure` like every other `DebugView` arm (the settings-drive-recompile proof), so it
   matches the established debug-view mechanism rather than a runtime toggle in the lit shader.

2. **`CascadeCount` recompiles; `CascadeSplitLambda` does not.** Count changes the atlas
   sizing (a `Configure` concern); lambda only reshapes the per-frame matrices (a per-frame
   concern). The split mirrors the `ShadowResolution`-vs-bloom-threshold line the renderer
   already draws.

3. **No per-cascade resolution.** All cascades share `ShadowResolution`. Independent
   per-cascade resolutions (a higher-res cascade 0) is a refinement not worth the settings
   surface here.

## Files

| File | Change |
|---|---|
| `engine/include/Veng/Renderer/SceneRenderer.h` | `CascadeCount` + `CascadeSplitLambda` settings; lower the `ShadowResolution` default to 1024. (`DebugView::Cascades` lands in Plan 04.) |
| `engine/src/Renderer/SceneRenderer.cpp` | Feed `CascadeCount`/`CascadeSplitLambda` into `ComputeCascades`; resize the atlas grid on a `CascadeCount` change (the deferred `Release()` window covers the old atlas). |
| `examples/hello-triangle/` (the debug UI) | Expose the `Cascades` view + the two knobs in the debug panel (Veng::UI), like the other settings. **Resync the `modeNames` combo array** — it is a fixed size-4 (`{Final, Albedo, Normal, Depth}`) already stale against the current enum (missing the Occlusion/Roughness/Metallic/AO/Shadows arms); bring it fully in line with `DebugView` so every arm is selectable, not just append `Cascades`. |

## Verification

- Clean build; `ctest` green across the bands.
- **`gpu` band:** changing `CascadeCount` resizes the atlas grid and recompiles (the
  CSM-selection pin itself lives in Plan 04's `DebugView::Cascades` test); changing
  `CascadeSplitLambda` shifts the split bands with **no** recompile (applied per `Execute`).
  **Validation gate under `VE_DEBUG`** clean across the recompiles — no stale atlas sample after
  a `CascadeCount` change resizes the atlas (the deferred `Release()` window covers the old one).
- **`smoke_golden`** is unaffected by this plan (the smoke pose renders `DebugView::Final` with
  the default settings; the debug arm and knobs do not change the default capture). If the
  default `CascadeCount`/`Lambda` differ from Plan 03's hardcoded values used to bless the
  golden, re-bless once here with the change stated.
- The smoke PPM is the correct size and the launcher exits 0.
