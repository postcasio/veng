# Plan 03 — `SceneBroadphase` + wire the renderer

**Goal:** tie the spatial version (Plan 01) and the BVH (Plan 02) together — a renderer-owned
`SceneBroadphase` that compares the scene's spatial version each frame, re-gathers and rebuilds the
tree when it moved (and while meshes are still resolving residency), and exposes a frustum query the
passes call instead of scanning. The image is **byte-identical**, planset-21's draw-count test is
unchanged, and a rebuilt-this-frame stat is the move's evidence. Consumes Plan 01 + Plan 02.

## What lands

### `SceneBroadphase` ([engine/include/Veng/Scene/SceneBroadphase.h](../../engine/include/Veng/Scene/SceneBroadphase.h), new + src)

A consumer-owned object that keeps a `BVH` current with a `Scene` by rebuilding on a version change.
It holds the tree, the gathered candidate list, the last-seen version, and the small set of
candidates still resolving residency.

```cpp
namespace Veng
{
    class SceneBroadphase
    {
    public:
        // Bring the tree current with `scene`. Rebuilds (re-gather + BVH::Build)
        // iff the scene's spatial version moved since the last sync, or a mesh that
        // was still loading has become resident. A cheap no-op otherwise.
        void Sync(const Scene& scene);

        // The live draw candidates, in GatherMeshes order; a Cull id indexes this.
        [[nodiscard]] std::span<const VisibleMesh> GetCandidates() const { return m_Candidates; }

        // Append the candidate indices visible to `frustum`, ascending (so draws
        // issue in GatherMeshes order, unchanged from the pre-BVH path). `out` is
        // the caller's reused scratch (cleared by the caller).
        void Cull(const Frustum& frustum, vector<u32>& out) const;

        // The union of the live candidates' world bounds (for ComputeCascades) —
        // exactly the outBounds the gather produced, i.e. SceneBounds(scene).
        [[nodiscard]] AABB GetSceneBounds() const { return m_SceneBounds; }

        // Whether the most recent Sync rebuilt the tree (false on a fully static
        // frame). Diagnostics; the rendered image is identical regardless.
        [[nodiscard]] bool DidRebuildLastSync() const { return m_DidRebuild; }
        [[nodiscard]] u32  GetNodeCount() const { return m_Tree.GetNodeCount(); }

    private:
        void Rebuild(const Scene& scene); // re-gather, build, refresh the pending set

        BVH                 m_Tree;
        vector<VisibleMesh> m_Candidates;      // dense, GatherMeshes order; Cull id == index
        vector<BVH::Leaf>   m_LeafScratch;     // reused: tight box + candidate index per leaf
        vector<Entity>      m_Pending;         // (Transform,MeshRenderer) entities not yet IsLoaded
        AABB                m_SceneBounds = AABB::Empty();
        u64                 m_LastVersion = ~0ull; // != any real version → first Sync rebuilds
        bool                m_DidRebuild  = false;
    };
}
```

**`Sync(scene)`** is a gate, not a maintenance loop:

1. `version = scene.GetSpatialVersion()`.
2. `needRebuild = (version != m_LastVersion)`. If not, and `m_Pending` is non-empty, poll **only
   those** entities: if any is still alive and its mesh is now `IsLoaded()`, set `needRebuild`
   (a load completed — the candidate set grew without a spatial mutation, so the version did not
   move). A dead pending entity is dropped.
3. If `needRebuild`: `Rebuild(scene)` and set `m_LastVersion = version`, `m_DidRebuild = true`.
   Else `m_DidRebuild = false` — the tree, candidate list, and bounds stand untouched.

**`Rebuild(scene)`**:

- `GatherMeshes(scene, m_Candidates, m_SceneBounds)` — the existing pure gather fills the candidate
  list (resident only, in `Transform`-dense order) and the bound in one pass.
- Build `m_LeafScratch` as `{ candidate.WorldBounds, index }` over `m_Candidates`, and
  `m_Tree.Build(m_LeafScratch)`.
- Refresh `m_Pending`: one `const View<Transform, MeshRenderer>` pass recording the entities whose
  mesh is not yet `IsLoaded()` (the `const` view does not bump the version). While any candidate's
  mesh is still loading, `m_Pending` is non-empty and step 2 keeps polling it; a permanently
  unloadable mesh stays in the set and is cheaply re-checked each frame.

`Cull` delegates to `m_Tree.Query` and sorts the returned candidate indices ascending — so the
g-buffer and shadow passes issue draws in `GatherMeshes` order, byte-identical to today.

> **Why rebuild, not incremental.** A from-scratch rebuild re-derives the whole tree (and every
> world matrix, through `GatherMeshes`' `WorldMatrix` walk), so a moved parent's descendants, a
> reparent, and a destroy are all correct with no per-entity bookkeeping — the cost is one
> *O(N log N)* build on the frames the scene's spatial version moved, microseconds at veng's candidate
> counts. `GatherMeshes` (`Visibility.h`) stays the pure one-shot gather; the broadphase is the
> version-gated, tree-building consumer on top of it. Incremental insert/update/remove is the named
> refinement for when *N* makes the rebuild cost matter.

### The cached gather in `Execute` ([engine/src/Renderer/SceneRenderer.cpp](../../engine/src/Renderer/SceneRenderer.cpp))

Replace the `m_VisibleMeshes` scratch + `GatherMeshes` call
([SceneRenderer.cpp:451](../../engine/include/Veng/Renderer/SceneRenderer.h),
[Execute](../../engine/src/Renderer/SceneRenderer.cpp)) with a member `SceneBroadphase
m_Broadphase`:

```cpp
m_Broadphase.Sync(view.World);
AABB sceneBounds        = m_Broadphase.GetSceneBounds(); // feeds ComputeCascades unchanged
resolvedView.Visible    = m_Broadphase.GetCandidates();  // the candidate span, as before
resolvedView.Broadphase = &m_Broadphase;                 // the tree, for the passes to query
```

`SceneView` gains `const SceneBroadphase* Broadphase` beside `std::span<const VisibleMesh> Visible`.
`ComputeCascades` still reads `sceneBounds` (the gather's `outBounds`, which is `SceneBounds(scene)`
— byte-identical to today); nothing else downstream changes shape.

### The passes query the tree ([SceneRenderer.cpp:322](../../engine/src/Renderer/SceneRenderer.cpp), [ShadowScenePass.cpp:179](../../engine/src/Renderer/ShadowScenePass.cpp))

Both cull sites today iterate the full `view.Visible` span and `continue` on a failed `Intersects`.
Replace the scan with a tree query into a **pass-owned reused scratch** `vector<u32>`:

```cpp
if (m_FrustumCull)
{
    m_CullScratch.clear();
    view.Broadphase->Cull(cameraFrustum, m_CullScratch);   // cascadeFrustum in the shadow pass
    for (u32 idx : m_CullScratch) { const VisibleMesh& item = view.Visible[idx]; /* ...same body... */ }
}
else
{
    for (const VisibleMesh& item : view.Visible) { /* ...same body... */ }
}
```

The per-item body (materials-ready check, vertex/index bind, MVP/normal push, per-submesh draw) is
**unchanged**. With cull on, the query returns the tight scan's exact set in ascending candidate
order; with cull off, the full span iterates as before.

### The rebuild signal ([engine/include/Veng/Renderer/SceneRenderer.h](../../engine/include/Veng/Renderer/SceneRenderer.h))

Beside `GetLastVisibleCount()` / `GetLastDrawnCount()`:

```cpp
// Whether the broadphase rebuilt its tree during the most recent Execute
// (false on a fully static frame — the scene's spatial version was unchanged).
// Diagnostics; the rendered image is identical regardless. Backed by
// SceneBroadphase::DidRebuildLastSync().
[[nodiscard]] bool DidBroadphaseRebuildLastFrame() const;
```

`GetLastVisibleCount()` is **re-pointed** at `m_Broadphase.GetCandidates().size()` (the
`m_VisibleMeshes` member it read is gone); `GetLastDrawnCount()` is unchanged.

## Decisions

1. **The broadphase replaces the gather scratch and owns the candidate cache.** The renderer no
   longer re-gathers every frame; it `Sync`s, and the broadphase re-gathers + rebuilds only when the
   spatial version moved (or a mesh finished loading). A static scene does zero work beyond the
   version compare.

2. **Residency is a second rebuild trigger, distinct from the version.** A mesh finishing async load
   does not mutate the `Scene`, so it does not bump the spatial version; the broadphase tracks the
   small set of not-yet-resident candidates and rebuilds when one becomes resident — so the candidate
   set still tracks residency exactly as the old per-frame `GatherMeshes` did. (`GatherMeshes` itself
   stays the pure, journal-free gather.)

3. **No broadphase reset on `Resize`/`Configure`.** The tree depends only on the scene (its spatial
   version), not the renderer's extent, format, or topology. A resize recreates render targets but
   moves no world matrix and changes no candidate, so the cached tree stays valid and the version is
   unmoved — the next `Sync` is a no-op. (The broadphase holds no GPU resource, so no retire-path
   interaction.)

4. **The signal is a stat, not a setting.** `DidBroadphaseRebuildLastFrame()` is read-only
   diagnostics — there is no knob to disable the tree. The existing `FrustumCull` toggle stays a
   genuine human debug switch (cull on → tree query; off → full iterate).

5. **Byte-identical render; the draw-count test is unchanged.** The query returns the scan's exact
   set (Plan 02 decision 1) in `GatherMeshes` order, and the opaque deferred g-buffer is
   draw-order-independent, so every pass draws the same meshes to the same pixels — `smoke_golden`
   does not move and planset-21's draw-count fixture asserts the same counts.

> **Dangling-`Parent` crash, closed by Plan 01.** `Rebuild` reaches the scene through `GatherMeshes`,
> whose `WorldMatrix` walk fatally asserts on a `Parent` pointing at a destroyed entity
> ([Transforms.cpp:29](../../engine/src/Scene/Transforms.cpp)). Plan 01 makes `DestroyEntity`
> recursive (a destroyed parent takes its subtree), so a child can no longer outlive its parent and
> the walk never meets a dangling up-link from a normal destroy — the assert stays as a genuine
> misuse detector. The broadphase needs no special orphan handling.

## Files

| File | Change |
|---|---|
| `engine/include/Veng/Scene/SceneBroadphase.h` (new) | The `SceneBroadphase` class. |
| `engine/src/Scene/SceneBroadphase.cpp` (new) | `Sync` (version gate + residency poll), `Rebuild` (gather + build + pending refresh), `Cull`. |
| `engine/CMakeLists.txt` | Add `src/Scene/SceneBroadphase.cpp`. |
| `engine/include/Veng/Renderer/SceneRenderer.h` | `SceneBroadphase m_Broadphase`; `DidBroadphaseRebuildLastFrame()`; re-point `GetLastVisibleCount()`; `SceneView::Broadphase`. |
| `engine/src/Renderer/SceneRenderer.cpp` | `Sync` in `Execute`; g-buffer pass queries the tree; pass-owned `m_CullScratch`. |
| `engine/src/Renderer/ShadowScenePass.cpp` | Each cascade queries the tree; pass-owned `m_CullScratch`. |
| `tests/unit/scene_broadphase.cpp` (new) + the unit suite source list | **Device-free** version-gate / rebuild / cull-equivalence integration test. |
| `tests/gpu/scene_renderer.cpp` (extend the planset-21 cull fixture) + the `gpu` suite list | Cull-equivalence + rebuild-over-frames assertions in the real path. |

## Verification

- Clean build; the renderer compiles against the `const` `View` overload (Plan 01) and the BVH
  (Plan 02).
- **`tests/unit/scene_broadphase.cpp`** (device-free, no ICD — `Sync`/`GetCandidates`/`Cull` are all
  CPU; meshes are runtime `Primitives` or stubbed bounds). This is the integration guard the golden
  cannot provide:
  - **Cull equivalence:** after a `Sync`, for many frustums, `Cull` returns exactly the set a linear
    `Intersects` scan of `GetCandidates()` returns — and in ascending index (gather) order.
  - **Version gate:** `Sync` an unmutated scene twice → the second reports `DidRebuildLastSync()
    == false` and `scene.GetSpatialVersion()` is **identical before and after** each `Sync` (the
    broadphase reads the scene `const`-only, so `Sync` itself never bumps the version).
  - **Mutation rebuilds and is correct:** move/add/remove/reparent/destroy an entity → the next
    `Sync` reports `true`, and `Cull` equals a linear scan and equals a second `SceneBroadphase` that
    full-rebuilt that frame (the rebuild-converges check across create/destroy/reparent orderings).
  - **Residency:** a candidate whose mesh becomes resident between two frames (no spatial mutation)
    enters `GetCandidates()`/the tree on the following `Sync`, matching the old per-frame gather.
- **`smoke_golden` byte-identical** — re-render the headless capture and confirm it is unchanged
  (fixed pose; the tree query must not move a pixel). No golden regeneration.
- **The planset-21 draw-count test is unchanged** — same visible/drawn counts with culling on and off.
- **`gpu`-band assertions** (`SKIP_RETURN_CODE 77`, skips with no ICD): with `FrustumCull` on, the
  camera pass's drawn count equals a linear `Intersects` scan of `view.Visible` (the tree culls
  identically through the renderer); render a fixed scene twice with no mutation between →
  `DidBroadphaseRebuildLastFrame()` is `false` on frame 2, same drawn count; a `Configure` between
  two `Execute`s still reports `false` (decision 3).
- **Validation gate clean** under `VE_DEBUG` (`ctest --test-dir build-debug -L validation`): the cull
  source changed, the barrier/layout schedule did not — pin that no MoltenVK validation error
  appears, including on a no-rebuild frame that issues the same draws.
- Smoke PPM correct size + exit 0 through the launcher.
