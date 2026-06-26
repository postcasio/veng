# Plan 00 — lift the resolution cap

**Goal:** stop the sample from clamping the render allocation to half the swapchain backing extent.
Default `MaxAllocationScale` to `1.0` so the managed viewport renders at native HiDPI resolution,
and let the allocation-tier outer loop discover the operating point under load — which is what
planset-32 built it to do. Independent of every other plan in this set.

## The problem

[`examples/hello-triangle/main.cpp`](../../examples/hello-triangle/main.cpp) sets

```cpp
.MaxAllocationScale = smoke ? 1.0f : 0.5f,
```

`MaxAllocationScale` is the outermost of the three multiplicative scales: the allocation extent is
`round(region · MaxAllocationScale · GetAllocationScale())`
([`ExtentForScale`](../../engine/src/Renderer/Viewport.cpp)). A managed viewport tracks the **full
swapchain framebuffer extent** — 2× the logical window on a HiDPI display — so the `0.5` clamps
every render-graph image back to logical-point resolution. On a retina display that is a blurry,
permanently-half-resolution render: native backing pixels are never rendered. The `0.5` was
intended as a HiDPI baseline budget, but it doubles as a hard ceiling that the adaptive controllers
can never rise above.

The outer-loop **allocation tier** ([`StepAllocationTier`](../../engine/include/Veng/Renderer/DynamicResolution.h))
already exists to fold the sub-rect scale into a multi-second EMA and step the allocation down a
quantized tier under sustained load. The fixed `0.5` pre-empts that discovery entirely.

## What lands

- **Sample default → `1.0`.** Drop the `smoke ? 1.0f : 0.5f` split; the windowed app uses the same
  full-native ceiling smoke already uses. The tier controller (`{1.0, 0.75, 0.5}`) discovers downward
  under load; the inner loop's per-frame sub-rect absorbs spikes. The hand-picked cap is gone.
- **Engine documentation re-framed.** The engine CLAUDE.md and the
  [`DynamicResolution.h`](../../engine/include/Veng/Renderer/DynamicResolution.h) /
  [`Viewport.h`](../../engine/include/Veng/Renderer/Viewport.h) doc comments currently describe
  `MaxAllocationScale` as "the HiDPI baseline budget" whose point is to bring the allocation "back to
  logical-point resolution rather than silently supersampling every render-graph image to the backing
  pixels." That framing is what justified the `0.5`. Rewrite it: `MaxAllocationScale` is a **ceiling**
  on the allocation relative to the backing extent, defaulting to `1.0` (full native); rendering at
  the backing pixels is **native resolution on a HiDPI display, not supersampling**, and reclaiming
  footprint under load is the tier outer loop's job, not the cap's. The cap stays available for an
  app that deliberately wants a lower ceiling (a fixed perf budget), but it is no longer the default
  posture.

## Decisions

1. **No DPI query.** The engine gains no `Window::GetContentScale`; the cap defaults to `1.0` and the
   tier loop does the discovering. A DPI-derived cap is a possible later refinement but adds a window
   query for a value the outer loop already converges to.

2. **The smoke golden is unaffected.** Smoke already ran at `1.0`; only the windowed default changes.
   `smoke_golden` renders the same fixed pose at the same scale, so no golden regeneration.

3. **The cap stays in the API.** `MaxAllocationScale` remains on `ManagedViewportInfo` /
   `ViewportInfo` — lowering it is a legitimate way to set a fixed perf ceiling. The change is the
   **default** and the **framing**, not the removal of the knob.

## Verification

Clean build, full `ctest`, `smoke_golden` and `validation_gate` green. Windowed sample renders at
native resolution on a HiDPI display (visually sharp); under a forced sustained load the tier read-out
in Stats steps down from tier 0, confirming the outer loop discovers the operating point with the cap
at `1.0`.
