# Plan 02 — docs + roadmap re-cut

**Goal:** record that the `SceneRenderer`'s frames-in-flight correctness is delivered —
cross-graph reuse barrier, single-copy output, zero added memory — and supersede planset-12
decision 6. Roadmap- and docs-only; no code.

## `future/scene-renderer.md`

Replace the **Frames-in-flight contract** section. Its current text contradicts the shipped
contract — it states "the cross-frame ring-buffer hazard does not arise" and lists
"Still future — frames-in-flight > 1 (area 2)" with the ring buffer as the expected fix.
Remove the "does not arise" claim and the "Still future" sub-item; rewrite the section as
delivered. State the shipped shape as present-tense fact: the handed-out output stays
single-copy; its cross-frame reuse is serialized by a renderer-owned
`PrepareForAccess(ColorAttachment)` barrier (the reverse of the consumer's `Sample`
transition), which suffices without a semaphore or ring because the consumer read and the
next producer write both record on the single graphics queue in submission order, so the
barrier's first synchronization scope reaches the prior frame's read; the internal
g-buffer/depth/HDR targets need no barrier of their own because each lives in the
renderer's own graph, which serializes its reuse.

Record the ring/semaphore as a bounded future escalation with its two triggers: *(a)* ring
the handed-out output (or any target read across a frame boundary) when a consumer samples
an older frame while the renderer races ahead — the temporal/history-buffer case; and *(b)*
add an explicit cross-queue semaphore (or the ring) if either side of the handoff moves off
the single graphics queue (async compute, a dedicated present/UI queue), since the barrier's
submission-order reach holds only on one queue. Named precisely so a later temporal effect
or a second queue adds the right mechanism in the right place.

## `future/README.md`

Area 8's scene-renderer entry: note the frames-in-flight follow-on is done (cross-graph
reuse barrier; single-copy output and internals; no ring). Keep the remaining batteries
(shadows/SSAO/bloom/transparent/post), multiple & typed lights, G2 PBR g-buffer target,
history-buffer ringing for temporal effects, cross-queue synchronization, and parallel pass
recording as named future increments.

## `CLAUDE.md`

In the `SceneRenderer` section: add the frames-in-flight contract — the output is
single-copy; a consumer transitions it for its read (`PrepareForAccess(Sample)`) and the
next frame's scene render transitions it back (`PrepareForAccess(ColorAttachment)`,
renderer-owned), bracketing a cross-graph handoff no single graph can derive a barrier for.
The barrier suffices without a semaphore or ring because both halves record on the single
graphics queue in submission order, so the barrier's first synchronization scope reaches the
prior frame's read; the internal targets are single-copy and serialized by the renderer's
own graph. Note that ringing the output — or a cross-queue semaphore — is reserved for a
future async/temporal consumer or a second queue.

## `plans/README.md`

Add the planset-13 line (after planset-12): correct the false single-in-flight comment,
close the cross-graph output hazard with a `PrepareForAccess` barrier (single-copy, zero
memory; per-FIF ring rejected and documented as a future escalation); supersedes planset-12
decision 6.

Fix the stale attribution in planset-12's index entry and the future-area list — both
currently name "frames-in-flight > 1 with ring-buffered output" as the held-back fix. Reword
to "frames-in-flight > 1 (delivered in planset-13 by a cross-graph reuse barrier;
ring-buffered output reserved for a future temporal/async consumer)."

## Acceptance

Docs reflect the delivered FIF contract (barrier, single-copy, ring as future escalation);
`future/` marks the follow-on done with remaining increments named; `CLAUDE.md` carries the
FIF contract and the `PrepareForAccess` reuse idiom; `plans/README.md` indexes planset-13
and notes the planset-12 decision-6 supersession; no code change; build + `ctest`
unaffected. Commit: `planset-13: docs + roadmap re-cut — frames-in-flight correctness
delivered (cross-graph reuse barrier, no ring), planset-12 decision 6 superseded`.
