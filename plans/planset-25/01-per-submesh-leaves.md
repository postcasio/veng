# Plan 01 — Per-submesh bounds + per-submesh BVH leaves

**Goal:** refine the delivered `SceneBroadphase` from mesh-granularity leaves to **per-submesh
leaves**, so a frustum rejects an off-screen *submesh* of an on-screen mesh by the same
sub-linear tree descent — the "per-submesh leaves" refinement
[future/scene-renderer.md](../future/scene-renderer.md) names (line 546). Each `SubMesh` gains
a local-space `AABB` folded at load; `GatherMeshes`/`SceneBroadphase` build one BVH leaf per
submesh; the g-buffer and shadow passes draw the surviving submeshes. Pure CPU work,
byte-identical, no GPU machinery. Independent of every other plan; lands first.

## What lands

### Per-submesh local bound ([engine/include/Veng/Asset/Mesh.h](../../engine/include/Veng/Asset/Mesh.h) + src)

`SubMesh` ([Mesh.h:28](../../engine/include/Veng/Asset/Mesh.h)) gains a local-space `AABB
Bounds`, folded over the submesh's index range through the canonical vertex positions:

```cpp
struct SubMesh
{
    u32  IndexOffset = 0;
    u32  IndexCount  = 0;
    u32  MaterialIndex = NoMaterial;
    AABB Bounds = AABB::Empty();   ///< Local-space bound of this range's referenced vertices.
    static constexpr u32 NoMaterial = ~0u;
};
```

This is a **new** fold, distinct from `Mesh::ComputeBounds` ([Mesh.h:141,146](../../engine/include/Veng/Asset/Mesh.h)),
which folds a *contiguous* vertex span (or raw interleaved bytes) and does not index. A
per-submesh fold walks `[IndexOffset, IndexOffset + IndexCount)`, dereferences each `u32` index
into the vertex array, and expands the bound by that vertex's `Position`. Add a static helper
beside `ComputeBounds`:

```cpp
/// @brief Folds the local-space AABB of a submesh's referenced canonical vertices.
///
/// Walks the index range [indexOffset, indexOffset + indexCount) into vertices.
/// Zero indices yields AABB::Empty().
[[nodiscard]] static AABB ComputeSubMeshBounds(std::span<const CanonicalVertex> vertices,
                                               std::span<const u32> indices,
                                               u32 indexOffset, u32 indexCount);
```

A raw-bytes overload mirrors `ComputeBounds`'s second form for `MeshLoader`, which operates on
interleaved bytes + the index buffer rather than typed `CanonicalVertex`es.

### Populated on both construction paths

The bound is **derived at load and never serialized** (no cooked-format change), on each path
that builds a `Mesh`:

- **`MeshLoader`** ([engine/src/Asset/Loaders/MeshLoader.cpp](../../engine/src/Asset/Loaders/MeshLoader.cpp))
  — after the submesh table and the index/vertex bytes are read, fill each `SubMesh::Bounds`
  from the raw-bytes overload over that submesh's index range. The whole-mesh `Mesh::ComputeBounds`
  call it already makes is unchanged; this adds a per-submesh pass over the same bytes.
- **`Mesh::Create(Context&, const MeshData&, …)`** ([engine/src/Asset/Mesh.cpp](../../engine/src/Asset/Mesh.cpp))
  — the runtime `Primitives`/`MeshData` path. `MeshData` carries the canonical vertices, the `u32`
  indices, and the submesh table, so the typed `ComputeSubMeshBounds` fills each entry. The fill
  runs over the **resolved** submesh list, *after* the empty-`MeshData::SubMeshes` branch that
  synthesizes one whole-range submesh ([Mesh.cpp:62–72](../../engine/src/Asset/Mesh.cpp)) — so the
  synthesized submesh also gets a bound (the whole index range, equal to the mesh bound), not
  `AABB::Empty()`. (`Primitives::Cube`/`Plane`/`Sphere` produce a single submesh today, so its
  bound equals the mesh bound — correct and exercised by the existing primitive tests.)

### Per-submesh candidates + BVH leaves ([engine/include/Veng/Scene/Visibility.h](../../engine/include/Veng/Scene/Visibility.h) + [SceneBroadphase](../../engine/include/Veng/Scene/SceneBroadphase.h))

A draw candidate becomes a **(mesh-instance, submesh)** pair. `VisibleMesh`
([Visibility.h:19](../../engine/include/Veng/Scene/Visibility.h)) stays the per-instance gather
record (world matrix + resident `Mesh*` + the whole-mesh world bound — still what
`SceneBounds`/cascade fit and the shadow logic read), but the broadphase's **leaf granularity**
drops to the submesh:

- `SceneBroadphase::Rebuild` ([SceneBroadphase.cpp](../../engine/src/Scene/SceneBroadphase.cpp))
  walks each gathered `VisibleMesh`, and for each of `Mesh->GetSubMeshes()` pushes a
  `BVH::Leaf{ .Box = subMesh.Bounds.Transformed(world), .Id = <flat submesh-candidate index> }`.
  `BVH::Leaf` already carries the opaque `u32 Id` ([BVH.h:21](../../engine/include/Veng/Math/BVH.h)),
  so no BVH change is needed — only the leaf set grows from one-per-mesh to one-per-submesh.
- A parallel `vector<SubMeshCandidate>` (`{ u32 MeshCandidate; u32 SubMeshIndex; }`, or the
  resolved `{ const Mesh*; mat4 World; u32 SubMeshIndex; }`) maps a leaf `Id` back to what a
  draw needs. `GetCandidates()` continues to expose the per-mesh `VisibleMesh` span (the scene
  bound and shadow consumers are unchanged); a new `GetSubMeshCandidates()` exposes the flat
  per-submesh list a `Cull` id indexes.
- `Cull(frustum, out)` is unchanged in shape — it still appends BVH-survivor ids ascending — but
  the ids now index the per-submesh candidate list.

The rebuild gate (spatial version) and the pending-resident logic are untouched: per-submesh
leaves are a finer *expansion* of the same candidate set on the same rebuild trigger.

### The draw loop draws surviving submeshes ([engine/src/Renderer/SceneRenderer.cpp](../../engine/src/Renderer/SceneRenderer.cpp))

The g-buffer pass ([SceneRenderer.cpp:368–375](../../engine/src/Renderer/SceneRenderer.cpp))
changes from "cull meshes, draw all submeshes of each survivor" to "cull submeshes, draw each
surviving submesh":

```cpp
m_CullScratch.clear();
view.Broadphase->Cull(cameraFrustum, m_CullScratch);          // now per-submesh ids
for (const u32 id : m_CullScratch)
{
    const SubMeshCandidate& c = candidates[id];
    DrawSubMesh(c);                                           // one submesh, its material
    ++m_LastDrawn;                                            // now per-submesh-draw granularity
}
```

`DrawSubMesh` is the body of today's per-submesh loop ([SceneRenderer.cpp:349–363](../../engine/src/Renderer/SceneRenderer.cpp))
hoisted to take a single candidate: bind the mesh's vertex/index buffers (once per distinct
mesh — track the last-bound mesh to skip redundant binds, which is sound because the candidate
list is in `GatherMeshes` order so a mesh's submeshes are contiguous; Plan 05's GPU path
re-establishes that contiguity by grouping survivors per mesh after compaction reorders them),
bind the submesh's material, push the MVP + normal matrix, `DrawIndexed`.

**Every `Cull` caller moves together**, because `Cull` now returns per-submesh ids while
`view.Visible` stays per-mesh (decision 2) — a survivor id that still indexed the per-mesh span
would be out of range. The three callers are the g-buffer geometry pass
([SceneRenderer.cpp:371](../../engine/src/Renderer/SceneRenderer.cpp)), the **cascaded** shadow
pass ([ShadowScenePass.cpp:212,215](../../engine/src/Renderer/ShadowScenePass.cpp), per-cascade
light frustum), and the **punctual** shadow pass
([PunctualShadowScenePass.cpp:194,197](../../engine/src/Renderer/PunctualShadowScenePass.cpp), per
spot/cube-face frustum); each switches from `view.Visible[idx]` to indexing the per-submesh
candidate list and drawing the surviving submesh.

### A submesh-granularity drawn counter

`m_LastDrawn` ([SceneRenderer.cpp:365,390](../../engine/src/Renderer/SceneRenderer.cpp)) is
incremented **once per mesh** today (outside the submesh loop), so it counts meshes, not draws —
it **cannot observe a per-submesh cull** (dropping a submesh of a drawn mesh leaves the
mesh-count unchanged). Move the increment **inside** the per-candidate loop so
`GetLastDrawnCount()` reports the number of submesh draws issued. `GetLastVisibleCount()`
likewise reports the per-submesh candidate count (`GetSubMeshCandidates().size()`). This is
what makes the Plan-01 draw-count fixture meaningful.

## Decisions

1. **Per-submesh leaves, not a linear sub-scan.** Expressing per-submesh culling as a per-mesh
   BVH descent followed by a linear `Intersects` over each survivor's submeshes would regress
   the sub-linearity planset-23 delivered (a 100-submesh mesh straddling the frustum edge would
   pay 100 linear tests per view). One leaf per submesh keeps the whole cull a single tree
   descent — the only choice consistent with the delivered broadphase and the granularity the
   GPU path (Plan 05) uploads. The cost side, stated: one leaf per submesh multiplies the BVH leaf
   count (and so rebuild time, which fires on a spatial-version bump) by the average
   submeshes-per-mesh factor, and under `CullMode::GPU` the per-frame candidate upload is one
   record per surviving *submesh* (Plan 05's `CullCandidate`, ~112 bytes with the `mat4`), not per
   mesh — a real multiplier on a detailed scene that the one-submesh smoke scene cannot reveal. It
   rides the same broadphase the planset already scales; finer granularity is the point.

2. **`VisibleMesh` stays per-mesh; the leaf/candidate granularity is per-submesh.** The scene
   bound, the cascade fit, and the shadow-view logic read per-mesh world bounds and gain nothing
   from submesh granularity; only the *cull and draw* go finer. Keeping `VisibleMesh` as the
   gather record and adding a flat per-submesh candidate list beside it is the smallest change
   that does not disturb the bound/cascade consumers.

3. **Bounds derive at load on both paths, never serialized.** A cooked-format change to store
   per-submesh bounds would version the archive for a value trivially recomputed from data
   already present. Folding at load mirrors the whole-mesh `Mesh::GetBounds()` precedent
   (planset-20) and keeps `.vengpack` v2 unchanged.

4. **The drawn counter moves to per-submesh granularity.** A mesh-count stat cannot guard a
   submesh cull. The counter already exists and is only read by the debug UI and the draw-count
   tests, so moving the increment inside the loop is free and makes the guard real.

## Files

| File | Change |
|---|---|
| `engine/include/Veng/Asset/Mesh.h` | `SubMesh::Bounds`; the `ComputeSubMeshBounds` typed + raw-bytes static helpers. |
| `engine/src/Asset/Mesh.cpp` | `ComputeSubMeshBounds` impl; fill each submesh bound in the `MeshData` `Create` path. |
| `engine/src/Asset/Loaders/MeshLoader.cpp` | Fill each `SubMesh::Bounds` from the index range over the raw vertex bytes at load. |
| `engine/include/Veng/Scene/Visibility.h` | The flat `SubMeshCandidate` record (and, if typed-resolved, its fields). |
| `engine/include/Veng/Scene/SceneBroadphase.h` + `engine/src/Scene/SceneBroadphase.cpp` | Build one leaf per submesh in `Rebuild`; the per-submesh candidate list; `GetSubMeshCandidates()`. |
| `engine/src/Renderer/SceneRenderer.cpp` | `DrawSubMesh`; per-submesh cull loop in the g-buffer pass; per-submesh `m_LastDrawn`; `GetLastVisibleCount()`/`GetLastDrawnCount()` over submesh granularity. |
| `engine/src/Renderer/ShadowScenePass.cpp` (+ `.h`) | Per-submesh draw against each cascade frustum: index the per-submesh candidate list, draw the surviving submesh (the `Cull`-id semantics changed). |
| `engine/src/Renderer/PunctualShadowScenePass.cpp` (+ `.h`) | The identical per-submesh change for the spot/cube-face shadow views. |
| `tests/unit/bounds.cpp` (or a new `submesh_bounds.cpp`) + the suite source list | Device-free per-submesh fold + leaf-equivalence tests. |
| `tests/gpu/scene_renderer.cpp` | The partially-off-frustum draw-count fixture. |

## Verification

- Clean build; `include_hygiene` still compiles `Veng/Scene/Visibility.h` / `SceneBroadphase.h`
  / `Veng/Asset/Mesh.h` (glm-only, no backend leak).
- **Device-free unit tests:**
  - `ComputeSubMeshBounds` over a known index range folds exactly the referenced vertices'
    bound; a range covering all indices equals `Mesh::ComputeBounds` over the whole vertex span;
    zero indices → `AABB::Empty()`.
  - **Leaf-equivalence:** for a multi-submesh mesh, the union of the per-submesh world bounds
    equals the whole-mesh world bound, and a `Cull` over a frustum that contains the whole mesh
    returns every submesh of it (no submesh wrongly dropped); a frustum that misses one submesh's
    bound (a wide mesh half outside) drops exactly that submesh's candidate.
- **GPU draw-count fixture** (`gpu` band): a mesh with two well-separated submeshes positioned so
  the camera frustum contains one and excludes the other → `GetLastDrawnCount()` reports **1**
  submesh, not 2 (and not the whole-mesh count); with the camera framing both, it reports 2.
  This is the test the per-submesh drawn counter exists for.
- **Shadow passes exercised:** `smoke_golden` renders through the cascaded and (when a punctual
  caster is present) punctual shadow passes, both of which now index the per-submesh candidate
  list — an out-of-range id from a missed caller surfaces as a crash or a moved golden, so the
  existing golden run covers the shadow-side index change.
- **`smoke_golden` byte-identical** — a submesh the frustum touches still draws, so the visible
  set and the pixels are unchanged; re-check and do not regenerate.
- Full `ctest` green across the unit/death/cooker bands and the `gpu` band where present;
  validation gate clean under `VE_DEBUG` (no barrier/topology change in this plan).
</content>
