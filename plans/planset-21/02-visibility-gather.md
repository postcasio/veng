# Plan 02 — The per-frame visibility gather

**Goal:** reduce a `Scene` to the per-frame list of resident mesh instances a renderer draws —
each carrying its world matrix and world-space bound — computed **once per frame** and reused by
both scene passes (replacing their per-draw, per-cascade world-matrix re-walks). This is the
shared candidate set both passes cull (Plan 03), and the seam a future BVH replaces. Pure and
device-free, like Plan 01.

## What lands

### `VisibleMesh` + `GatherMeshes` ([engine/include/Veng/Scene/Visibility.h](../../engine/include/Veng/Scene/Visibility.h), new + src)

A new `Veng/Scene/` header beside `Transforms.h` (the same scene-query home as `SceneBounds`),
depending only on the scene + asset surface — no renderer type.

```cpp
namespace Veng
{
    // One resident drawable CANDIDATE (pre-cull — the name reflects "a mesh that may
    // be visible," not a post-cull result): a (Transform, MeshRenderer) entity's
    // world matrix, its world-space bound, and its resident mesh. Built per frame and
    // borrowed for the frame — Mesh points into the MeshRenderer's resident
    // AssetHandle, valid for exactly the Execute that gathered it because no garbage
    // collection or handle mutation runs mid-Execute (the record-time stability the
    // light pack relies on). A future cross-frame consumer (TAA history) must not
    // reuse the span across frames.
    struct VisibleMesh
    {
        Entity      Owner;
        mat4        World;
        AABB        WorldBounds;
        const Mesh* Mesh;
    };

    // Gather every resident (Transform, MeshRenderer) entity into `out` (cleared
    // first) and union their world bounds into `outBounds` (AABB::Empty() when none).
    // One pass over the Transform pool for the world matrices; each entry's
    // WorldBounds = Mesh->GetBounds().Transformed(world). Non-resident mesh handles
    // are skipped (an async-loading scene gathers what is loaded). NO culling — this
    // is the unculled candidate set; a consumer applies Intersects per frustum.
    // outBounds is exactly SceneBounds(scene), produced as a free by-product of the
    // same pass — the renderer feeds it to ComputeCascades and skips a second
    // SceneBounds call. Recompute-on-demand, no cached visibility.
    void GatherMeshes(const Scene& scene, vector<VisibleMesh>& out, AABB& outBounds);
}
```

`GatherMeshes` **subsumes** `SceneBounds`: in one walk of the Transform pool's dense order it both
*lists* each resident drawable and *unions* their bounds into `outBounds`. For each entity with a
**resident** `MeshRenderer` (`Mesh.IsLoaded()`) it computes the world matrix, transforms the mesh
bound, pushes a `VisibleMesh`, and expands `outBounds`. The world matrix is the same `WorldMatrix`
walk `ComputeWorldMatrices` performs (it may reuse `ComputeWorldMatrices` into renderer scratch, or
compute inline). The amortization is that this runs **once per frame** and both scene passes (and
every cascade) reuse `item.World` — **not** that the parent-chain walk is memoized within the pass:
`ComputeWorldMatrices` is itself a per-entity `WorldMatrix` loop, not a shared-subtree cache, so the
plan does not claim intra-pass sharing (memoizing parent worlds is a separate, deferred win). The
`out` vector is caller-owned and reused across frames (the renderer holds one as scratch, like its
light-pack vector), so the steady state allocates nothing new.

The list comes out in **Transform-pool dense order** (the gather's walk order), which differs from
the passes' former `Each<Transform, MeshRenderer>` order. Byte-identity (Plan 03) relies on the
opaque g-buffer and depth-only shadow passes being **draw-order-independent**; a future alpha-blended
consumer of this list must sort, not assume gather order.

The material-readiness check the draw loops perform (`all materials IsLoaded`) stays in the
**consumer** (Plan 03), not the gather: the gather is a pure scene→geometry reduction shared by
any consumer, and "are this mesh's materials ready to bind" is a draw-time concern. The gather
includes a mesh as soon as its geometry bound is known; the pass skips a not-yet-ready material
set at record time exactly as today. So the gathered candidate count (`out.size()`) counts
**geometry-resident** meshes — a mesh whose materials are still loading is counted yet not drawn, so
the "total" in Plan 04's visible/total stat can legitimately exceed the drawn count during async
material load, even with culling off.

## Decisions

1. **The gather subsumes `SceneBounds` — one pass, both outputs.** A list *and* its bound-union fall
   out of the same walk of the resident `(Transform, MeshRenderer)` set, so `GatherMeshes` produces
   `outBounds` (= `SceneBounds(scene)`) as a free by-product and the renderer **drops its separate
   `SceneBounds` call** (Plan 03), leaving exactly one world-matrix pass per frame. `SceneBounds`
   survives as the standalone public primitive for other callers (editor focus, tests); it is just no
   longer invoked in the render hot path. One definition of "the scene's resident drawables," one
   place a BVH later optimizes.

2. **No culling in the gather.** The gather produces the **unculled** candidate set; the cull is
   the consumer's, because the two consumers cull against **different** frustums (camera vs. each
   cascade) over the **same** gathered list. Folding a single frustum into the gather would force
   a re-gather per frustum — the opposite of the "gather once" goal.

3. **`const Mesh*`, borrowed for the frame.** A `VisibleMesh` borrows the resident mesh pointer
   (checked `IsLoaded()` at gather time) rather than copying an `AssetHandle` — the gather and its
   consumers all run inside one `Execute`, within which the scene and its handles are stable, so a
   borrowed pointer is sound and copy-free. This matches the per-frame, read-only lifetime of the
   list.

4. **Recompute-on-demand, no cached visibility.** The gather re-runs every `Execute`, mirroring
   `SceneBounds`/`ComputeWorldMatrices`. A persistent, dirty-tracked visible set (or a BVH the
   gather queries instead of a linear walk) is the scaling step shared with the `SceneBounds`
   reduction — deferred, and the gather is its seam.

5. **Lives in `Veng/Scene/`, not the renderer.** Producing world matrices + world bounds from a
   `Scene` is scene-query math (no `vk::`, no renderer type), so it sits beside `SceneBounds` and
   is reusable by anything that needs the resident-drawable set (a future picking query, an
   editor focus-on-selection). The renderer is merely its first consumer.

## Files

| File | Change |
|---|---|
| `engine/include/Veng/Scene/Visibility.h` (new) | `VisibleMesh` + `GatherMeshes` declaration. |
| `engine/src/Scene/Visibility.cpp` (new) | `GatherMeshes` body (one Transform-pool pass: lists `VisibleMesh`es + unions `outBounds`). |
| `engine/CMakeLists.txt` | Add `src/Scene/Visibility.cpp`. |
| `tests/unit/visibility.cpp` (new) + the unit suite source list | Device-free gather tests. |

## Verification

- Clean build; `include_hygiene` compiles `Veng/Scene/Visibility.h` (scene + asset only — no
  backend leak).
- **`tests/unit/visibility.cpp`** (device-free, no ICD — the `bounds.cpp` pattern: `Mesh`es built
  through the `MeshInfo` factory carry only a name + bound, empty buffers, enough for the gather to
  read):
  - A `Scene` with two mesh entities at known transforms → `GatherMeshes` returns **two**
    `VisibleMesh`es with the expected world matrices and world bounds (each
    `GetBounds().Transformed(world)`), in the Transform pool's dense order.
  - A `(Transform, MeshRenderer)` entity whose mesh handle is **non-resident** contributes
    nothing; an entity with a `Transform` but no `MeshRenderer` contributes nothing.
  - An empty scene → an empty list.
  - The `outBounds` a gather returns **equals `SceneBounds(scene)`** (the by-product agrees with the
    standalone primitive) — the cross-check pinning that the renderer can drop its separate
    `SceneBounds` call.
  - Passing a pre-filled `out` vector → it is cleared first (no stale entries).
- `smoke_golden` is **byte-identical** — Plan 02 adds no rendering.
- Full `ctest` green across the unit/death/cooker bands and the `gpu` band where present.
</content>
