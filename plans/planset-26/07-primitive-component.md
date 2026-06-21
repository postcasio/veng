# Plan 07 — `PrimitiveComponent` + resolution

**Goal:** the join. A `PrimitiveComponent` whose `Variant<…> Shape` is the persistable
recipe, per-shape parameter structs mirroring the `Primitives::` generators, and a
`ResolvePrimitiveMeshes(Scene&, AssetManager&, PrimitiveMeshCache&)` that generates the
active shape into a real mesh through plan 05's async factory + plan 06's pending handle,
dedups identical shapes through a caller-owned `PrimitiveMeshCache`, and stores the handle
in the entity's `MeshRenderer.Mesh`. Depends on plans 01 + 02 (variant data model) and
05 + 06 (async streaming). The editor's variant widget (plan 04) already gives the
selection UX.

## Why this is its own plan

It is where the two arcs meet and where the one component-specific decision lives — how a
recipe becomes a renderable without leaking generation logic into the prefab spawner. It
ships no new mechanism, only wiring the prior six plans into a usable component.

## Per-shape parameter structs — `engine/include/Veng/Scene/Components.h`

One reflected struct per `Primitives::` generator, each carrying only that shape's
parameters plus its material — no dead fields:

```cpp
struct CubeShape      { f32 Extent = 1.0f;  AssetHandle<Material> Material; };
struct PlaneShape     { vec2 Size = vec2(1.0f); uvec2 Subdivisions = uvec2(1); AssetHandle<Material> Material; };
struct SphereShape    { f32 Radius = 0.5f;  u32 Rings = 16; u32 Segments = 32; AssetHandle<Material> Material; };
struct IcosphereShape { f32 Radius = 0.5f;  u32 Subdivisions = 3; AssetHandle<Material> Material; };

using PrimitiveShapeVariant = Variant<CubeShape, PlaneShape, SphereShape, IcosphereShape>;

/// @brief A procedural-mesh recipe: regenerated into the entity's MeshRenderer at spawn.
struct PrimitiveComponent { PrimitiveShapeVariant Shape; };
```

Each shape gets a `VE_REFLECT` block (params with `.Min`/`.DisplayName` metadata matching
the generators' valid ranges — e.g. `Rings`/`Segments` min 3, `Subdivisions` min 1). The
variant gets a `VE_VARIANT`; `PrimitiveComponent` gets a `VE_REFLECT` with one
`VE_FIELD(Shape)`. Mint real `TypeId`s with `vengc generate-type-id` once the build is
green; placeholders until then.

`MeshRenderer` is **unchanged**.

## Builtin registration — `BuiltinTypes.h` / `Scene/BuiltinTypes.cpp`

`RegisterBuiltinTypes` registers `PrimitiveComponent`; `Register<PrimitiveComponent>()`
transitively registers the variant and its four alternatives (plan 01's dependency
recursion), so one line suffices. This stays GPU-free (the registration contract) — the
shapes are plain reflected structs.

## Generation — shape → `MeshData`

A small dispatch maps the active alternative to its `Primitives::` call:

```cpp
// Build CPU geometry for whichever shape is active. Empty variant → no mesh.
optional<MeshData> BuildShapeMeshData(const PrimitiveShapeVariant& shape);
```

It reads the active alternative through the variant ops and calls the matching generator
(`Primitives::Cube(extent, material)`, etc.). Pure CPU; no Context. Lives beside the
generators (`Veng/Asset/Primitives.h` or a new `Scene/PrimitiveResolve.h`).

## The dedup cache — `PrimitiveMeshCache`

A small caller-owned cache gives the "N cubes share one GPU mesh" dedup that `CreateAsync`
deliberately omits. It is a plain strong-handle map, touched only on the render thread — no
`WeakRef`, no interaction with `CollectGarbage`'s eviction:

```cpp
/// @brief A shape key: the active alternative's TypeId, a hash of its parameter bytes, and
///        its material id — equal keys denote an identical procedural mesh.
struct ShapeKey { TypeId Kind; u64 ParamHash; AssetId Material; /* ==, std::hash */ };

/// @brief Maps a ShapeKey to the (pending or resident) mesh handle built for it, so
///        identical shapes share one upload and one GPU mesh. Render-thread only; owned by
///        the resolution caller (the app / the prefab-editor document) for its scene's life.
struct PrimitiveMeshCache { unordered_map<ShapeKey, AssetHandle<Mesh>> Entries; };
```

`ShapeKey` is computed generically from the variant's active member: `Kind` is
`VariantActiveType`, `ParamHash` hashes the active member's bytes (size from its `TypeInfo`),
and `Material` is the alternative's `AssetHandle<Material>` id.

## Resolution — `ResolvePrimitiveMeshes(Scene&, AssetManager&, PrimitiveMeshCache&)`

```cpp
/// @brief Generates and streams in a Mesh for each PrimitiveComponent whose MeshRenderer
///        does not already hold the mesh for its current shape, storing the (pending)
///        handle on the entity's MeshRenderer. Adds a MeshRenderer when the entity has none.
///
/// Idempotent: an entity already pointing at its current shape's cached mesh is skipped, so
/// the app calls it after Prefab::SpawnInto and the prefab editor calls it every frame.
void ResolvePrimitiveMeshes(Scene& scene, AssetManager& manager, PrimitiveMeshCache& cache);
```

Per `(PrimitiveComponent)` entity:

1. If the variant is empty, skip (no mesh).
2. Compute the current `ShapeKey`. If the entity has a `MeshRenderer` whose `Mesh` is the
   **same handle** as `cache.Entries[key]` (handle identity — same cache entry), skip:
   already resolved to the current shape. (After an editor edit the key changes, so the
   stored handle no longer matches `cache.Entries[newKey]` and the entity re-resolves.)
3. Otherwise get-or-create the cached handle: on a cache miss build the `MeshData`
   (`BuildShapeMeshData`) and `cache.Entries[key] = manager.CreateAsync(Mesh::CreateAsync(
   context, tasks, std::move(data), name))` (`AssetManager` holds the `Context&` /
   `TaskSystem&`). Store that handle in the entity's `MeshRenderer.Mesh` (adding a
   `MeshRenderer` if absent).
4. After the scan, **prune** `cache.Entries` of any key no entity's `MeshRenderer.Mesh`
   references, so a parameter dragged across many values does not accumulate resident
   meshes — dropping the cache's handle retires the mesh through the normal per-frame path.

The pending handle is `!IsLoaded()` until the build lands; `GatherMeshes` gathers only
*resident* `(Transform, MeshRenderer)` meshes (it skips a handle whose `IsLoaded()` is
false), so the entity renders nothing for a few frames and then appears — no stall.
Identical shapes across entities share one cache entry, so a field of identical cubes
uploads once.

### Where it runs

`Prefab::SpawnInto` stays generic and component-agnostic — it does **not** call this. Each
caller holds one `PrimitiveMeshCache` beside its `Scene` and invokes `ResolvePrimitiveMeshes`
explicitly:

- the app, once after its `SpawnInto` (plan 08 migrates hello-triangle to this);
- the prefab-editor document, **every frame** (the call is idempotent — only an entity
  whose shape key changed re-resolves), so an inspector edit to a shape or parameter is
  picked up on the next frame. The `InspectorPanel` already holds the `AssetManager&` and
  the document's `Scene`, so no changed-signal plumbing is needed.

## Editor selection

Plan 04's variant widget already renders `PrimitiveComponent.Shape` as a shape combo +
the active shape's parameters + its material picker. No new editor code is required here
beyond the prefab-editor document's per-frame `ResolvePrimitiveMeshes` call (above). A
bespoke merged "mesh source" control (cooked-vs-primitive in one widget) is explicitly out
of scope.

## Tests

- **Unit (CPU).** `BuildShapeMeshData` over each active alternative returns the same
  geometry the corresponding `Primitives::` call does (vertex/index counts); an empty
  variant returns `nullopt`.
- **GPU.** Spawn a scene with a `PrimitiveComponent`, run `ResolvePrimitiveMeshes(scene,
  manager, cache)`, pump until resident, assert the `MeshRenderer.Mesh` is loaded with the
  expected index count. Two entities with identical shapes share one underlying `Ref<Mesh>`
  (dedup via the `PrimitiveMeshCache`); a second resolve scan is a no-op (idempotence); and
  changing an entity's shape then re-resolving swaps its handle and prunes the orphaned key.
- **Round-trip (with plan 02).** A `PrimitiveComponent` serialized via `WriteFields` and
  read back yields the same active shape + params; after resolution both produce an
  equivalent mesh.

## Acceptance

- Clean build; `ctest` green (unit + the new GPU case; GPU skips with no device).
- A `PrimitiveComponent` persists its shape recipe through reflection; resolution streams
  in the mesh and fills `MeshRenderer.Mesh` with no render-thread stall; identical shapes
  dedup to one mesh through the `PrimitiveMeshCache`, and the cache is render-thread-only
  (no `WeakRef`/GC interaction).
- `Prefab::SpawnInto` carries no primitive-specific logic; resolution is an explicit,
  idempotent entry point.
- `VE_DEBUG` validation gate clean.
