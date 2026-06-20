# Plan 01 — AABB facility + mesh/scene bounds

**Goal:** add the engine's first bounds primitive — an `AABB` type — a **local-space bound
per `Mesh`** computed from its vertices, and a **world-space bound per `Scene`** unioned on
demand. This is the reusable facility; Plan 02's cascade fit consumes both.

## What lands

### The `AABB` type ([engine/include/Veng/Math/AABB.h](../../engine/include/Veng/Math/AABB.h), new)

A glm-only axis-aligned bounding box in a new `Veng/Math/` home (the engine's first math
header). Public, pulling in nothing but `Veng.h`.

```cpp
namespace Veng
{
    struct AABB
    {
        vec3 Min;
        vec3 Max;

        // The empty box: Min > Max on every axis, the identity for Union. Folding
        // over zero points/boxes yields Empty(), not a degenerate box at the origin.
        [[nodiscard]] static AABB Empty();

        [[nodiscard]] bool IsEmpty() const;        // any Min > Max
        [[nodiscard]] vec3 Center() const;          // (Min + Max) * 0.5
        [[nodiscard]] vec3 Extents() const;         // (Max - Min) * 0.5 (half-extents)
        [[nodiscard]] vec3 Size() const;            // Max - Min

        void Expand(vec3 point);                    // grow to include a point
        void Expand(const AABB& other);             // grow to include another box

        // The 8 corners, for transform-and-refit and frustum tests.
        [[nodiscard]] std::array<vec3, 8> Corners() const;

        // A new axis-aligned box bounding this box's 8 corners under `m`. The
        // standard transform-and-refit: an AABB transformed by a rotation is the
        // AABB of the rotated corners, not the rotated AABB.
        [[nodiscard]] AABB Transformed(const mat4& m) const;
    };

    // Union as a free function so it reads at the call site; Empty() is its identity.
    [[nodiscard]] AABB Union(const AABB& a, const AABB& b);
}
```

`Empty()` is `{ vec3(+inf), vec3(-inf) }`, so `Expand`/`Union` are unconditional `min`/`max`
folds with no empty special-case. `Transformed` builds the box from the 8 transformed
corners (not min/max of two transformed extreme corners, wrong under rotation) — exactly
what Plan 02 needs to take a world box into light space and a scene box for near-plane
extension. Non-trivial bodies live in `engine/src/Math/AABB.cpp`; one-liners stay inline.

### `Mesh` carries a local-space bound

- `Mesh` gains `AABB m_Bounds` + `[[nodiscard]] const AABB& GetBounds() const`
  (local/object space).
- `MeshInfo` gains an `AABB Bounds` field.
- The runtime `Mesh::Create(Context&, const MeshData& data, const string& name)` folds
  `data.Vertices`' `Position` into the bound before upload.
- `MeshLoader` computes the bound from the decoded canonical vertices (the `vec3` at offset
  0 of each interleaved vertex) and passes it into `MeshInfo.Bounds`.

A shared helper folds a bound from canonical vertex positions (a span of `CanonicalVertex`,
or raw interleaved bytes + stride — the form `MeshLoader` has), so there is one definition
of "a mesh's bound." `Primitives::Cube`/`Plane`/`Sphere` build `MeshData`, so their bounds
fall out of the runtime `Create` path with no per-primitive code.

The `.vengpack` mesh format ([CookedBlobs.h](../../assetpack/include/Veng/Asset/CookedBlobs.h))
is **unchanged** — no `CookedMeshHeader` field, no version bump; the bound is derived from
the vertices the blob already carries.

### `SceneBounds` — a scene's world-space bound ([engine/include/Veng/Scene/Transforms.h](../../engine/include/Veng/Scene/Transforms.h) + src)

A free function beside `WorldMatrix` / `ComputeWorldMatrices` (the same recompute-on-demand
home):

```cpp
// The world-space AABB bounding every resident (Transform, MeshRenderer) entity's
// mesh — each mesh's local bound transformed by the entity's world matrix and
// unioned. Empty (AABB::Empty()) when the scene has no resident mesh renderers.
// Recomputed on demand, no cached bound (mirrors ComputeWorldMatrices).
[[nodiscard]] AABB SceneBounds(const Scene& scene);
```

It computes world matrices **once** via `ComputeWorldMatrices` (not per-entity `WorldMatrix`,
which re-walks the parent chain), then for each `(Transform, MeshRenderer)` with a **resident**
mesh handle (`IsLoaded()`) folds `mesh->GetBounds().Transformed(worldMatrix)` into the
accumulating box. A non-resident mesh contributes nothing (an async-loading scene bounds to
what is loaded). `SceneBounds` lives in `libveng` and depends only on the scene + asset
surface — no renderer type.

## Decisions

1. **Compute-on-load, not a cooked-format field.** A mesh's bound is derived from its
   vertices; the `.vengpack` and the cooker stay untouched, and cooked/runtime meshes share
   one bounds path. Storing it in the header (a version bump) is the optimization if mesh
   load is ever measured hot — it is not.

2. **Local-space on the mesh, world-space at the consumer.** `GetBounds()` is object-space;
   `Transformed(worldMatrix)` lifts it to world space per instance, so one stored bound
   serves a mesh drawn at N transforms.

3. **`SceneBounds` reuses `ComputeWorldMatrices`, no cache.** One amortized matrix pass, not
   N parent-chain walks. Recompute-on-demand matches the transform contract; a cached,
   dirty-tracked bound (or a BVH) is the scaling answer shared with frustum culling, deferred.

4. **`AABB` is value-type math, not a `Create`-factory resource** — copied freely like
   `vec3`/`mat4`, no ownership rule, not reflected this planset.

## Files

| File | Change |
|---|---|
| `engine/include/Veng/Math/AABB.h` (new) | The `AABB` struct + `Union`. |
| `engine/src/Math/AABB.cpp` (new) | `Corners`/`Transformed`/`Expand` bodies. |
| `engine/include/Veng/Asset/Mesh.h` | `MeshInfo.Bounds`; `Mesh::m_Bounds` + `GetBounds()`; runtime `Create` folds `MeshData.Vertices`. |
| `engine/src/Asset/Mesh.cpp` (where the `Context&` `Create` lives) | Bound fold before upload. |
| `engine/src/Asset/Loaders/MeshLoader.cpp` | Compute the bound from decoded vertices → `MeshInfo.Bounds`. |
| `engine/include/Veng/Scene/Transforms.h` + `engine/src/Scene/Transforms.cpp` | Declare + define `SceneBounds`. |
| `engine/CMakeLists.txt` | Add `src/Math/AABB.cpp`. |
| `tests/unit/bounds.cpp` (new) + the unit suite source list | Device-free `AABB` + bounds tests. |

## Verification

- Clean build; `include_hygiene` compiles `Veng/Math/AABB.h` (glm-only — no backend leak).
- **`tests/unit/bounds.cpp`** (device-free, no ICD):
  - `Empty().IsEmpty()`; `Union(Empty(), b) == b` (empty is the identity).
  - `Expand(point)` / `Union(a, b)` produce the tight min/max; `Center`/`Extents`/`Size` on a
    known box.
  - **`Transformed` refits under rotation** — a unit box rotated 45° yields the larger
    axis-aligned box of the rotated corners, not the original.
  - **Primitive bounds**: `Cube` is `[-0.5, 0.5]³`, `Sphere` matches its radius, `Plane` is
    flat on one axis.
  - **`SceneBounds`**: a `Scene` with two primitives at known transforms returns their union;
    an empty scene returns `AABB::Empty()`.
- `smoke_golden` is **byte-identical** — Plan 01 adds no rendering.
- Full `ctest` green across the unit/death/cooker bands and the `gpu` band where present.
