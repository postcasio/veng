# Plan 00 — the allocation-tier controller

**Goal:** add a pure, device-free **outer-loop** controller beside the existing inner-loop one in
[`DynamicResolution.h`](../../engine/include/Veng/Renderer/DynamicResolution.h): given a long-running
signal of the per-frame sub-rect scale, decide what **quantized allocation tier** the render targets
should be sized at — moving across coarse tiers only through a **hysteresis band** and only after a
**sustained dwell**, so the decision changes at most once every few seconds and never oscillates.
**Purely additive** — no caller wires it yet (Plan 02 does); this plan delivers the math and its tests.

## Why it is its own plan

The whole anti-thrash guarantee lives in this function: that the allocation cannot oscillate is a
property of *how the tier is decided*, not of how it is applied. Isolating it as pure math makes that
property **unit-testable** — feed a synthetic scale trace + a timestep and assert the tier does not
flip on noise, honors the dwell, and respects the hysteresis gap — before any `Viewport` state, EMA
plumbing, or `SceneRenderer::Resize` depends on it. It mirrors `ComputeDynamicResolutionScale`: a
settings struct + a pure step, reviewable against the inner loop's contract on its own.

## What lands

- **`AllocationTierSettings` — the outer-loop tuning.**
  - `std::array<f32, N>` (or a small fixed `vector<f32>`) **`Tiers`** — the allowed allocation scales,
    descending, e.g. `{1.0f, 0.75f, 0.5f}`. The first is the baseline (full allocation); the last is the
    floor. Tier *index* is the controller's state, not the float.
  - `f32 EmaHalfLifeSeconds = 2.0f` — the half-life of the long EMA the controller folds the per-frame
    sub-rect scale into. This is what makes the decision react to the *sustained* operating point, not
    an instantaneous spike; converted to a per-update alpha from the frame `deltaSeconds`.
  - `f32 DownMargin = 0.03f` — step **down** a tier when the sustained scale falls below
    `Tiers[next] + DownMargin` (i.e. the sub-rect is consistently using almost all of the next-smaller
    tier, so the current larger allocation is wasted).
  - `f32 UpHysteresis = 0.12f` — step **up** a tier only when the sustained scale rises above
    `Tiers[current] + UpHysteresis`. The gap between the down-threshold and this up-threshold is the
    dead band a scene parked near a boundary lives in without flipping.
  - `f32 ShrinkDwellSeconds = 1.5f` / `f32 GrowDwellSeconds = 5.0f` — the asymmetric dwell. The
    threshold condition must hold continuously for this long before the tier actually moves: quicker to
    shrink (the sub-rect is already absorbing the load, so deferring the reclaim is cheap and the hitch
    is paid when the load is clearly sustained), slow to grow (eager growth is what thrashes).

- **`AllocationTierState` — the caller-held controller state.**
  - `f32 SustainedScale` — the long EMA of the sub-rect scale (seeded to `Tiers[0]` = 1.0).
  - `u32 TierIndex` — the current tier index (seeded to 0 = baseline).
  - `f32 ShrinkTimer` / `f32 GrowTimer` — seconds the down/up threshold has held continuously; reset to
    0 whenever the condition lapses.

- **`StepAllocationTier(AllocationTierState&, f32 currentRenderScale, f32 deltaSeconds, const AllocationTierSettings&) → u32`**
  — folds `currentRenderScale` into `SustainedScale` (EMA over `deltaSeconds`), advances the dwell
  timers against the down/up threshold conditions, moves `TierIndex` by **at most one step** when a
  timer crosses its dwell, and returns the resulting tier index. **Pure** (mutates only the passed-in
  state, touches no device, no clock — `deltaSeconds` is supplied), so it is deterministic and testable.
  A non-positive `deltaSeconds` (no frame this tick) holds the state. The returned index maps to a scale
  via `Tiers[index]`; the caller multiplies the region extent by it to get the allocation extent.

## Decisions

1. **The input is the inner loop's output, not a fresh measurement.** The controller folds
   `m_RenderScale` (the sub-rect scale the inner loop already computed from GPU frame time) into its EMA.
   That scale *is* the perf response; reusing it means the outer loop needs no second timing path and is
   guaranteed consistent with the inner loop. The long EMA is the "what is the scene sustaining" signal
   the conversation called for.

2. **Tier index is the state; the float is derived.** Carrying the index (not the scale) makes
   hysteresis trivial — "step down" / "step up" are `±1` on an index into a sorted array — and makes
   "at most one tier per update" a one-line clamp. It also keeps quantization exact (no float drift
   accumulating a phantom tier).

3. **One step per update, asymmetric dwell.** Moving at most one tier per call, gated by a dwell timer
   that resets the instant the threshold lapses, is the mechanism that forbids oscillation: a transient
   dip never reaches the dwell, and crossing back requires re-accumulating the *other* timer across the
   hysteresis gap. Down is faster than up because shrinking is cheap-to-defer reclamation and growing is
   the thrash risk.

4. **Pure and clock-free.** Like `ComputeDynamicResolutionScale`, the function takes its time delta and
   mutates only caller state — no `Time::Now`, no device. That is what lets a unit test drive a
   thousand-frame trace deterministically and assert the tier curve.

5. **Lives beside the inner loop.** `DynamicResolution.h` is already "the adaptive render-resolution
   controller"; the outer loop is the same subsystem at a slower timescale, so it shares the header
   rather than spawning a parallel one. The two structs read as inner/outer halves of one story.

## Files

| File | Change |
|---|---|
| `engine/include/Veng/Renderer/DynamicResolution.h` | Add `AllocationTierSettings`, `AllocationTierState`, and the inline `StepAllocationTier` (full Doxygen, in the house style of the existing `ComputeDynamicResolutionScale`). |
| `tests/unit/…` | A new pure-math case: drive `StepAllocationTier` over synthetic scale traces. Assert (a) a steady scale parked just above a boundary never changes tier (no oscillation); (b) a sustained drop steps down only after `ShrinkDwellSeconds`, one tier at a time; (c) a recovery steps up only after `GrowDwellSeconds` and only past `UpHysteresis`; (d) a single-frame spike inside an otherwise steady trace never moves the tier; (e) the result is clamped to the `Tiers` bounds. |

## Verification

- Clean build; `ctest` green. The new unit case passes (label `unit`, no device).
- `include_hygiene` unaffected — `DynamicResolution.h` stays header-only over `Veng.h` (`<array>` if the
  tier list uses `std::array`), no backend include.
- No GPU test, no golden movement — nothing renders through the controller yet.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
