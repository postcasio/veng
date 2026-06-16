# Plan 05 — docs + roadmap re-cut

**Goal:** record planset-12 in the roadmap and the project docs. No code; the
`planset-12:` commit (not `Plan NN:`) closes the phase.

## `plans/README.md`

Add the **planset-12** entry to the planset list: the `SceneRenderer` — a long-lived,
configurable deferred über-pipeline on `RenderGraph` (owns an offscreen target,
renders a `Scene`/`Camera` through reusable `ScenePass` units, hands back a sampleable
result), delivering the **g-buffer → directional-light → tonemap** v1 spine + a
directional `Light` builtin, with hello-triangle's main view migrated onto it
(designed for N renderers, one wired). Note it was taken up **before** the editor
(area 6) by choice — the editor inherits the multi-viewport consumer solved.

When this lands, also update the `planset-11` line if it still reads as the latest
phase, and confirm the prerequisite cross-links (planset-8 graph, planset-10
scene/camera, planset-11 `generate-type-id`) resolve.

## `future/README.md`

- **Area 8 → DONE (planset-12).** Rewrite the area-8 entry to a DONE recap (shell +
  lifetime split + `ScenePass` framework + the minimal deferred spine + the `Light`
  builtin), pointing at planset-12. Keep the **still-future** remainder explicit: the
  rest of the über-pipeline batteries (shadows, SSAO, bloom, MSAA, transparent/forward
  pass, post stack), **multiple & typed lights** + light culling, **frames-in-flight >
  1** with ring-buffered output, and **parallel pass recording** (area 2's seam).
- **Ordering & dependencies:** update the sequencing block — area 8 is done; the
  remaining open areas are **6 (editor)** and **4 (events/input)**. Note the editor's
  scene viewport is now a *consumer* of a delivered `SceneRenderer`, not a blocker for
  it.
- **Status section:** add planset-12 to the DONE list.

## `future/scene-renderer.md`

Re-cut from "vision only" to **delivered vs. still-future**:

- Mark the **delivered** core: the lifetime split (`Create`/`Resize`/`Configure`/
  `Execute`/`GetOutput`), owned target, `SceneView`, the `ScenePass` reusable-unit
  abstraction + `ScenePassContext`, the self-contained-graph-per-renderer resolution,
  the minimal deferred chain, and the directional `Light`.
- Reconcile the design sketch with what shipped: `IRenderPass` → **`ScenePass`** (the
  `I`-prefix naming-rule fix), the `SceneView`-on-`PassContext` design sketch → the
  **opaque user-pointer** channel (RenderGraph stays scene-agnostic), the g-buffer/HDR
  as **renderer-owned imported images** (registered once into bindless) rather than
  graph transients, and the single-in-flight v1 with the recorded-and-asserted FIF>1
  allocation note.
- Keep the **still-future** sections (batteries, multi-light, FIF>1, parallel record)
  as the named next increments behind the same mechanism.

## `CLAUDE.md`

Add a **`SceneRenderer`** subsection (under the rendering/scene material) stating, as
present-tense fact:

- What `SceneRenderer` is and its lifetime split; that it owns its offscreen target
  and is `Unique`; that `Configure`/`Resize` recompile and `Execute` replays.
- The **deferred opaque material g-buffer contract** — an opaque material's fragment
  shader writes the g-buffer (albedo + world-normal via `GBufferOutput`), the deferred
  lighting pass consumes it — stated where a material author will look, with the v1
  channels marked as the **minimum** set (a future G2 PBR target extends it) and noted
  as the *opaque* contract (a transparent/forward material outputs final color, a
  separate entry).
- The directional `Light` builtin in the scene-model component list.
- That the über-pipeline is **batteries-included, not extensible** — bespoke graphs
  drop to `RenderGraph` directly (the composite path).

Follow the comment/doc rules: no plan citations, present-tense facts, no future-work
phrasing.

## Acceptance

Docs build/read clean; every cross-reference resolves; the planset-12 status table
shows all plans `done`. Commit: `planset-12: docs + roadmap re-cut — area 8
(SceneRenderer) delivered`.
