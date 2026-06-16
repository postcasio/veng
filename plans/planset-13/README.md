# planset-13 — frames-in-flight correctness for the `SceneRenderer` (measure first, barrier not ring)

**Phase goal:** make the `SceneRenderer` demonstrably correct at the engine's actual
frames-in-flight count (`MaxFramesInFlight = 2`) — the regime it already runs in — with
the **minimum** that buys correctness. Two things stand in the way: planset-12 ships a
`Create`-time `VE_ASSERT(GetMaxFramesInFlight() == 1, …)` that is **wrong** (it aborts at
the engine's real FIF), and the handed-out output is potentially a **cross-graph
write-after-read** hazard across frames. This planset **measures the hazard first**
(nothing exercises frame overlap today), removes the false guard, and — if the hazard is
real — fixes it with the engine's existing cross-graph barrier idiom (`PrepareForAccess`),
**zero extra memory**. A per-frame-in-flight **ring** of the (large) output texture is
explicitly **rejected as the v1 fix** and documented as a future escalation reserved for a
consumer that does not yet exist. This realizes the **frames-in-flight > 1** increment
[planset-12](../planset-12/README.md) recorded-and-guarded (its decision 6), and is
**future area 8**'s named follow-on
([future/scene-renderer.md](../future/scene-renderer.md#frames-in-flight-contract)).

Its prerequisite is **planset-12**: the `SceneRenderer` shell, the `ScenePass`/`PassIO`
framework, the deferred **g-buffer → directional-light → tonemap** chain, and the
renderer-owned-and-imported pipeline images. planset-13 assumes that chain has landed and
changes only the **synchronization** of the handed-out output, plus the contract and
guard around it.

## The defect this corrects (planset-12 decision 6)

planset-12 decision 6 guards frames-in-flight > 1 with a `Create`-time
`VE_ASSERT(context.GetMaxFramesInFlight() == 1, …)`, on the premise that "nothing drives
FIF > 1 while the render thread is single." **That premise is false:** the engine already
runs **two frames in flight** (`MaxFramesInFlight = 2`,
`engine/include/Veng/Renderer/Backend/ContextNative.h`), load-bearing for the retire bins
and the bindless reclaim window. So the `== 1` assert does not "fire when someone bumps
FIF later" — it **fires on the first `Create`**, aborting hello-triangle at launch. It is
not a future tripwire; it is presently incorrect, and planset-13 **removes it** (it is the
first thing to go, since the multi-frame overlap test cannot even run while `Create`
asserts `== 1`).

## Why "measure first" — the hazard is unproven

The output is the only renderer-owned image read by a *separate* compiled graph (the
composite) — so the producing `SceneRenderer` graph cannot derive the barrier serializing
frame N+1's write against frame N's composite read; that bridging barrier is nobody's job.
That is a **potential** write-after-read. But whether it is a **live** hazard today is
unknown, because **nothing exercises frame overlap**: the headless smoke renders **one**
frame and exits, so the validation gate's only GPU driver never has two frames in flight.
A green gate proves single-frame cleanliness and says nothing cross-frame. The engine also
already tracks each image's layout across graph boundaries (`PrepareForAccess` "updates the
image's tracked state, so a later graph pass declaring the same use correctly sees no
hazard", `CommandBuffer.h`), so the single-copy output may already be serialized — or may
race. **planset-13 settles it with a test before building any fix.**

## Why a barrier, not a ring

The instinct to ring the output (one copy per frame-in-flight) is the textbook FIF move
for **CPU-written** per-frame data and for **temporal** targets (this frame reads last
frame's version). The handed-out output is **neither**:

- **The memory cost is real and recurring.** An output is a full-resolution texture;
  ringing it costs `MaxFramesInFlight × output × every renderer`. For an editor with
  several viewports that is meaningful memory spent permanently.
- **The overlap it would "preserve" is already gone.** With single-copy internal targets
  (g-buffer, HDR), the graph's own reuse barriers already serialize consecutive frames'
  early passes; FIF=2's real payoff is **CPU/GPU** overlap (record/submit/logic for frame
  N+1 while the GPU runs frame N), which needs no ringed render targets. Ringing the output
  buys no meaningful concurrency.
- **The cheap fix is the engine's own idiom.** The composite already calls
  `PrepareForAccess(output, Sample)` before it reads; the **symmetric**
  `PrepareForAccess(output, ColorAttachment)` before each frame's scene render emits the
  cross-graph reuse barrier — ordered after the prior frame's composite read, via the same
  `ScopeFor + DecideBarrier` path — for **one barrier and zero bytes**. The over-
  serialization is negligible: the g-buffer reuse barrier already gates the next frame's
  geometry a few passes earlier, so this adds a sliver of tail dependency, not a stall.

**When the ring *is* warranted (documented, not built):** a genuinely **async or
multi-rate** consumer — one that samples an *older* frame's output while the renderer races
ahead to a newer frame (temporal reprojection / TAA history, a renderer running at a
different cadence than its consumer). veng has no such consumer: the editor's ImGui samples
*this* frame's viewport within *this* frame's ordered UI pass. That case is the same class
as **history-buffer ringing**, already named future, and it rings a *different* resource
(the history target), not this output. So the ring is recorded as the escalation for that
future, and v1 ships the barrier.

## What this delivers, and what it holds back

A `SceneRenderer` that is **proven** correct under frame overlap at FIF=2: the false
`== 1` guard removed; a **multi-frame overlap GPU test under synchronization validation**
as the standing oracle; and — if that oracle shows the output races — the minimal
`PrepareForAccess(output, ColorAttachment)` reuse barrier, single-copy output retained,
**zero added memory**. If the oracle shows the compiled graph already serializes the output
via its tracked state, the "fix" is the explicit barrier made a documented contract plus
the test kept as a regression gate — still no ring.

What it **holds back** (named, not silently dropped):

- **The per-FIF output ring** — rejected as the v1 fix (memory for no benefit); documented
  as the future escalation for an async/multi-rate/temporal consumer, which does not exist.
- **Ring-buffered history targets** for temporal effects (TAA/motion-vector/denoise reads
  of a *prior* frame's internal target) — the genuine future ringing case, behind the same
  reasoning; no such pass exists.
- **Raising `MaxFramesInFlight` past 2 or making it configurable** — out of scope; the
  barrier fix is correct for whatever the context reports.
- **Parallel pass recording** (area 2's seam) — unchanged; planset-12 decision 3's
  record-time channel already made the choice *for* it, but no parallel recording is built.

## Decisions

1. **Measure before fixing.** The cross-graph output hazard is potential, not proven —
   nothing drives frame overlap today (smoke = one frame). A multi-frame overlap GPU test
   under synchronization validation is built **first**, as the oracle that decides whether a
   fix is needed and, after, stands as the regression gate. No fix is committed on the
   strength of reasoning alone.

2. **A cross-graph reuse barrier, not a per-FIF ring.** If the output races, the fix is the
   engine's existing escape hatch — `PrepareForAccess(output, AccessKind::ColorAttachment)`
   before each frame's scene render — symmetric with the composite's existing
   `PrepareForAccess(output, Sample)`, bracketing the cross-graph handoff in both
   directions. Zero extra memory; negligible over-serialization (the internal reuse barriers
   already serialize there). The single-copy output is retained.
   - **Rejected — ring the output per frame-in-flight:** `MaxFramesInFlight ×` a
     full-resolution texture per renderer, for no meaningful concurrency (the internal
     single-copy targets already serialize consecutive frames; FIF's real win is CPU/GPU
     overlap). Reserved as a future escalation for an async/multi-rate consumer that veng
     does not have.
   - **Rejected — add nothing, trust incidental MoltenVK serialization:** ships a sync-
     validation hazard and undefined behaviour; veng's correctness posture is a clean gate.

3. **Remove the `== 1` guard, do not relax it.** It encodes a false premise (FIF is already
   2) and aborts at launch. It is deleted; in its place a present-tense fact next to the
   output handoff states why the cross-graph reuse needs an explicit barrier (no single
   graph spans the producer's write and the consumer's read) and why the internal targets
   need none (each lives in one graph that serializes its own reuse). No "future work"
   phrasing (CLAUDE.md).

4. **The ring is documented as a bounded future escalation.** Recorded in
   `future/scene-renderer.md` and `CLAUDE.md` as: *ring the handed-out output (or any
   target read across a frame boundary) only when a consumer samples an older frame while
   the renderer races ahead — the temporal/history-buffer case — which rings the history
   target, not this output.* Named precisely so a later temporal effect adds it in the right
   place, without pre-paying its memory now.

## Scope of this phase

| In scope | Out of scope (later / other phases) |
|---|---|
| Remove planset-12's `Create`-time `== 1` assert (it aborts at the engine's real FIF) | Raising `MaxFramesInFlight` past 2 / making it configurable |
| A multi-frame overlap GPU test (drive > FIF frames, read back an in-flight earlier frame, assert uncorrupted) under the **synchronization validation** gate — the oracle and standing regression test | A new smoke/headless multi-frame harness beyond the GPU suite; CI changes |
| The minimal **cross-graph reuse barrier** — `PrepareForAccess(output, ColorAttachment)` before the scene render — applied if the oracle shows the output races; single-copy output retained, zero added memory | A per-frame-in-flight **ring** of the output (rejected v1 fix; documented future escalation) |
| Extend planset-12's two-renderer test to **multiple frames** so the barrier holds under the N-renderer shape | Ring-buffered history targets for temporal effects (the genuine future ringing case) |
| Docs: `future/scene-renderer.md` FIF section → delivered (barrier fix, ring as bounded escalation); `future/README.md` area 8 FIF note; `CLAUDE.md` `SceneRenderer` FIF contract; `plans/README.md` planset-13 line | A general post-processing or history-buffer surface; parallel pass recording (area 2) |

## Plans

| # | Plan | Summary | Status |
|---|---|---|---|
| 01 | [The overlap oracle + drop the false guard](01-overlap-oracle.md) | **Remove** planset-12's `Create`-time `== 1` assert (it aborts at FIF=2, and blocks the test). Build the standing oracle: a GPU test that drives **more frames than `MaxFramesInFlight`**, reads back an earlier frame's output while a later frame is in flight, and asserts the readback is uncorrupted, run under **synchronization validation** — reporting whether the single-copy output races across frames (and confirming the internal g-buffer/depth/HDR targets are clean). **Pixels unchanged**; `smoke_golden` stays unchanged. | proposed |
| 02 | [The minimal cross-graph reuse barrier](02-reuse-barrier.md) | Outcome-driven by plan 01. If the output races: apply `PrepareForAccess(output, AccessKind::ColorAttachment)` before each frame's scene render — the symmetric reuse barrier — making the overlap oracle sync-clean with **zero added memory**, single-copy output retained; document the cross-graph reuse contract. If plan 01 shows the compiled graph already serializes via tracked state: land the explicit barrier as the documented contract anyway and keep the oracle as the regression gate. Either way: **no ring**. Extend the two-renderer test to multiple frames. | proposed |
| 03 | [Docs + roadmap re-cut](03-docs-roadmap.md) | `future/scene-renderer.md` (FIF section → **delivered**: the cross-graph reuse barrier; single-copy internals; the ring as a bounded future escalation for temporal/async consumers); `future/README.md` (area 8 FIF follow-on done; history-buffer ringing + parallel record named future); `CLAUDE.md` (the `SceneRenderer` FIF contract + the reuse-barrier idiom); `plans/README.md` (planset-13 line). Note that this **supersedes planset-12 decision 6's guard**. | proposed |

## Dependencies & dispatching

One short ordered chain on top of planset-12:

```
planset-12 (SceneRenderer + deferred chain + single-copy renderer-owned output)
   ──► 01 (drop the == 1 guard; build the overlap oracle under sync validation; measure)
       ──► 02 (apply the minimal reuse barrier per the oracle; no ring; N-renderer multi-frame)
           ──► 03 (docs + roadmap re-cut)
```

- **Recommended order:** `01 → 02 → 03`. 01 unblocks the renderer (drops the aborting
  assert) and *measures* — its result drives 02. 02 applies the minimal fix and proves it.
  03 is roadmap-only. 01 and 02 may be one session if the oracle's verdict is quick.
- **Keep on the main thread:** **01**'s oracle framing (what "uncorrupted across overlap"
  asserts, the sync-validation gate wiring) and the assert removal; **02**'s fix decision
  (barrier placement + the cross-graph contract); **03** (docs).
- **Good `model: sonnet` delegation** once the framing is fixed: the mechanical parts of
  **01**'s multi-frame drive loop + readback comparison, **02**'s two-renderer multi-frame
  extension, and **03**'s doc edits against the agreed contract.

## Process & conventions

Same cadence as every planset: implement → migrate `examples/hello-triangle` in the same
pass → verify (clean build, `ctest` green, smoke binary writes a correct-sized 1280×720 RGB
PPM ≈ 2,764,816 bytes) → update this table → one commit per plan, `Plan NN: <summary>` with
a `Co-Authored-By` trailer (`planset-13:` for the docs plan).

- **The look does not change — `smoke_golden` stays unchanged.** The fix is pure
  synchronization (a barrier), not pixels; any single frame renders identically.
  `smoke_golden` is **not** regenerated.
- **The smoke path cannot prove this planset.** It renders one frame; overlap never
  happens. The GPU overlap test (plan 01) under **synchronization validation** is the only
  oracle that exercises the hazard — a green smoke is necessary, not sufficient.
- **Zero new memory; no per-frame allocation.** The fix is a recorded barrier, not a
  resource. No ring, no extra image, no bindless change.
- **Public headers stay backend-free.** `AccessKind::ColorAttachment` and
  `PrepareForAccess` are existing public vk-free API — no new include; `include_hygiene`
  stays green.
- **Synchronization validation is already enabled** (`eSynchronizationValidation` in
  `Context.cpp`, behind `VE_ENABLE_VALIDATION_LAYERS`). Plan 01 consumes it; the work is
  asserting on its hazards in the gate under genuine frame overlap, not enabling it.
- **Contract comments are present-tense facts.** Why the output's cross-frame reuse needs
  an explicit barrier and the internal targets do not is stated next to the handoff as a
  fact about the code now — no plan citations, no "future work" phrasing (CLAUDE.md).

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.

## On completion

The `SceneRenderer` is **proven** correct under the engine's two frames in flight: the
false `== 1` guard is gone, a multi-frame overlap test under synchronization validation is
the standing oracle, and the handed-out output's cross-graph reuse is serialized by the
minimal `PrepareForAccess` barrier — **single-copy, zero added memory**. The per-FIF output
ring is recorded as a bounded future escalation for an async/multi-rate/temporal consumer
(the history-buffer case), not pre-paid now. **planset-12 decision 6's single-in-flight
guard is superseded.** The remaining area-8 follow-ons — the rest of the über-pipeline's
batteries, multiple & typed lights, a G2 PBR g-buffer target, ring-buffered **history**
targets for temporal effects, and parallel pass recording — stay named future increments
behind the same mechanisms. The **editor** (area 6) and **events/input** (area 4) remain
the open plansets.
