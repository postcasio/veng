# planset-23 — frustum-cull BVH broadphase

**Phase goal:** build the **spatial broadphase** the
[planset-20](../planset-20/README.md) bounds reduction and the
[planset-21](../planset-21/README.md) frustum-cull gather both named — a **bounding
volume hierarchy** over the resident draw candidates so frustum culling stops being a
linear scan. Today every view re-tests **every** candidate one at a time: the g-buffer
pass scans the whole list against the camera frustum
([SceneRenderer.cpp:322](../../engine/src/Renderer/SceneRenderer.cpp)), and **each**
shadow cascade scans it again against its own light frustum
([ShadowScenePass.cpp:179](../../engine/src/Renderer/ShadowScenePass.cpp)) — so *N*
candidates over *C* cascades cost *N·(C+1)* `Intersects` calls per frame. A BVH replaces
each scan with a **tree descent** (a node wholly outside a frustum rejects its subtree in
one test). The tree is **rebuilt only when the scene's spatial state actually changes**,
gated by a single scene-version counter — so a static scene rebuilds not at all and every
view queries a stable tree, and the **many-view workload**
[planset-24](../planset-24/README.md) brings (a shadow frustum per punctual light)
queries **one tree, many times**.

The rendered image is **byte-identical**: a query returns exactly the linear scan's set,
the candidate list stays in `GatherMeshes` order so draw order is unchanged, and the
opaque deferred g-buffer is order-independent regardless — so `smoke_golden` **must not
move** (a moved golden here is a bug).

## The maintenance signal: version-gated rebuild, not incremental update

A tree is only useful if it stays current with the scene cheaply. veng's immediate-mode
ECS hands out a **mutable** `Transform&` (`Scene::Get`, `View`/`Each`), so a write is
invisible — there is no setter to hook, and the renderer even `const_cast`s its `const
Scene&` to iterate lights
([SceneRenderer.cpp:1579](../../engine/src/Renderer/SceneRenderer.cpp)). So veng must
**recover** the "did anything move?" signal a physics API is handed at its setter.

It does it with the cheapest signal that is correct: a **single Scene-owned spatial
version counter** (decision 1). `Scene` bumps it whenever a **spatial** pool (`Transform`,
`Parent`, `MeshRenderer`) is *structurally changed* or *accessed non-`const`* (a potential
in-place edit). A broadphase caches the version it last built against; if the version is
unchanged, it does nothing — the O(1) "is there work?" check. If it moved, the broadphase
**rebuilds the tree from scratch** over the current candidates. A matching **`const`
iteration path** (decision 2) lets read-only consumers iterate **without** bumping the
version, so a static scene's tree is genuinely untouched.

This is the **access-as-write change-tick** the mainstream ECS engines ship (Bevy's
`Changed<T>`, Unity DOTS's per-chunk change versions) — reduced to its minimal form: one
counter, no per-entity journal, no cursors, no trim. We do **not** add an explicit
`SetTransform`/`MarkDirty` API (fragile — a forgotten mark is a silently stale render),
and we do **not** maintain the tree incrementally (insert/update/remove with fat boxes).
Per-frame rebuild over veng's candidate counts (tens to hundreds today) is microseconds
and yields a fresh, quality-optimal tree; **incremental maintenance is recorded as the
refinement** for when *N* grows into the thousands — built behind the same seam, not ahead
of the workload that needs it.

## Scope decisions

1. **The change signal is a single Scene-owned spatial version counter.** `Scene` bumps a
   monotonic `u64 m_SpatialVersion` on: `DestroyEntity` of an entity holding a spatial
   component; `Add`/`Remove` of a **spatial pool** (`Transform`, `Parent`, `MeshRenderer`);
   any **non-`const`** `TryGetRaw` of a spatial pool; and a `ForEachComponent` visit of one
   (the editor inspector's erased-`void*` edit path). A consumer caches the version it last
   built against — `version == cached` means nothing spatial changed. The rejected
   alternatives: an explicit `MarkTransformDirty(Entity)` (fragile — a forgotten mark is a
   silently stale render) and a **per-entity journal** with cursor replay + trimming
   (machinery neither Bevy's change-tick nor DOTS's chunk-version use, whose per-entity
   granularity only pays under an individual-`Get` access pattern veng does not have — a
   non-`const` `View<Transform>` re-dirties the whole pool, collapsing the journal to the
   same per-frame cost as a rebuild with far more complexity and a retained-reference hole).

2. **A `const` `View`/`Each` path lands with it.** Because a non-`const` spatial access
   bumps the version, a read-only consumer needs a `const` way to iterate or it bumps the
   version every frame (forcing a rebuild). Plan 01 adds `const`-qualified `View`/`Each`
   forwarding to the `const` `TryGetRaw`, and the renderer's light pack drops its
   `const_cast`. A general win independent of the tree.

3. **Rebuild on change, not incremental update.** When the version moved, the broadphase
   re-gathers the candidates (reusing the pure `GatherMeshes`) and **rebuilds the tree from
   scratch** — *O(N log N)*, a fresh balanced tree. No fat boxes, no per-entity proxy map,
   no hierarchy cascade: a rebuild walks each candidate's parent chain fresh through
   `WorldMatrix`, so a moved parent's descendants are correct for free. **Incremental
   insert/update/remove** (fat-box no-op within a margin, rotation balancing) is the named
   refinement behind the same seam, for when per-frame rebuild cost matters.

4. **Byte-identical output is the contract.** A query descends **internal** nodes by their
   (enclosing) box and accepts a **leaf** only by its stored **tight** box, with the same
   `Intersects` the linear scan uses — so the query result equals the tight linear scan
   **exactly** (every leaf whose tight box hits the frustum sits under ancestors whose boxes
   also hit, so it is always reached and tight-tested). The candidate list stays in
   `GatherMeshes` order and `Cull` returns the survivors **ascending**, so draw order matches
   today's; the opaque deferred g-buffer is draw-order-independent regardless.
   `smoke_golden` does not move and planset-21's draw-count test is unchanged. The guard is a
   *pure* equivalence test (Plan 02): query == linear tight scan over randomized builds and
   many frustums.

5. **The tree is consumer-owned, persistent across frames.** A `SceneBroadphase` (Plan 03)
   owns the tree, the candidate list, and the last-seen version, and threads its query into
   the passes per frame. It is **not** `Scene` state: one `Scene` is read by N renderers
   (editor previews), each wanting its own tree lifetime, and a tree maintained through a
   `const Scene&` would be a `mutable`-state lie. The `Scene` owns only the version counter
   (the record that its own spatial state moved); each broadphase derives its tree and
   candidate set from the scene on the frames the version moves.

6. **No render-topology change, no new GPU resource.** A CPU data-structure planset: no pass
   added or removed, no descriptor/image/buffer change, no shader touched. `SceneRenderer`
   gains a broadphase member and the passes query it; the compiled graph is untouched. No
   `Configure` recompile and no `GetOutput()` invalidation.

## Plans

| # | Plan | Summary | Status |
|---|---|---|---|
| 01 | [Scene spatial-version, recursive destroy + `const` iteration](01-scene-change-tracking.md) | `Scene` bumps a monotonic `u64 m_SpatialVersion` on spatial-pool (`Transform`/`Parent`/`MeshRenderer`) structural change, non-`const` `TryGetRaw`, and `ForEachComponent` visits; `GetSpatialVersion()` is the cheap "anything moved?" check. Make `DestroyEntity` recursive (destroy the subtree), closing the dangling-`Parent` crash at the source. Add `const`-qualified `View`/`Each`; drop the renderer's light-pack `const_cast`. Device-free unit tests pin the bump/no-bump matrix and recursive destroy. | done |
| 02 | [The BVH](02-dynamic-bvh.md) | New `Veng/Math/BVH.h` (+ src): a bounding volume hierarchy beside `AABB`/`Frustum` — `Build(span<leaf>)` (top-down, surface-area split) and a `Query(const Frustum&, out)` that descends by node box and accepts leaves by **tight** box (byte-identical to a tight scan). Pure, device-free. Equivalence + validity tests over randomized builds and frustums. | ready |
| 03 | [`SceneBroadphase` + wire the renderer](03-wire-renderer.md) | A renderer-owned `SceneBroadphase`: caches the spatial version, re-gathers (via `GatherMeshes`) + rebuilds the tree when it moves (and while meshes are still resolving residency), and exposes `GetCandidates()` + `Cull(frustum, out)`. The g-buffer and shadow-cascade passes query it instead of scanning. Golden **byte-identical**; draw-count test **unchanged**; `DidRebuildLastSync()` stat. Device-free integration test + validation gate. | ready |
| 04 | [Debug/stats exposure + docs/roadmap re-cut](04-debug-docs.md) | Surface rebuilt-this-frame/node-count/visible/drawn in the example debug UI; a pause-spin toggle so a still scene's zero-rebuild state is demonstrable. Document the broadphase in `CLAUDE.md`. Mark the **BVH broadphase delivered** in `future/`, and name **incremental maintenance**, **GPU/occlusion culling**, **per-submesh leaves**, and **a shared scene tree** as the refinements. Finalize the status table and `plans/README.md`. | ready |

## Dependency analysis

```
01 (spatial version + const iteration)
   │
   ├──► 03 (SceneBroadphase + wire) ──► 04 (debug + docs)
   │        ▲
02 (BVH build + query) ─┘
```

- **Plan 01** (the change signal + `const` iteration) and **Plan 02** (the BVH) are
  **independent** — one is a `Scene` change, the other a pure math primitive. Both are
  device-free and fully unit-tested with no GPU. They may land in parallel.
- **Plan 03** is the only render-path change: it consumes both (the version as the rebuild
  gate, the tree as the structure) behind a renderer-owned `SceneBroadphase` that rebuilds on
  demand and wires the query into the passes. The golden stays byte-identical; the draw-count
  test is unchanged.
- **Plan 04** is the debug/stats surface + docs/roadmap, last.

Order: **(01 ∥ 02) → 03 → 04**. Plans 01–02 land their value in unit tests before any GPU
work; the only render-path change is concentrated in 03, and it moves no golden.

## Process & conventions

Same cadence as every planset: implement → migrate any caller in the same pass → verify (clean
build, `ctest` green across the unit/death/cooker bands and the `gpu` band where a device is
present, the smoke PPM correct size + exit 0, the `smoke_golden` capture **re-checked and unmoved**)
→ update this table → one commit per plan (`Plan NN: <summary>`, `Co-Authored-By` trailer).

Common to all plans:

- **`smoke_golden` stays byte-identical across the whole planset.** A query returns the linear
  scan's exact set in `GatherMeshes` order, and the opaque deferred g-buffer is
  draw-order-independent, so the image does not move — a moved golden is a bug. No golden
  regeneration.
- **The cull guard is a pure equivalence test** (Plan 02): query == linear tight scan over
  randomized builds and frustums, including empty/single/degenerate trees. **A device-free
  `SceneBroadphase` integration test** (Plan 03) pins the version-gate → rebuild → cull path on
  the primary no-ICD dev path; the rebuilt-this-frame stat (Plan 03/04) shows the tree
  *changed*, the equivalence test shows it stayed *correct*.
- **The version counter's conservatism is named, not hidden.** A non-`const` spatial access
  bumps the version even when it was a read (decision 1); the `const` iteration path (decision 2)
  is the read-only escape hatch. **One constraint to honor:** a `Transform&` retained across
  frames and written without re-acquiring it each frame bypasses the version bump — write
  transforms through the scene accessors per frame, as all engine and sample code does.
- **Run the validation gate under `VE_DEBUG`** (`ctest --test-dir build-debug -L validation`): the
  broadphase changes no barrier, layout, or draw set, so the gate stays clean; pin it.
- **Contract comments are present-tense facts** — the `CLAUDE.md` comment policy applies; the
  existing "recompute-on-demand, no cached visibility" wording becomes "served through a BVH
  broadphase rebuilt when the scene's spatial version moves," with no historical narrative.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.

## On completion

Frustum culling is **sub-linear**: the engine maintains one BVH over the draw candidates, the
camera and every shadow cascade query it by descent, and the tree is rebuilt only on a frame the
scene's spatial version moved — a static scene rebuilds not at all. The signal that gates it,
recovered from the immediate-mode ECS, is a single Scene spatial-version counter the broadphase
compares each frame; the `GatherMeshes`/`Frustum`/`AABB` primitives stay pure and the image is
byte-identical. **Incremental tree maintenance** (fat boxes + per-object insert/update/remove),
**GPU/occlusion culling**, **per-submesh leaf granularity**, and **a Scene-shared tree** are
recorded as the next refinements behind this same seam — and planset-24's per-light shadow views
are its first new consumer.
