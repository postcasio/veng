# Plan 01 — mesh-source unification: primitives load as meshes

**Goal:** retire the special-cased two-pass `Primitive` resolve. Make a mesh reference's **source**
either a cooked `AssetId` or an **inline procedural recipe**, both resolved to a pending
`AssetHandle<Mesh>` through the ordinary async load path during the prefab populate pass. Remove the
`Primitive` component, the `SpawnResolve`/`VE_RESOLVE` machinery, and `Prefab::SpawnInto`'s second
pass. Foundational; **Plan 02 depends on it.**

## Why this is the change

Today a procedural mesh is plumbed unlike a cooked one. A `Primitive { Shape }` component
([`Components.h`](../../engine/include/Veng/Scene/Components.h)) carries the recipe; a
`SpawnResolve` thunk ([`ResolvePrimitive`](../../engine/src/Scene/Resolve.cpp)) builds `MeshData`,
calls `AssetManager::Build<Mesh>` — which **already** returns a pending async handle — and writes it
into a **separate** `MeshRenderer.Mesh`. The thunk fires from a distinct post-populate pass (2b) in
[`Prefab::SpawnInto`](../../engine/src/Asset/Prefab.cpp) and is re-fired by `ResolveComponents`
([`Resolve.h`](../../engine/include/Veng/Scene/Resolve.h)) from every editor mutation path.

The async build is not the problem — it is already load-shaped. The cost is the plumbing around it:
recipe and handle in two components, a second spawn pass, the standing "every editor path must call
`ResolveComponents`" hazard, and a residency model that does not compose (a primitive spawned by a
Sim system at `OnStart` is produced in a side pass and escapes any pre-spawn residency wait — exactly
the footgun a primitive *player* would hit in the headless smoke capture).

The three major engines: Unity ships **baked shared mesh assets** for standard primitives; Unreal
splits **baked StaticMesh** from a **component-owned DynamicMesh**; **Godot** makes the parametric
primitive a **subclass of `Mesh`** in the single `MeshInstance.mesh` slot, recipe on the resource,
regenerated in place. Godot is "primitives load as meshes" literally. veng adopts it: one mesh slot,
whose source can be a recipe.

## What lands

- **A mesh source sum type.** A mesh reference's serialized source becomes `cooked AssetId` **or**
  `inline recipe` (the existing `PrimitiveShapeVariant`). The runtime field stays a single
  `AssetHandle<Mesh>` on `MeshRenderer` — the renderer query (`Transform, MeshRenderer`) and every
  draw path are unchanged. The *source* is what gains the alternative, not the handle.

- **Recipe resolution folds into the populate pass.** During `SpawnInto`'s per-component populate
  (the same loop that rehydrates cooked `AssetHandle` fields,
  [`Prefab.cpp`](../../engine/src/Asset/Prefab.cpp)), a recipe source is built via
  `AssetManager::Build<Mesh>` right there, yielding a pending handle identical in kind to a cooked
  async load. The build reads only its own recipe (no sibling entities), so it has no reason to wait
  for a second pass.

- **Pass 2b and `SpawnResolve` are removed.** `Primitive` is the only `SpawnResolve` rider, so with
  its build folded into populate, the post-populate resolver pass in `SpawnInto`, the
  `TypeInfo::SpawnResolve` thunk slot ([`TypeRegistry.h`](../../engine/include/Veng/Reflection/TypeRegistry.h)),
  the `VE_RESOLVE` macro + `VengResolver<T>` trait, and the public `ResolveComponents`
  ([`Resolve.{h,cpp}`](../../engine/src/Scene/Resolve.cpp)) all go away. `BuildShapeMeshData` /
  `BuildPrimitiveMesh` survive as the recipe→`MeshData`→handle helper the populate pass calls.

- **The dependency walk recurses the recipe.** The prefab loader's embedded-handle collection
  ([`CollectHandleDeps`](../../engine/src/Asset/Loaders/PrefabLoader.cpp)) already recurses structs
  and variants; a recipe's embedded `AssetHandle<Material>` continues to load as an ordinary
  dependency through the same walk, whether the source is cooked or a recipe.

- **Serialization + inspector.** The recipe persists inline in the prefab as a mesh source (a
  `Variant` of the cooked-id alternative and the shape alternatives), through the existing reflection
  serializer ([`Serialize.cpp`](../../engine/src/Reflection/Serialize.cpp)). The editor inspector
  edits it through the existing `Variant`/`AssetHandle` field widgets — selecting cooked-vs-shape and
  the per-shape parameters. An edit re-loads the derived mesh (re-`Build`), the same shape as
  repointing a cooked asset field; the editor triggers it on the inspector changed-bool, replacing the
  bespoke `ResolveComponents` call.

- **The editor resolve seam.** The editor's `ResolveComponents` calls funnel through one helper —
  `PrefabEditContext::ResolveEntity`
  ([`PrefabEditContext.h`](../../editor/src/panels/PrefabEditContext.h)) — which the panels call after
  add-component and field-edit (the inspector changed-bool above). Two of the three triggers collapse
  into "the mesh source re-resolves like any asset field." The **duplicate** path is different and
  must be handled explicitly: `PrefabExplorerPanel::DuplicateSubtree`
  ([`PrefabExplorerPanel.cpp`](../../editor/src/panels/PrefabExplorerPanel.cpp)) copies a component's
  raw bytes through `ReadFields`, with **no inspector edit and no changed-bool to hook**. Under the new
  model a byte copy yields a `MeshRenderer` whose source is set but whose `AssetHandle<Mesh>` was never
  built, so the duplicate path must rebuild the derived mesh from the copied source (the surviving
  `BuildShapeMeshData`/`Build<Mesh>` helper) where it currently calls `ResolveEntity`.

- **Sample migration.** The sample prefabs that author `Primitive` components
  ([`scene.prefab.json`](../../examples/hello-triangle/assets/prefabs/scene.prefab.json)) move to the
  mesh-source recipe form. Behavior is identical — the same shapes stream in the same few frames late.

- **Doc migration.** Retiring `SpawnResolve`/`VE_RESOLVE`/`ResolveComponents`/`Primitive` removes
  surface that both module guides document as architecture, so they migrate in the same pass:
  `engine/CLAUDE.md` (the spawn-resolve-thunk / `VE_RESOLVE` / post-populate-pass prose) is rewritten
  to the mesh-source model, and `editor/CLAUDE.md` (the "any editor path that adds or edits a
  resolver-bearing component must call `ResolveComponents`" contract and the `Primitive` inspector
  example) is rewritten to "a mesh source re-resolves like any asset field, with the duplicate path
  rebuilding from the copied source."

- **Future-area writeup.** Finalize [`future/dynamic-meshes.md`](../future/dynamic-meshes.md) (area 16,
  already drafted and registered in [`future/README.md`](../future/README.md)): the mutable-`Mesh`
  substrate (sculpting, voxels, CSG/destruction, gameplay-generated geometry) the recipe model
  deliberately does **not** cover, to design against a real consumer later. Verify it reads correctly
  against the model this plan lands rather than re-authoring it.

## Decisions

1. **Recipe-as-source, not component-owned mesh.** veng takes Godot's model (the recipe is the mesh's
   source, in the one mesh slot), not Unreal's DynamicMeshComponent (a component owning a mutable
   buffer). The recipe is declarative and re-derivable; mutable geometry is a different capability,
   deferred to area 16. The single-slot model keeps that future open: a DynamicMesh would produce a
   `Mesh` into the same slot.

2. **Handle home stays `MeshRenderer.Mesh`.** The derived handle lands in the existing renderer slot,
   so the visibility/draw query is untouched; only the *source* of that handle gained an alternative.
   No new render component, no dual-pool query.

3. **Resolution moves to populate, not a synthetic asset cache.** A recipe is built per spawn through
   `Build<Mesh>` (no recipe-hash dedup), matching today's behavior (identical recipes build
   independent meshes; a caller wanting N entities on one mesh shares one handle). A recipe-keyed
   asset cache is a possible later optimization, not part of this unification.

4. **`SpawnResolve` is retired wholesale.** It has exactly one rider; keeping the generic seam for a
   hypothetical future resolver (a mesh-baking spline, a buffer-allocating emitter) is speculative
   generality. When such a consumer arrives it is reconsidered then — likely as its own
   source/load path, consistent with this model — rather than maintained empty now.

## Verification

Clean build, full `ctest`, `smoke_golden` and `validation_gate` green. The sample renders identically.
The editor opens the migrated prefab, edits a shape's parameters, and sees the mesh rebuild — with no
`ResolveComponents` call anywhere in the tree.

The test suites built on the retired surface do not compile against this change and are migrated in
the same pass — they are not covered by "the prefab/spawn suites" today:

- `tests/unit/spawn_resolve.cpp` and `tests/gpu/spawn_resolve.cpp` test the `SpawnResolve` seam itself
  (`HasSpawnResolver<T>`, the `TypeInfo::SpawnResolve` thunk slot, the post-populate resolver pass).
  The seam is gone, so they are **deleted**.
- `tests/unit/primitive_resolve.cpp` and `tests/gpu/primitive_resolve.cpp` test the `Primitive` →
  `ResolveComponents` → `MeshRenderer.Mesh` streaming path. They are **rewritten** to exercise the
  mesh-source recipe round-trip (a recipe source resolving to a pending handle through the populate
  pass), preserving the streaming-resolution coverage under the new model.
- Every other `Primitive`-authoring test (the gpu scene/viewport suites) moves to the mesh-source
  recipe form, the same migration the sample prefabs take.
