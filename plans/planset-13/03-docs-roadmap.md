# Plan 03 — docs + roadmap re-cut

**Goal:** record that the `SceneRenderer`'s frames-in-flight correctness is delivered — by a
cross-graph reuse **barrier**, single-copy output, zero added memory — and that planset-12
decision 6's single-in-flight guard is superseded. Roadmap- and docs-only; no code.

## `future/scene-renderer.md`

- The **Frames-in-flight contract** section: mark it **delivered**. State the shipped shape
  as present-tense fact — the handed-out output stays **single-copy**; its cross-frame reuse
  is serialized by an explicit `PrepareForAccess(ColorAttachment)` barrier (the symmetric
  partner to the consumer's `PrepareForAccess(Sample)`), because no single graph spans the
  producer's write and the consumer's read; the internal g-buffer/depth/HDR targets need no
  barrier of their own because each lives in the renderer's own graph, which serializes its
  reuse.
- Record the **ring as a bounded future escalation**, not a gap: ring the handed-out output
  (or any target read across a frame boundary) **only** when a consumer samples an *older*
  frame while the renderer races ahead — the temporal/history-buffer case — which rings the
  history target, not this output. Name it precisely so a later temporal effect adds it in
  the right place without pre-paying its memory now.

## `future/README.md`

- Area 8's scene-renderer entry: note the **frames-in-flight** follow-on is **done**
  (cross-graph reuse barrier; single-copy output and internals; no ring). Keep the remaining
  batteries (shadows/SSAO/bloom/transparent/post), multiple & typed lights, the G2 PBR
  g-buffer target, **history-buffer ringing for temporal effects**, and parallel pass
  recording named as future increments.

## `CLAUDE.md`

- In the `SceneRenderer` section (added by planset-12 plan 05): add the **frames-in-flight
  contract** — the output is single-copy; a consumer transitions it for its read
  (`PrepareForAccess(Sample)`) and the next frame's scene render transitions it back
  (`PrepareForAccess(ColorAttachment)`), the two halves bracketing a cross-graph handoff no
  single graph can derive a barrier for; the internal targets are single-copy and serialized
  by the renderer's own graph. State the reason as the factual *why* (cross-graph vs.
  intra-graph barrier derivation). Note that ringing the output is reserved for a future
  async/temporal consumer (history buffers), not done.

## `plans/README.md`

- Add the **planset-13** line to the index (after planset-12's, added by planset-12 plan
  05): a coherent phase — prove the `SceneRenderer` correct at the engine's two frames in
  flight by *measuring* the handed-out output's cross-graph hazard under synchronization
  validation, removing the false single-in-flight guard, and serializing the output's reuse
  with a `PrepareForAccess` barrier (single-copy, zero memory; the per-FIF ring rejected and
  documented as a future temporal-consumer escalation); **supersedes planset-12 decision 6's
  guard**; realizes area 8's frames-in-flight follow-on.

## Acceptance

Docs reflect the delivered FIF contract (barrier, single-copy, ring-as-future-escalation);
`future/` marks the follow-on done with the remaining increments named; `CLAUDE.md` carries
the FIF contract + the `PrepareForAccess` reuse idiom; `plans/README.md` indexes planset-13
and notes the planset-12 decision-6 supersession; no code change; build + `ctest`
unaffected. Commit: `planset-13: docs + roadmap re-cut — frames-in-flight correctness
delivered (cross-graph reuse barrier, no ring), planset-12 decision 6 superseded`.
