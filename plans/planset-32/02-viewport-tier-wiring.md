# Plan 02 — wiring the allocation tier into the viewport

**Goal:** make the allocation **follow** the sustained sub-rect. `Viewport` tracks a long EMA of its
per-frame `RenderScale`, steps the allocation tier through Plan 00's `StepAllocationTier`, and on a tier
change debounces a `SceneRenderer::Resize(round(region · tier))` — replacing the static
`AllocationScale() == MaxScale` that hand-picks the allocation today. **Migrate hello-triangle** so the
managed viewport allocates automatically, and expose the live tier in its Stats window. Depends on Plan
00 (the controller) and reads atop Plan 01 (a capped baseline).

## Why it is its own plan

This is where the pure controller meets the expensive side effect, and it is the plan that has to get
the **safety** right: a `Resize` retires every target, re-registers bindless, and recompiles the graph,
so wiring it to the wrong signal would reintroduce exactly the thrash the controller's math forbids.
Splitting it from the controller (Plan 00) keeps the math reviewable without the `Resize` plumbing, and
splitting it from the HiDPI cap (Plan 01) keeps the baseline decision separate from the follower. It is
also the migration plan — the sample's manual render-scale knob becomes an automatic allocation.

## Background — the seam being replaced

`Viewport::AllocationScale()` returns the static `m_DynamicResolution->MaxScale` (or the fixed
`m_RenderScale` with DRS off); the owned `SceneRenderer` is allocated at `round(region · AllocationScale())`
and `ViewRenderScale()` (= `m_RenderScale / AllocationScale()`) is the sub-rect fraction pushed into
`SceneView::RenderScale`. The inner loop already updates `m_RenderScale` each frame from GPU frame time
(`UpdateDynamicResolution`). This plan makes `AllocationScale()` **dynamic** — driven by the tier
controller — and triggers the `Resize` when it changes.

## What lands

- **`AllocationTierSettings` on the viewport's dynamic-resolution config.** Extend the opt-in
  (`Viewport::SetDynamicResolution`, `ManagedViewportInfo::DynamicResolution`) so enabling dynamic
  resolution also configures the outer loop. The tiers default to Plan 00's `{1.0, 0.75, 0.5}` —
  **absolute** scales of the region, with the Plan 01 `MaxAllocationScale` applied **outside** them (see
  the formula below), and the floor (`0.5`) strictly above the lowered `MinScale` (`0.25`) so the outer
  loop never targets a tier the inner loop cannot fit inside.

- **`Viewport` holds an `AllocationTierState`** and, each frame after the inner-loop update, calls
  `StepAllocationTier(m_TierState, m_RenderScale, delta, m_TierSettings)`. The frame `delta` is the one
  the viewport already receives via `ViewState::Delta`. The returned tier index maps to a scale; that
  scale becomes the new `AllocationScale()`.

- **A tier change debounces a `SceneRenderer::Resize`.** When the tier index changes, the viewport
  schedules a resize to `round(region · MaxAllocationScale · Tiers[index])` (clamped ≥ 1) — the single
  allocation-extent formula from Plan 01, with `AllocationScale() == Tiers[index]`. It reuses the existing
  resize-on-next-`Render` debounce path (`m_PendingExtent`) so the `Resize` lands at a clean point in the
  frame, not mid-record. Because the controller's dwell already makes a change rare and sustained, the
  resize is infrequent; the debounce only ensures it is issued at the same safe point a region resize is.

- **`ViewRenderScale()` recomputes against the live allocation.** With `AllocationScale()` now dynamic,
  `ViewRenderScale() = min(m_RenderScale / AllocationScale(), 1.0)` continues to give the sub-rect
  fraction — and because a tier change keeps `m_RenderScale` constant while `AllocationScale()` drops, the
  pushed `SceneView::RenderScale` jumps from "sub-rect of the big target" to "near-full of the small
  target" with the **same rendered pixel count**: the visual-continuity guarantee, realized. This holds
  *exactly* (not just approximately) because Plan 00's down-step guard (decision 6) only shrinks the
  allocation once `currentRenderScale ≤ Tiers[next]`, so after the `Resize` the ratio is already ≤ 1.0 and
  never hits the `min(…, 1.0)` clamp that would otherwise crop the sub-rect and pop. (The only residual is
  sub-pixel rounding in `round(region · …)` at very small extents.)

- **A getter for the live tier.** `[[nodiscard]] f32 GetAllocationScale() const` (or expose the tier
  index) so the sample/editor can show it. The existing `GetRenderScale()` (sub-rect) stays; the two
  together read as "rendering at S of an allocation that is T of the window."

- **Migrate hello-triangle.** The managed viewport opts into dynamic resolution with the tier controller;
  the manual "Render scale" drag becomes a *read-out* (or a manual override that disables the controller,
  mirroring `SetDynamicResolution`/the manual hold), and the Stats window shows both the sub-rect scale
  and the allocation tier (and the resulting allocation extent), so the auto-sizing is visible.

## Decisions

1. **Drive the tier from `m_RenderScale`, after the inner loop.** The outer loop reads the inner loop's
   freshly-updated output each frame, so the EMA is over the actual operating point and the two loops are
   never inconsistent. No separate measurement, no extra timing query.

2. **Reuse the existing resize debounce.** A tier change routes through the same `m_PendingExtent`
   next-`Render` resize the region-change path already uses, so there is exactly one place a viewport's
   `SceneRenderer::Resize` is issued and one safe point it lands. The controller decides *whether*; the
   debounce decides *when within the frame*.

3. **Three multiplicative scales, `MaxAllocationScale` outermost.** The allocation extent is
   `round(region · MaxAllocationScale · Tiers[tierIndex])` and the rendered sub-rect is that times
   `ViewRenderScale`. The HiDPI cap (Plan 01) is the steady-state ceiling, the tier is the outer loop's
   coarse follower below it, and the sub-rect is the inner loop's fine knob inside that — the three nest
   rather than fight. Tiers are absolute (`{1.0, 0.75, 0.5}`), not relative to the cap, so the cap and the
   tier compose by multiplication at one site (`ExtentForScale`).

4. **The manual knob becomes an override, not a competitor.** With the controller on, the sample's slider
   either reads out the auto value or, when touched, disables the controller and holds (the existing
   `Viewport` manual-hold behavior). There is no mode where both write the scale.

5. **Resize-on-tier-change pays a real but rare hitch.** A tier-change `Resize` is the existing
   `SceneRenderer::Resize`: it retires every extent-sized render-graph target, re-registers each into
   bindless, and rebuilds + re-`Compile()`s the graph — a multi-millisecond stall on the resize frame
   (the rendered *pixel count* is continuous across it, per the visual-continuity guarantee, but the
   *frame time* is not). The dwell + hysteresis make the event rare and already-sustained, so paying the
   hitch inline at the debounced point is acceptable; deferring it further to a scene transition / static
   camera is the named future refinement, not needed for correctness.

## Files

| File | Change |
|---|---|
| `engine/include/Veng/Renderer/Viewport.h` | `AllocationTierSettings` in the DRS opt-in; `AllocationTierState m_TierState`; dynamic `AllocationScale()`; `GetAllocationScale()` (full Doxygen). |
| `engine/src/Renderer/Viewport.cpp` | Step the tier each frame after the inner-loop update; map tier→scale; debounce a `SceneRenderer::Resize` on tier change; recompute `ViewRenderScale()` against the live allocation. |
| `engine/include/Veng/Application.h` | Thread the tier settings through `ManagedViewportInfo::DynamicResolution` (or a sibling field). |
| `engine/src/Application.cpp` | Pass the tier settings into the managed viewport's `SetDynamicResolution`. |
| `examples/hello-triangle/main.cpp` | Opt the managed viewport into the tier controller; turn the manual render-scale drag into a read-out / override; show the sub-rect scale + allocation tier + allocation extent in Stats. |
| `tests/gpu/…` | A headless test: a viewport with the tier controller, fed a synthetic sustained-low `RenderScale` **via the manual/override path** (not `UpdateDynamicResolution`, which early-returns when `IsGpuTimingSupported()` is false — the headless device has no timing, so it would no-op), steps down a tier and the `SceneRenderer` allocation (its `GetOutput()` extent) shrinks accordingly after the dwell; a recovery grows it back after the longer dwell. Assert no resize occurs on a one-frame spike. |

## Verification

- Clean build; `ctest` green. The new gpu test passes; skips cleanly with no ICD (label `gpu`).
- `smoke_golden` does **not** move — the smoke path renders a fixed pose at `HeadlessExtent`, and the
  controller is provably inert there: with GPU timing unsupported the inner loop holds `m_RenderScale` at
  the configured ceiling (= `Tiers[0]`), so the outer-loop EMA sits at `Tiers[0]` and never reaches a
  down-threshold. The allocation stays at the baseline and the capture is byte-identical.
- The windowed sample auto-sizes its allocation under load (verify by enabling TAA on the HiDPI machine:
  the allocation now steps down — the previously-fruitless min-resolution behavior — and frame time
  recovers, the originating motivation).
- `validation_gate` green under `build-debug` — a tier-change `Resize` is the existing `SceneRenderer::Resize`
  path (retire + re-register + recompile), already covered; no new descriptor or barrier surface.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
