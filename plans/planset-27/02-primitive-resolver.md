# Plan 02 — `CreatePrimitiveMesh` + the primitive resolver; delete the cache

**Goal:** make `PrimitiveComponent` the first rider on plan 01's seam, and delete
the planset-26 resolution model in the same pass. Add
`CreatePrimitiveMesh(AssetManager&, const PrimitiveShapeVariant&) →
AssetHandle<Mesh>`, register a `SpawnResolve` on `PrimitiveComponent` that builds the
active shape and assigns the entity's `MeshRenderer.Mesh`, and remove
`PrimitiveMeshCache`, `ShapeKey`, `ResolvePrimitiveMeshes`, and the prune. Depends on
plan 01.

This plan uses the **pre-rename** component name `PrimitiveComponent`; plan 05 renames
it to `Primitive` last, after the seam work has settled.

## Why this is its own plan

It is where the generic seam meets the one concrete consumer, and where the
planset-26 cache/scan/prune is removed. Keeping the add (the resolver) and the delete
(the cache) in one commit means the tree never carries both resolution models at
once.

## `CreatePrimitiveMesh` — the concrete builder

Lift the build body currently inline in `ResolvePrimitiveMeshes`
([`PrimitiveResolve.cpp`](../../engine/src/Scene/PrimitiveResolve.cpp) lines ~168–175)
into a free function beside `BuildShapeMeshData`:

```cpp
/// @brief Builds the active shape into a streaming Mesh and returns its pending handle.
///
/// Generates the CPU geometry (BuildShapeMeshData) and uploads it through the async
/// Mesh::CreateAsync path; the handle is !IsLoaded() until the build lands a few
/// frames later, exactly as a cooked-mesh Load. Returns an empty handle for an empty
/// variant. Does not dedup — identical recipes build independent meshes; share the
/// returned handle to reuse one.
[[nodiscard]] AssetHandle<Mesh> CreatePrimitiveMesh(AssetManager& manager,
                                                    const PrimitiveShapeVariant& shape);
```

Body is the existing three steps, unchanged: `BuildShapeMeshData(shape)` →
`Mesh::CreateAsync(manager.GetContext(), manager.GetTasks(), std::move(*data), name)`
→ `manager.CreateAsync<Mesh>(...)`. It is a **free function**, not an `AssetManager`
method, so the manager stays primitive-agnostic (decision 5). `BuildShapeMeshData` is
unchanged and stays public for tests/tools.

## The `PrimitiveComponent` resolver

A **typed** resolver function — declared beside `PrimitiveComponent`, defined in the
primitive `.cpp` — over `CreatePrimitiveMesh`. It takes `PrimitiveComponent&`, not
`void*`; `VE_RESOLVE` (plan 01) generates the erasing trampoline:

```cpp
// Build the active shape's mesh and point the entity's MeshRenderer at it,
// adding a MeshRenderer when the entity has none. An empty variant leaves the
// renderer untouched (nothing to render yet).
void ResolvePrimitiveComponent(PrimitiveComponent& primitive, Scene& scene, Entity entity,
                               AssetManager& manager)
{
    AssetHandle<Mesh> mesh = CreatePrimitiveMesh(manager, primitive.Shape);
    if (!mesh.Id().IsValid())
    {
        return; // empty variant — empty handle (Id().Value == 0)
    }
    if (scene.TryGet<MeshRenderer>(entity) == nullptr)
    {
        scene.Add<MeshRenderer>(entity);
    }
    scene.Get<MeshRenderer>(entity).Mesh = std::move(mesh);
}
```

Registered on the type with plan 01's seam — one line beside its existing
`VE_REFLECT`:

```cpp
VE_RESOLVE(PrimitiveComponent, ResolvePrimitiveComponent);
```

`Register<PrimitiveComponent>()` (already called by `RegisterBuiltinTypes`) now wires
`SpawnResolve` through the `if constexpr` branch — no registration-site change.
Registration stays GPU-free: `VE_RESOLVE` records a function *pointer*; nothing runs
at registration.

## Delete the planset-26 resolution model

Remove, in this same pass:

- `PrimitiveMeshCache`, `ShapeKey`, and `template<> struct std::hash<ShapeKey>` from
  `Veng/Scene/PrimitiveResolve.h`.
- `ResolvePrimitiveMeshes(Scene&, AssetManager&, PrimitiveMeshCache&)` and its body —
  the scene scan, the handle-identity skip, and the orphan-prune — from
  `PrimitiveResolve.cpp`. Its build body survives as `CreatePrimitiveMesh`; its
  per-entity assign survives as the resolver.
- The `ShapeKey` hashing helpers (`HashBytes`/`ShapeParamHash`/`ShapeMaterial`/`KeyFor`)
  that only the cache used.

`PrimitiveResolve.h` is renamed to `Resolve.h` (decided in plan 01 / README
decision 4): it now hosts the generic `ResolveComponents`, plus `CreatePrimitiveMesh`
and `BuildShapeMeshData`. The generic, primitive-agnostic `ResolveComponents` must
not live in a primitive-named header, so this rename is not optional.

## Tests — migrate `tests/gpu/primitive_resolve.cpp`

The existing GPU test drives the deleted `ResolvePrimitiveMeshes(scene, manager,
cache)`. Re-point it at the new path:

- **Spawn resolves.** Build a prefab carrying a `PrimitiveComponent` (or `Add` one to
  a live entity and call `ResolveComponents`), pump the task system until resident,
  assert the entity's `MeshRenderer.Mesh` is loaded with the expected index count for
  that shape.
- **Each entity gets its own mesh (no dedup).** Two entities with identical shapes
  resolve to **distinct** `Ref<Mesh>` (the cache is gone) — both correct, both
  resident. This replaces the old "share one underlying `Ref<Mesh>`" assertion.
- **Re-resolve swaps the handle.** Change an entity's shape, call
  `ResolveComponents`, assert its `MeshRenderer.Mesh` now points at a mesh for the new
  shape and the old handle retires through the normal per-frame path (no leak).
- **Empty variant.** A `PrimitiveComponent` with an empty variant adds no mesh / leaves
  the renderer empty.

The CPU `BuildShapeMeshData` unit test (geometry parity with the `Primitives::` calls)
is unchanged.

## Acceptance

- Clean build; `ctest` green (the migrated GPU case + the unchanged
  `BuildShapeMeshData` unit test; GPU skips with no device).
- A spawned `PrimitiveComponent` streams its mesh in with no caller resolve call;
  `CreatePrimitiveMesh` is the concrete builder; the resolver assigns
  `MeshRenderer.Mesh`.
- `PrimitiveMeshCache`, `ShapeKey`, its `std::hash`, `ResolvePrimitiveMeshes`, and the
  prune are gone; no caller owns a primitive cache.
- `VE_DEBUG` validation gate clean — the async build reuses planset-26's clean
  `Mesh::CreateAsync`; no allowlist change.
