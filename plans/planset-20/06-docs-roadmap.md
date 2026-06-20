# Plan 06 — docs + roadmap re-cut

**Goal:** record the bounds facility and CSM in the living docs and re-cut the roadmap —
scene/mesh AABB + bounds and cascaded shadow maps delivered, the remaining shadow/culling
follow-ons re-pointed to read the facility. No code beyond Plans 01–05.

## What lands

### `CLAUDE.md` — bounds + CSM in the renderer/scene paragraphs

- The **SceneRenderer** paragraph's shadow sentence drops "a fixed-size ortho box since no
  scene-bounds facility exists yet" and states the present fact: the directional light is
  shadowed by **cascaded shadow maps** — the camera frustum split into depth slices, each
  cascade fit (bounding-sphere + texel-snapped) to its slice and rendered into a depth atlas
  bound in a **dedicated descriptor set** (off bindless), the lighting pass selecting the
  cascade and **`SampleCmp`ing** it (hardware comparison sampler) per fragment with a boundary
  cross-fade. State that **set 1 is the whole directional-shadow system** — the atlas, the
  immutable comparison sampler, and the per-frame `ShadowConstants` buffer (cascade matrices +
  splits + params) — while the set-0 view-constants block stays trimmed to material-facing
  camera/view state. Note `CascadeCount`/`CascadeSplitLambda`/`ShadowResolution` as the CSM knobs
  and `DebugView::Cascades` as the visualizer.
- The **Scene & ECS** section gains `SceneBounds` (world-space union of `(Transform,
  MeshRenderer)` mesh bounds via `ComputeWorldMatrices`, recompute-on-demand) and `Mesh`'s
  local-space `GetBounds()` (derived from vertices).
- A one-line mention of the `Veng/Math/` home for `AABB` and the device-free `ShadowCascades`
  (`ComputeCascades`) math.
- The **SceneRenderer** section's "internal targets reach passes as bindless `TextureHandle`s
  through `PassIO`" contract gains the **bound-view** exception: a closed producer→consumer
  resource (the shadow atlas) reaches its consumer through a **dedicated descriptor set** via
  the `PassIO` bound-view slot — off bindless — so the lighting pass can use a comparison
  sampler / `SampleCmp`. State why (a comparison sampler is barred from set 0's bindless
  argument buffer on MoltenVK; a closed resource needs no global registration).

### `plans/future/scene-renderer.md` + `plans/future/README.md` — re-cut

- **Reconcile the existing uncommitted edits first.** The working tree already carries
  planset-20 forward-references in both files (added ahead of implementation — e.g. the atlas
  decision in `scene-renderer.md`); fold these into the delivered framing rather than
  double-adding, so the final text reads as one coherent "delivered" cut.
- Move **scene/mesh AABB + bounds** and **CSM** from "the named next prerequisite" / "still
  future" to **delivered** (planset-20). (The intermediate single-map tight fit was skipped —
  planset-20 went straight to CSM — so it is not a separate delivered line.)
- Re-point what sat behind them: **shadowed punctual lights** (point/spot cubemap/atlas) and
  **frustum culling** (the other prime consumer of mesh bounds) are the named next increments
  reading the delivered `AABB`/`SceneBounds`; a cached/dirty-tracked scene bound or a BVH is
  the scaling step both share.
- Add the **`view_constants.slang` / `shadow.slang` split** as the present state of the
  view/shadow shader headers (the bound-view exception in the imported-images contract already
  covered above), and note the set-0 `ViewConstants` is now material-facing view state.

### `plans/README.md` — the planset-20 entry

Add the planset-20 summary paragraph (bounds facility + cascaded shadow maps), in the same
form as the other entries.

### `plans/planset-20/README.md` — finalize the status table

Flip Plans 01–06 to `done` as each lands.

## Decisions

1. **Docs-only plan, mirroring planset-19's Plan 07.** Keeps the render-behavior change and
   the roadmap bookkeeping in separate commits, matching the house pattern of a final
   docs/roadmap re-cut plan.

2. **Comment policy holds.** The `CLAUDE.md` edits and any touched code comments state
   present-tense facts. There is **no surviving fixed box** — CSM always fits the XY extent from
   the camera frustum slice; the only fixed fallback is the empty-`sceneBounds` near-plane
   pullback inside `ComputeCascades` (step 5), commented as such. No "used to be a single fixed
   box" narrative, no plan citations in code.

## Files

| File | Change |
|---|---|
| `CLAUDE.md` | SceneRenderer shadow sentence (CSM); Scene & ECS `SceneBounds` + `Mesh::GetBounds`; `Veng/Math/` + `ShadowCascades` mention. |
| `plans/future/scene-renderer.md` | AABB/bounds + CSM delivered; shadowed punctual lights / frustum culling named next behind the facility. |
| `plans/future/README.md` | Same re-cut in the area-8 status line. |
| `plans/README.md` | Planset-20 entry. |
| `plans/planset-20/README.md` | Status table → `done`. |

## Verification

- Docs only — no build/test impact. A final clean build + full `ctest` confirms Plans 01–05
  are still green at the planset's close, and the `smoke_golden` capture matches the
  Plan-03/04-regenerated golden.
