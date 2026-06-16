# Plan 02 — the minimal cross-graph reuse barrier

**Goal:** act on plan 01's verdict with the **minimum** that buys correctness. The
handed-out output is the one renderer-owned image read by a *separate* graph (the
composite), so no single graph derives the barrier serializing frame N+1's write against
frame N's read. The fix is the engine's existing escape hatch —
`PrepareForAccess(output, AccessKind::ColorAttachment)` before each frame's scene render —
the symmetric partner to the composite's existing `PrepareForAccess(output, Sample)`,
bracketing the cross-graph handoff in both directions. **Single-copy output retained, zero
added memory, no ring.** Then extend planset-12's two-renderer test to multiple frames so
the barrier holds under the N-renderer shape.

## Why this is the fix, and on the main thread

Plan 01 measured; this plan fixes, and the *choice of fix* is the design judgement to keep
on the main thread: a barrier (zero memory, the engine's idiom) over a per-FIF ring of a
full-resolution texture (real recurring memory, no concurrency benefit — the internal
single-copy targets already serialize consecutive frames, and FIF=2's payoff is CPU/GPU
overlap, not ringed render targets). The two-renderer multi-frame extension is good
`model: sonnet` delegation once the barrier placement is fixed.

## The barrier

The composite already transitions the output for its read:
`PrepareForAccess(GetOutput(), AccessKind::Sample)` before sampling. The missing partner is
the **reverse** transition before the output is written again next frame:

```cpp
// Before SceneRenderer::Execute each frame (the scene render writes the output as a
// color attachment; the previous frame's composite left it in ShaderReadOnly).
cmd.PrepareForAccess(m_SceneRenderer->GetOutput(), Renderer::AccessKind::ColorAttachment);
m_SceneRenderer->Execute(cmd, view);
```

`PrepareForAccess` "funnels into the same barrier path as the graph (ScopeFor +
DecideBarrier) and updates the image's tracked state" (`CommandBuffer.h`), so this emits a
`ShaderReadOnly → ColorAttachment` transition whose source scope covers the prior frame's
composite read — serializing frame N+1's output write behind frame N's composite read, and
leaving the image's tracked state correct so the scene graph's own acquire sees no further
hazard.

- **Placement:** before `SceneRenderer::Execute` (the app drives both graphs, so the app is
  the one place that sees both the consumer read and the next producer write). The
  over-serialization is negligible — the g-buffer reuse barrier already gates the next
  frame's geometry a few passes earlier, so this adds a sliver of tail dependency, not a
  stall.
- **Encapsulation note:** the app-side call mirrors the existing composite-side
  `PrepareForAccess(Sample)`. If preferred, the symmetric transition can instead be owned by
  the renderer (the output is its resource and its handed-out contract), keeping both halves
  of the cross-graph handoff named in one place; pick the placement that reads cleanest
  against the sample's existing composite barrier and state it as a present-tense fact.

### If plan 01 found the output already clean

If plan 01's oracle showed no cross-frame hazard (the compiled graph already serializes the
output via its tracked state), still land the explicit `PrepareForAccess(ColorAttachment)`
as the **documented contract** — making the cross-graph reuse dependency visible and
intentional rather than incidental — and keep the overlap oracle as the standing regression
gate. The single-copy output is retained either way; **no ring** is built.

## The contract, stated as fact

Next to the output handoff (and reflected in `CLAUDE.md` by plan 03), a present-tense
comment: the output's cross-frame reuse needs an explicit `PrepareForAccess` barrier because
no single graph spans the composite's read and the next scene render's write; the internal
g-buffer/depth/HDR targets need none because each lives in the renderer's own graph, which
transitions it from its prior-frame layout on the next `Execute` and so serializes its own
reuse. No "future work" phrasing, no plan citation (CLAUDE.md).

## The two-renderer multi-frame extension

planset-12's design-for-N test `Execute`s two `SceneRenderer`s interleaved in one command
stream. Extend it across **K > `MaxFramesInFlight`** frames and assert each renderer's
per-frame captures stay independent **and** uncorrupted across overlap — so the reuse
barrier holds under the N-renderer shape the editor will use, not just a single renderer.

## Tests

- **GPU (`veng_gpu`):** the plan 01 overlap oracle now passes **with the barrier applied**
  (each frame's readback matches its signature, no `SYNC-HAZARD` on the output across
  frames); the two-renderer multi-frame independence test. Verify the oracle **fails** if
  the barrier is reverted (a guard against regression — confirm by a local revert, not
  committed).
- **Synchronization-validation gate:** green — no cross-frame hazard on the output or the
  internal targets; allowlist empty.
- **Validation gate:** standard error/warning gate green, allowlist empty.
- **`smoke_golden`:** unchanged (the fix is a barrier, not pixels).
- **`include_hygiene`:** unaffected — `PrepareForAccess`/`AccessKind` are existing public
  vk-free API.

## Acceptance

Clean build; the output's cross-frame reuse is serialized by `PrepareForAccess(output,
ColorAttachment)` (single-copy output retained, **no ring, zero added memory**); the plan 01
overlap oracle passes and reports no sync hazard on the output across frames; the
two-renderer multi-frame test passes; the cross-graph reuse contract is stated as fact;
`ctest` green; `smoke_golden` unchanged; smoke PPM correct size + exit 0; both validation
gates green, allowlist empty. Commit: `Plan 02: serialize SceneRenderer output cross-frame
reuse with a PrepareForAccess barrier (no ring), two-renderer multi-frame test`.
