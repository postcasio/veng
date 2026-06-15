# Plan 04 — `Prefab` asset loader + `SpawnInto` + sample migration

**Goal:** load the cooked prefab at runtime through the **same asset path as every
other asset** — a registered `AssetLoader` producing a cached `AssetHandle<Prefab>`
— and **spawn its entities into a live `Scene`**. Migrate hello-triangle to a
**fully data-driven** world — a cooked sphere mesh + a cooked prefab, **nothing
built in code** (planset-10 plan 04 still built the world and drew a runtime
primitive). Adds the `Prefab` loader, the `SpawnInto` API (`Entity`-
reference remap + embedded `AssetHandle` rehydration), and the
`add_asset_pack MODULE` / `veng_add_game` **build-order edge** (decision 9). The one
GPU-touching plan — validation gate + smoke.

## Why this is its own plan, and on the main thread

It defines two runtime contracts the editor and any future consumer build on: how a
prefab loads (as a normal cached asset — **uniform**, no bespoke path) and how its
contents land in a world (`SpawnInto`). A `Scene` is an engine primitive that is
never "loaded" — it is constructed empty and **populated** by spawning prefabs into
it. The plan also wires the build graph's `lib → cook → bundle` edge. The sample
migration proves the whole planset end-to-end through the real shipping path
(launcher → cooked pack → loaded prefab → spawned scene → draw).

## The asset shape — load like everything else, spawn into a `Scene`

A cooked prefab's payload is immutable and shareable (an entity/component value tree
+ its embedded mesh/material references), so it loads through the **identical path
as textures/meshes/materials** (decision 8): a registered `AssetLoader` for
`AssetType::Prefab` yields a cached, refcounted **`Prefab`**. The runtime `Scene`
stays `Unique`/mutable (planset-10 decision 6) and is **never loaded** — you create
one and **spawn the prefab's entities into it**. No `AssetManager::LoadScene`, no
cache bypass.

```cpp
// The cached, immutable cooked-prefab asset: a serialized recipe of entities +
// components + values + references. Holds the decoded value tree + the AssetHandles
// its components reference (resolved as load-time dependencies, like a Material's
// textures). CPU-only Ref<T>; no GPU resource.
class Prefab
{
public:
    // Spawn this prefab's entities/components into `scene`. Returns the spawned
    // root entities (those with no in-prefab Parent), in authoring order. Remaps
    // intra-prefab Entity references to the freshly-created handles; rehydrates
    // AssetHandle fields via `manager`. Spawning twice spawns twice — a prefab is a
    // reusable recipe, not a singleton. A fresh world is just spawning into a
    // newly-created empty Scene.
    vector<Entity> SpawnInto(Scene& scene, AssetManager& manager) const;
};
```

`SpawnInto` lives on `Prefab`, not `Scene`: the dependency points **asset → engine
primitive** (a prefab knows about `Scene`/`AssetManager`), so the `Scene` primitive
from planset-10 stays free of any asset-system dependency.

### The loader — a registered `AssetLoader`, nothing special

`PrefabLoader : AssetLoader`, `Type() == AssetType::Prefab`, registered in the
`AssetManager` loader table beside the others. Its `Load` returns the standard
`Detail::LoadJob`:

- **Resource:** a `Ref<Prefab>` holding the decoded value tree (entities →
  components → name-keyed records, straight from the cooked blob — no per-component
  deserialization yet; that happens at `SpawnInto`).
- **Dependencies:** every embedded `AssetHandle` field's `AssetId`, surfaced as
  `LoadJob::Dependencies` so the **existing** dependency machinery (the one a
  `Material` uses for its textures/shaders) loads + finalizes them first. A
  `MissingDependency` is the structured `AssetLoadError` it already is — no new error
  path. Async vs. sync follows the manager's usual policy.
- **Finalize:** null (no bindless registration, no pipeline) — a `Prefab` is CPU
  data.

So a prefab is, verbatim, "an asset with dependencies" — the same statement true of
a material. The `TypeRegistry` the loader needs (to know which fields are
`AssetHandle`-class, for dependency extraction) is threaded into the loader the way
the GPU loaders receive `Context&` — `Application` holds both and supplies them.

### `SpawnInto` — populating a `Scene` from the prefab

`prefab.SpawnInto(scene, manager)` does the per-component work against an
already-resident prefab (its dependencies finalized at load):

1. Create the prefab's entities in `scene`, recording the prefab-index → new-`Entity`
   map; track which are roots (no in-prefab `Parent`) for the return value.
2. Per component: resolve its `TypeId` in the registry (**unknown** → fatal — the
   cook validated against descriptors, so at runtime this is a registry/module
   mismatch, a `VE_ASSERT`, not a recoverable case), `Add` it by `TypeId` (type-
   erased, default-constructed), and `ReadFields(records, slot, typeInfo, registry)`
   to populate it.
3. **Remap references:** rewrite every `Reference` (`Entity`) field from the cooked
   `{Index,Generation}` to the new handle via the index map (a prefab-internal
   reference resolves to the spawned entity; the prefab cannot reference entities
   that already existed in the scene).
4. **Rehydrate `AssetHandle` fields:** the cooked value is an `AssetId`; resolve it
   to the prefab's already-resident handle through `manager` (cheap — loaded as a
   dependency at load time, exactly the rehydration planset-10's `ReadFields` left to
   "the deferred loader").

The registry `SpawnInto` walks against is the one the `Scene` was created with
(planset-10's `Scene::Create(TypeRegistry&)`), reached through `scene`.

## The build-order edge — `cmake/`

Prefab cooking needs the module loaded (plan 02's `--module`), so a pack with
prefabs must cook **after** `libgame` builds (decision 9):

- **`add_asset_pack(... MODULE <lib-target>)`** — optional. When set, the cook
  command gains `--module $<TARGET_FILE:<lib-target>>` and the custom command
  `DEPENDS` on the lib, so the build graph adds `lib → cook`. Packs with no `MODULE`
  are unchanged — independent of any lib.
- **`veng_add_game`** wires it: if the game's `ASSET_PACK` contains prefabs, that
  pack target is declared with `MODULE <name>` (the `lib<name>` target), giving
  `lib<name> → cook pack → copy beside launcher`. The relocatable trio is preserved.
- **The chicken-and-egg is fine:** the lib carries the game *code*; the pack is
  cooked *from* it by reflection. `lib → cook → bundle` is a clean DAG; no cycle.

## The sample — `examples/hello-triangle/`

planset-10 plan 04 builds the world in `OnInitialize` (one entity: `Transform` +
`MeshRenderer` + `Spinner`) and draws a **runtime primitive** sphere. The migration
is **fully data-driven** — nothing visible is constructed in code, so the path is
genuinely exercised end to end:

- Cook a **sphere mesh asset** into the hello-triangle pack (a `*.mesh.json` source;
  the existing brick `Material` becomes the mesh's material via planset-7's
  mesh-owns-materials model, resolved by id at cook). The runtime `Primitives::Sphere`
  call is **removed** from the sample — the geometry now comes from a cooked asset.
- Author `assets/sphere.prefab.json` — one entity with `Transform`, `MeshRenderer`
  (`Mesh` = the cooked sphere mesh's `AssetId`), and the game `Spinner` — and add it
  to the pack manifest as a `prefab`-type entry with a minted `AssetId`.
- The pack is declared with `MODULE hello_triangle` so it cooks after the lib.
- `OnInitialize` constructs **only** the empty world, then loads + spawns the prefab:
  `m_Scene = Scene::Create(GetTypeRegistry());` `auto prefab =
  GetAssetManager().LoadSync<Prefab>(<prefab id>);`
  `prefab->SpawnInto(*m_Scene, GetAssetManager());`. It builds no mesh, no material,
  no entity by hand — the prefab's `MeshRenderer` carries the mesh id, the mesh owns
  its material, and `SpawnInto` rehydrates both through the `AssetManager`. The
  `Spinner` is driven over a query exactly as before; only the world's *source*
  changed (a cooked prefab + cooked mesh, not code).

Mint the real `AssetId` (and any new `TypeId`s) with `vengc generate-id` /
`generate-type-id` once the build is green, replacing placeholders (working norm).

## Verification

- Clean build (`-j 2`); `ctest` green across `unit`/`death`/`cooker`/`gpu`.
- **Validation gate:** `cmake -B build-debug -DVE_DEBUG=ON` then `ctest --test-dir
  build-debug -L validation` — the render path is materially unchanged, so the
  allowlist stays empty (it may not be widened).
- **Smoke:** the launcher writes a correct-sized 1280×720 RGB PPM (≈ 2,764,816
  bytes) and exits 0 — through the full `dlopen` → load prefab → spawn → draw chain.
- **`smoke_golden`:** the pose is fixed, but the **geometry source changes** — a
  cooked sphere mesh replaces `Primitives::Sphere`, whose tessellation/normals
  differ — so the capture **will** move and the golden **must** be regenerated per
  `CLAUDE.md`. This is a deliberate, expected change (call it out in the commit), not
  a regression; confirm the new capture is a sensible lit sphere before blessing it.

## Acceptance

Clean build; full `ctest` green; validation gate clean (allowlist empty); smoke PPM
correct-sized and exit 0; `smoke_golden` green (regenerated only if a deliberate
shift is explained). Commit: `Plan 04: Prefab asset loader + SpawnInto + cooked-
prefab sample — uniform Load path, reference remap, add_asset_pack MODULE edge`.
