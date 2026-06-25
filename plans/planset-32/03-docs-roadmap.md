# Plan 03 — docs + roadmap

**Goal:** document the two-loop adaptive-resolution model where it is read, update the roadmap, and run
the full verification band. Last in the planset.

## What lands

- **`engine/CLAUDE.md`.** Extend the dynamic-resolution description (the `SceneRenderer` sub-rect note
  and the `Viewport` section) to state the **two loops**: the per-frame sub-rect (`RenderScale`, free,
  GPU-frame-time driven) as the fast inner loop, and the **allocation tier** (quantized, hysteretic,
  dwell-gated, EMA-driven) as the slow outer loop that sizes the targets via `SceneRenderer::Resize`.
  State the anti-thrash invariant as a present-tense fact ("the allocation never reacts to instantaneous
  frame time; it follows a multi-second EMA of the sub-rect scale across quantized tiers through a
  hysteresis band, so it cannot oscillate") and the visual-continuity fact ("a tier change keeps the
  rendered pixel count constant, so reallocation does not pop"). Note the HiDPI cap
  (`MaxAllocationScale`) as the baseline budget decoupling the allocation from the swapchain backing
  extent.

- **`future/README.md` (area 8).** Record the delivered allocation self-sizing, and name the follow-ons:
  safe-moment reallocation (defer the tier-change `Resize` to a transition / static camera), memory-driven
  initial tier (from a device memory-budget query), and the TAA history ping-pong (removing the
  history-copy from the full-allocation tail).

- **`plans/README.md`.** The planset-32 entry (this record).

## Files

| File | Change |
|---|---|
| `engine/CLAUDE.md` | The two-loop adaptive-resolution model on the `SceneRenderer`/`Viewport` sections. |
| `plans/future/README.md` | Area 8: delivered allocation self-sizing + named follow-ons. |
| `plans/README.md` | The planset-32 summary entry. |
| `plans/planset-32/README.md` | Mark the status column `done` as plans land. |

## Verification

- Full `ctest --output-on-failure` green (`unit` incl. the Plan 00 controller case; `gpu` incl. the
  Plan 02 allocation-shrink test).
- `smoke_golden` green and **unmoved** — the controller is inert in the fixed-pose headless capture.
- `validation_gate` green under `build-debug` (`ctest -L validation`).
- The HiDPI machine: enabling TAA now steps the allocation down under sustained load and frame time
  recovers — the originating problem, resolved.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
