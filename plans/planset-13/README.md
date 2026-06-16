# planset-13 — frames-in-flight correctness for the `SceneRenderer`

**Phase goal:** make the `SceneRenderer` correct at the engine's actual frames-in-flight
count (`MaxFramesInFlight = 2`) with the minimum that buys correctness: correct a false
contract comment and add one `PrepareForAccess` barrier. This realizes the
**frames-in-flight > 1** increment [planset-12](../planset-12/README.md) held back
(decision 6) and is [future area 8](../future/scene-renderer.md#frames-in-flight-contract)'s
named follow-on.

Its prerequisite is **planset-12**: the `SceneRenderer` shell, the `ScenePass`/`PassIO`
framework, the deferred g-buffer → directional-light → tonemap chain, and the
renderer-owned-and-imported pipeline images.

## The defect

planset-12 decision 6 landed a `Single-in-flight contract` comment block in
`engine/include/Veng/Renderer/SceneRenderer.h` that ends:

> … so the frames-in-flight > 1 hazard (a compositor caching/sampling an output across
> frames-in-flight) **does not arise**.

That sentence is false. The engine already runs two frames in flight (`MaxFramesInFlight = 2`,
`engine/include/Veng/Renderer/Backend/ContextNative.h`). A separate compiled graph reads the
handed-out output across frames, and no single graph derives the barrier serializing frame
N+1's write against frame N's read. The comment is rewritten and the barrier is added.

## Why a barrier, not a ring

The instinct to ring the output (one copy per frame-in-flight) is the textbook FIF move
for CPU-written per-frame data and for temporal targets. The handed-out output is neither:

- **The memory cost is real.** An output is a full-resolution texture; ringing it costs
  `MaxFramesInFlight × output × every renderer` — meaningful for an editor with several
  viewports.
- **The overlap it would "preserve" is already gone.** With single-copy internal targets
  (g-buffer, HDR), the graph's own reuse barriers already serialize consecutive frames' early
  passes. FIF=2's real payoff is CPU/GPU overlap (record/submit/logic for N+1 while the GPU
  runs N), which needs no ringed render targets.
- **The cheap fix is the engine's own idiom.** The consumer leaves the output in
  `ShaderReadOnly`. The reverse `PrepareForAccess(output, ColorAttachment)` before each
  scene render emits the cross-graph reuse barrier via the same `ScopeFor + DecideBarrier`
  path — **one barrier, zero bytes**. It serializes frame N+1's write behind frame N's read
  because both record on the single graphics queue in submission order: a pipeline barrier's
  first synchronization scope covers every command earlier in submission order on the same
  queue, not only the current command buffer.

**When a ring (or semaphore) is warranted — documented, not built:** *(a)* a genuinely
async or multi-rate consumer that samples an older frame while the renderer races ahead
(temporal reprojection / TAA history) — the history-buffer ringing case, named future, rings
a different resource; *(b)* either side of the handoff moving to a second queue (async
compute, a dedicated present/UI queue), since submission-order reach holds only on one queue.
Both are recorded as future escalations.

## Decisions

1. **Barrier, not ring.** `PrepareForAccess(output, ColorAttachment)` inside
   `SceneRenderer::Execute` — the reverse of the consumer's `Sample` transition — closes the
   cross-graph write-after-read with zero extra memory. Single-copy output retained.

2. **Renderer-owned, not consumer-owned.** The consumer owns its read-side `Sample`
   transition; the renderer owns the write-side `ColorAttachment` transition. Emitting it in
   `Execute` means no consumer can forget it — a consumer-side barrier repeated per renderer
   per frame has one-omission failure mode with no compile-time signal.

3. **Correct the comment, not just close the hazard.** The false "does not arise" claim is
   rewritten to the true present-tense contract: why the output needs an explicit barrier (no
   single graph spans both sides, but both record on the single graphics queue) and why the
   internal targets do not (each lives in one graph that serializes its own reuse). No
   historical narrative, no plan citations (CLAUDE.md).

4. **Ring documented as bounded future escalation.** Two triggers named precisely in
   `future/scene-renderer.md` and `CLAUDE.md` — temporal/history-buffer consumer, or either
   side moving to a second queue — so a later effect or queue adds the right mechanism
   without pre-paying anything now.

## Scope

| In scope | Out of scope |
|---|---|
| Correct the false single-in-flight comment in `SceneRenderer.h` | Raising `MaxFramesInFlight` past 2 |
| `PrepareForAccess(output, ColorAttachment)` at the top of `SceneRenderer::Execute` | Per-FIF output ring (rejected; documented future escalation) |
| Docs: `future/scene-renderer.md` FIF section → delivered; `future/README.md` area 8; `CLAUDE.md` FIF contract; `plans/README.md` planset-13 line | Ring-buffered history targets; parallel pass recording |

## Plans

| # | Plan | Summary | Status |
|---|---|---|---|
| 01 | [The reuse barrier + comment fix](01-overlap-oracle.md) | Correct the false `SceneRenderer.h` comment; add `PrepareForAccess(m_OutputView, ColorAttachment)` at the top of `Execute`. One call, zero new memory, smoke unchanged. | done |
| 02 | [Docs + roadmap re-cut](02-reuse-barrier.md) | `future/scene-renderer.md` FIF section → delivered; `future/README.md` area 8 done; `CLAUDE.md` FIF contract; `plans/README.md` planset-13 line + planset-12 stale attribution fix. No code. | done |

## Process & conventions

Same cadence as every planset: implement → migrate `examples/hello-triangle` in the same
pass → verify (clean build, `ctest` green, smoke PPM correct size + exit 0) → update this
table → one commit per plan.

- **The look does not change — `smoke_golden` stays unchanged.** The fix is pure
  synchronization; any single frame renders identically.
- **Public headers stay backend-free.** `AccessKind::ColorAttachment` and `PrepareForAccess`
  are existing public vk-free API — no new include; `include_hygiene` stays green.
- **Contract comments are present-tense facts.** Why the output's cross-frame reuse needs
  the barrier and the internal targets do not is stated next to the handoff as a fact about
  the code now — no plan citations, no "future work" phrasing (CLAUDE.md).

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.

## On completion

The `SceneRenderer` is correct under the engine's two frames in flight: the false
single-in-flight comment is corrected, and the handed-out output's cross-graph reuse is
serialized by the minimal `PrepareForAccess` barrier — single-copy, zero added memory. The
per-FIF output ring is documented as a bounded future escalation for an async/temporal
consumer. **planset-12 decision 6's single-in-flight contract is superseded.** The remaining
area-8 follow-ons — the rest of the über-pipeline's batteries, multiple & typed lights, G2
PBR g-buffer target, ring-buffered history targets for temporal effects, parallel pass
recording — stay named future increments. The **editor** (area 6) and **events/input**
(area 4) remain the open plansets.
