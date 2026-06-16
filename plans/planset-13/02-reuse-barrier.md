# Plan 02 — the minimal cross-graph reuse barrier

**Goal:** act on plan 01's verdict with the **minimum** that buys correctness. The
handed-out output is the one renderer-owned image read by a *separate* graph (the
composite), so no single graph derives the barrier serializing frame N+1's write against
frame N's read. The fix is the engine's existing escape hatch —
`PrepareForAccess(output, AccessKind::ColorAttachment)` before each frame's scene render —
the reverse of the consumer's read transition, bracketing the cross-graph handoff in both
directions. It works without a semaphore or a ring **because the consumer read and the next
producer write both record on veng's single graphics queue in submission order**, so the
barrier's source scope reaches the prior frame's read. **Single-copy output retained, zero
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

The consumer transitions the output for its read: the composite declares `.Sample(output)`
in its own graph, and the windowed ImGui path calls
`PrepareForAccess(GetOutput(), AccessKind::Sample)` directly (the context's bindless-acquire
also transitions it to `Sample` at frame start). Each leaves the output in `ShaderReadOnly`.
The missing partner is the **reverse** transition before the output is written again next
frame. It is **owned by the renderer** — `SceneRenderer::Execute` emits it before replaying
its internal graph, since the output is the renderer's resource and its handed-out contract:

```cpp
// SceneRenderer::Execute, before replaying the internal graph. The scene render writes the
// output as a color attachment; the previous frame's consumer left it in ShaderReadOnly.
cmd.PrepareForAccess(m_Output, Renderer::AccessKind::ColorAttachment);
m_Graph->Execute(cmd, imports, &view);
```

`PrepareForAccess` "funnels into the same barrier path as the graph (ScopeFor +
DecideBarrier) and updates the image's tracked state" (`CommandBuffer.h`), so this emits a
`ShaderReadOnly → ColorAttachment` transition. Its source scope covers the prior frame's read
**because that read and this write are both on the single graphics queue in submission
order** — a pipeline barrier's first synchronization scope reaches every command earlier in
submission order on the same queue, not just the current command buffer. The transition
serializes frame N+1's write behind frame N's read and leaves the tracked state correct, so
the scene graph's own acquire sees no further hazard.

- **Placement: renderer-owned, not consumer-owned.** `SceneRenderer::Execute` emits the
  `ColorAttachment` transition itself, so no consumer can forget it. This matters for the
  editor — the planset's motivating N-renderer consumer: a consumer-side barrier would have to
  be repeated correctly per renderer per frame, and a single omission is a silent cross-frame
  hazard with no compile-time signal. The consumer still owns the *read*-side `Sample`
  transition (it owns the read); the renderer owns the *write*-side transition (it owns the
  resource). State that asymmetry as a present-tense fact.
- **Cost:** the over-serialization is negligible — the g-buffer reuse barrier already gates
  the next frame's geometry a few passes earlier, so this adds a sliver of tail dependency,
  not a stall.

### If plan 01 found the output already clean

If plan 01's oracle showed no cross-frame hazard (the compiled graph already serializes the
output via its tracked state), still land the explicit `PrepareForAccess(ColorAttachment)`
as the **documented contract** — making the cross-graph reuse dependency visible and
intentional rather than incidental — and keep the overlap oracle as the standing regression
gate. The single-copy output is retained either way; **no ring** is built.

## The contract, stated as fact

Next to the output handoff (and reflected in `CLAUDE.md` by plan 03), a present-tense
comment: the output's cross-frame reuse needs an explicit `PrepareForAccess` barrier because
no single graph spans the consumer's read and the next scene render's write — but both record
on the single graphics queue in submission order, so the barrier's source scope reaches the
prior frame's read (no semaphore or ring). The internal g-buffer/depth/HDR targets need none
because each lives in the renderer's own graph, which transitions it from its prior-frame
layout on the next `Execute` and so serializes its own reuse. No "future work" phrasing, no
plan citation (CLAUDE.md).

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
