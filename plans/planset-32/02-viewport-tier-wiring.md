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
  resolution also configures the outer loop. The tiers default to a sensible descending set bounded above
  by the HiDPI-capped baseline (Plan 01) and below by `DynamicResolutionSettings::MinScale`, so the
  outer loop never targets a tier the inner loop would immediately overshoot.

- **`Viewport` holds an `AllocationTierState`** and, each frame after the inner-loop update, calls
  `StepAllocationTier(m_TierState, m_RenderScale, delta, m_TierSettings)`. The frame `delta` is the one
  the viewport already receives via `ViewState::Delta`. The returned tier index maps to a scale; that
  scale becomes the new `AllocationScale()`.

- **A tier change debounces a `SceneRenderer::Resize`.** When the tier index changes, the viewport
  schedules a resize to `round(region · Tiers[index])` (clamped ≥ 1) — reusing the existing
  resize-on-next-`Render` debounce path (`m_PendingExtent`) so the `Resize` lands at a clean point in the
  frame, not mid-record. Because the controller's dwell already makes a change rare and sustained, the
  resize is infrequent; the debounce only ensures it is issued at the same safe point a region resize is.

- **`ViewRenderScale()` recomputes against the live allocation.** With `AllocationScale()` now dynamic,
  `ViewRenderScale() = min(m_RenderScale / AllocationScale(), 1.0)` continues to give the sub-rect
  fraction — and because a tier change keeps `m_RenderScale` constant while `AllocationScale()` drops,
  the pushed `SceneView::RenderScale` jumps from "sub-rect of the big target" to "near-full of the small
  target" with the **same rendered pixel count**: the visual-continuity guarantee, realized.

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

3. **Tiers are bounded by the capped baseline and the inner-loop floor.** `Tiers[0]` is the Plan 01
   baseline (not 1.0× backing on HiDPI), and the smallest tier is ≥ `MinScale`, so the outer loop never
   picks an allocation the inner loop can neither fill nor fit inside. This keeps the two loops' ranges
   nested rather than fighting.

4. **The manual knob becomes an override, not a competitor.** With the controller on, the sample's slider
   either reads out the auto value or, when touched, disables the controller and holds (the existing
   `Viewport` manual-hold behavior). There is no mode where both write the scale.

5. **Resize-on-tier-change is acceptable as-is.** The dwell + hysteresis make a tier change a rare,
   already-sustained event, so issuing the `Resize` inline (at the debounced point) is fine; deferring it
   further to a scene transition / static camera is a named future refinement, not needed for correctness.

## Files

| File | Change |
|---|---|
| `engine/include/Veng/Renderer/Viewport.h` | `AllocationTierSettings` in the DRS opt-in; `AllocationTierState m_TierState`; dynamic `AllocationScale()`; `GetAllocationScale()` (full Doxygen). |
| `engine/src/Renderer/Viewport.cpp` | Step the tier each frame after the inner-loop update; map tier→scale; debounce a `SceneRenderer::Resize` on tier change; recompute `ViewRenderScale()` against the live allocation. |
| `engine/include/Veng/Application.h` | Thread the tier settings through `ManagedViewportInfo::DynamicResolution` (or a sibling field). |
| `engine/src/Application.cpp` | Pass the tier settings into the managed viewport's `SetDynamicResolution`. |
| `examples/hello-triangle/main.cpp` | Opt the managed viewport into the tier controller; turn the manual render-scale drag into a read-out / override; show the sub-rect scale + allocation tier + allocation extent in Stats. |
| `tests/gpu/…` | A headless test: a viewport with the tier controller, fed a synthetic sustained-low `RenderScale` (via the manual/override path or by driving `UpdateDynamicResolution` with an over-budget frame time), steps down a tier and the `SceneRenderer` allocation (its `GetOutput()` extent) shrinks accordingly after the dwell; a recovery grows it back after the longer dwell. Assert no resize occurs on a one-frame spike. |

## Verification

- Clean build; `ctest` green. The new gpu test passes; skips cleanly with no ICD (label `gpu`).
- `smoke_golden` does **not** move — the smoke path renders a fixed pose at `HeadlessExtent` with the
  controller inert (no GPU timing / fixed scale in smoke), so the allocation stays at the baseline and
  the capture is byte-identical.
- The windowed sample auto-sizes its allocation under load (verify by enabling TAA on the HiDPI machine:
  the allocation now steps down — the previously-fruitless min-resolution behavior — and frame time
  recovers, the originating motivation).
- `validation_gate` green under `build-debug` — a tier-change `Resize` is the existing `SceneRenderer::Resize`
  path (retire + re-register + recompile), already covered; no new descriptor or barrier surface.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
