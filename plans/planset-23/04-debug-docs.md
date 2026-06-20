# Plan 04 — Debug/stats exposure + docs/roadmap re-cut

**Goal:** surface the broadphase's rebuild-on-change behavior in the example debug UI, update the
engine docs to describe culling as a BVH broadphase rebuilt when the scene's spatial version moves,
and re-cut the roadmap — marking the **BVH broadphase delivered** and naming **incremental tree
maintenance**, **GPU/occlusion culling**, **per-submesh leaves**, and **a Scene-shared tree** as the
refinements behind the same seam.

## What lands

### Example debug UI ([examples/hello-triangle](../../examples/hello-triangle))

In the renderer/stats debug panel (`Veng::UI`), beside the planset-21 visible/drawn readout, surface
the broadphase:

- **`Broadphase: rebuilt` / `Broadphase: static`** this frame, from
  `DidBroadphaseRebuildLastFrame()`, plus the tree node count and the visible-after-cull count
  already reported. A moving scene shows `rebuilt`; a still scene shows `static` — the tree was not
  touched at all.

- **A "pause spin" debug toggle makes the static state visible.** The hello-triangle scene's only
  drawable spins every frame (`OnUpdate` writes its `Transform.Rotation` — a non-`const` `Each`,
  which bumps the spatial version), so it always rebuilds. Add a windowed-only checkbox that, when
  set, skips the `OnUpdate` write: no non-`const` `Each` → the version does not move → the next frame
  reads `static` and the tree stands. Toggling it flips the readout live between `rebuilt` and
  `static` — the visible proof that a static scene touches the tree not at all. The toggle is gated
  to the windowed app; **smoke mode and the golden are untouched.**

No new setting — the broadphase has no toggle (Plan 03 decision 4); the existing `FrustumCull` switch
stays.

### `CLAUDE.md`

Update the scene/gather/cull wording where it currently describes recompute-on-demand linear culling:

- The `SceneRenderer` frustum-cull paragraph: the camera and each cascade cull through a **BVH
  broadphase** (`SceneBroadphase`) rebuilt when the scene's spatial version moves, not a per-view
  linear scan — sub-linear, byte-identical, with a static scene rebuilding the tree not at all.
  Present-tense fact — no "used to scan linearly."
- The `SceneBounds`/`Visibility.h` wording: `GatherMeshes` stays the pure one-shot candidate gather;
  the **broadphase** caches it and builds the BVH, re-gathering only when the scene's spatial version
  moves (or a mesh finishes loading). The "BVH (or a cached, dirty-tracked scene bound) is the named
  scaling step" sentence becomes "the BVH broadphase is the delivered scaling step; incremental tree
  maintenance, GPU/occlusion culling, and per-submesh leaves are its refinements."
- The Scene/ECS paragraph: note the **spatial version counter** — `Scene` bumps a monotonic
  `GetSpatialVersion()` on spatial-pool (`Transform`/`Parent`/`MeshRenderer`) structural change,
  non-`const` access, or a `ForEachComponent` visit; a `const` `View`/`Each` does not bump it. This
  is the access-as-write change-tick a consumer (the broadphase) gates its rebuild on.

### Roadmap re-cut

- **`future/scene-renderer.md`** — in "Still future" (the "BVH (or a cached, dirty-tracked scene
  bound)" bullet): the **BVH broadphase is delivered** (planset-23), rebuilt from the scene's
  spatial version behind the `GatherMeshes`/broadphase seam; the refinements are **incremental tree
  maintenance** (per-object insert/update/remove with fat boxes, for large *N*),
  **GPU/compute-driven culling** + **occlusion (hi-Z / two-phase)**, **per-submesh leaf granularity**
  (ties to planset-25), and **a Scene-shared tree** (one tree across consumers rather than one per
  renderer). Note planset-24's per-light shadow views are the broadphase's prime new consumer. Keep
  shadowed punctual lights named as the next renderer feature.
- **`future/README.md`** — the area-8 / culling paragraph and the area-9 summary: the **BVH
  broadphase is the delivered scaling step**, not "a cached scene bound or BVH"; incremental
  maintenance, GPU/occlusion, per-submesh, and a shared tree are the refinements behind it.
- **`plans/README.md`** — the planset-23 entry already exists: flip planset-23's status to **done**
  and update its one-line summary to the BVH broadphase. Adjust any cross-reference that calls
  planset-23 a "cache" or an "incremental BVH" (planset-24/25's dependency notes) to the BVH
  broadphase.
- Finalize the planset-23 status table (all plans `done`).

## Decisions

1. **A stat, not a knob.** The debug UI *shows* the broadphase working; there is nothing to configure
   (Plan 03 decision 4). The example's settings surface stays about genuine choices
   (bloom/shadows/AO/cull) and the broadphase about transparency.

2. **The pause-spin toggle is the honest demo.** A scene that mutates every frame can never show the
   static state, so the windowed app gains a switch that stops the per-frame Transform write — the
   smallest change that makes "moving → rebuilt" / "static → no rebuild" visible. Smoke mode is
   untouched, so the golden does not move.

3. **The refinements are recorded now, while the seam is fresh.** Incremental tree maintenance,
   GPU/occlusion culling, per-submesh leaves, and a Scene-shared tree all drop behind the
   `Build`/`Query` + version-gate surface. Capturing them at the moment the tree lands — when the
   trade-offs are clearest — is what makes the later exploration cheap to start. Roadmap notes, not
   commitments.

## Files

| File | Change |
|---|---|
| `examples/hello-triangle/.../*.cpp` | The `Broadphase: rebuilt/static` + node-count readout; a windowed-only pause-spin toggle. |
| `CLAUDE.md` | Scene/gather/cull paragraphs note the broadphase + spatial version. |
| `plans/future/scene-renderer.md` | BVH broadphase delivered; incremental maintenance + GPU/occlusion + per-submesh + shared-tree named. |
| `plans/future/README.md` | Same re-cut, area-8/9 paragraphs. |
| `plans/README.md` | Flip planset-23 to done; de-"cache"/de-"incremental" the cross-references. |
| `plans/planset-23/README.md` | Status table → all `done`. |

## Verification

- Clean build; the example links and the debug panel shows the readout (windowed run: `rebuilt` while
  the spinner turns, `static` with spin paused).
- `smoke_golden` **byte-identical** (this plan is UI + docs only; the pause-spin toggle is
  windowed-only and does not touch the smoke path). No golden regeneration.
- Full `ctest` green across all bands; validation gate clean under `VE_DEBUG`.
- The docs read as present-tense fact (no historical narrative), and every roadmap cross-reference
  (planset numbers, the refinements pointer) resolves.
