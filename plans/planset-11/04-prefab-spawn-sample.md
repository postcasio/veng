# Plan 04 — `Prefab` asset loader + `SpawnInto` + sample migration

**Goal:** load the cooked prefab at runtime through the **same asset path as every
other asset** — a registered `AssetLoader` producing a cached `AssetHandle<Prefab>`
— and **spawn its entities into a live `Scene`**. Migrate hello-triangle to a
**data-driven** world — a cooked prefab supplies the entity + its `Transform`,
`MeshRenderer`, and game `Spinner`, with **no entity/component built in code**
(planset-10 plan 04 built that world by hand). The mesh stays a runtime-generated
primitive adopted into an `AssetHandle` — the prefab serializes its renderer's mesh
field as "no asset" (a runtime resource has the invalid `AssetId`), and the app
assigns the adopted handle to the spawned renderer after `SpawnInto`. Adds the
`Prefab` loader, the `SpawnInto` API (`Entity`-reference remap + embedded
`AssetHandle` rehydration), and the `add_asset_pack MODULE` / `veng_add_game`
**build-order edge** (decision 9). The one GPU-touching plan — validation gate + smoke.

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
  deserialization yet; that happens at `SpawnInto`). The loader first checks
  `CookedPrefabHeader.Version == CookedPrefabVersion` and returns a structured
  `AssetLoadError` (not a fatal) on a mismatch — a stale/foreign blob is a recoverable
  load failure, like any malformed asset.
- **Dependencies:** every embedded `AssetHandle` field's `AssetId`, surfaced as
  `LoadJob::Dependencies` so the **existing** dependency machinery (the one a
  `Material` uses for its textures/shaders) loads + finalizes them first. A
  `MissingDependency` is the structured `AssetLoadError` it already is — no new error
  path. Async vs. sync follows the manager's usual policy.
- **Finalize:** null (no bindless registration, no pipeline) — a `Prefab` is CPU
  data.

So a prefab is, verbatim, "an asset with dependencies" — the same statement true of
a material. The `TypeRegistry` the loader needs (to know which fields are
`AssetHandle`-class, for dependency extraction) reaches it through the
**`AssetManager`**: plan 01 threads the host-owned registry into the `AssetManager`
constructor (a `TypeRegistry&` beside its `Context&`/`TaskSystem&`), and the
`AssetManager` supplies it to the loader the same way it already supplies the
`Context`/`TaskSystem` — passed into `AssetLoader::Load(...)` (its signature gains a
`TypeRegistry&`; the existing loaders ignore it). The `PrefabLoader` does not store a
back-reference; it reads the registry from the `Load` call like every other context it
needs.

### `SpawnInto` — populating a `Scene` from the prefab

`prefab.SpawnInto(scene, manager)` does the per-component work against an
already-resident prefab (its dependencies finalized at load):

1. Create the prefab's entities in `scene`, recording the prefab-index → new-`Entity`
   map; track which are roots (no in-prefab `Parent`) for the return value.
2. Per component: resolve its `TypeId` in the registry (**unknown** → fatal — the
   cook validated against descriptors, so at runtime this is a registry/module
   mismatch, a `VE_ASSERT`, not a recoverable case), add a default-constructed slot
   for it **by `TypeId`** (type-erased), and `ReadFields(records, slot, typeInfo,
   registry)` to populate it.
3. **Remap references:** for every `Reference` (`Entity`) field, read the cooked
   `Index` (`Generation` is ignored): the null sentinel `Entity::Null.Index` (`~0u`)
   stays `Entity::Null`; any other index maps through the prefab-index → new-`Entity`
   table to the spawned handle (an out-of-range index is fatal — the cook bounds-checked
   it). A prefab-internal reference resolves to the spawned entity; the prefab cannot
   reference entities that already existed in the scene.
4. **Rehydrate `AssetHandle` fields:** the cooked value is an `AssetId`; resolve it
   to the prefab's already-resident handle through `manager` (cheap — loaded as a
   dependency at load time, exactly the rehydration planset-10's `ReadFields` left to
   "the deferred loader"). An **invalid** `AssetId` (the "no asset" value) stays an
   empty handle — the app may assign a runtime resource to it after spawn.

**Scene API `SpawnInto` needs.** `SpawnInto` is a method of `Prefab`, not `Scene`, so
two `Scene` surfaces it relies on must be **public**: a `[[nodiscard]] TypeRegistry&
GetTypeRegistry() const` accessor (the registry the `Scene` was created with — the one
`SpawnInto` walks descriptors against) and a type-erased **add by `TypeId`** returning
the new component's slot (`Scene::AddRaw(Entity, TypeId)` exists but is private — expose
it, or add a thin public wrapper). Both are small additions to the planset-10 `Scene`;
the templated `Add<T>`/`Get<T>` surface is unchanged.

## The build-order edge — `cmake/`

Prefab cooking needs the module loaded (plan 02's `--module`), so a pack with
prefabs must cook **after** `libgame` builds (decision 9):

- **`add_asset_pack(... MODULE <lib-target>)`** (`cmake/AssetPack.cmake`) — optional.
  When set, the cook command gains `--module $<TARGET_FILE:<lib-target>>` and the
  custom command `DEPENDS` on the lib, so the build graph adds `lib → cook`. Packs
  with no `MODULE` are unchanged — independent of any lib.
- **`veng_add_game`** (`cmake/Game.cmake`) wires it: if the game's `ASSET_PACK` contains prefabs, that
  pack target is declared with `MODULE <name>` (the `lib<name>` target), giving
  `lib<name> → cook pack → copy beside launcher`. The relocatable trio is preserved.
- **The chicken-and-egg is fine:** the lib carries the game *code*; the pack is
  cooked *from* it by reflection. `lib → cook → bundle` is a clean DAG; no cycle.

## The sample — `examples/hello-triangle/`

planset-10 plan 04 builds the world in `OnInitialize` (one entity: `Transform` +
`MeshRenderer` + `Spinner`) **by hand** and draws a runtime primitive (it generates
`Primitives::Icosphere`, uploads it via `Mesh::Create`, and adopts it into an
`AssetHandle` with `GetAssetManager().Adopt(sphere)`). The migration makes the
**entity and its components data** — a cooked prefab — while the **mesh stays a
runtime-adopted primitive**, demonstrating exactly the runtime-resource path
`AssetManager::Adopt` exists for (a prefab cannot reference a runtime mesh by id, since
an adopted resource carries the invalid `AssetId` the serializer records as "no
asset"):

- Author `assets/sphere.prefab.json` — one entity with `Transform`, a `MeshRenderer`
  (its `Mesh` field **omitted**, so it cooks to the "no asset" value), and the game
  `Spinner` — and add it to the pack manifest as a `prefab`-type entry with a minted
  `AssetId`. **No mesh is cooked** — the pack stays as it is geometry-wise.
- The pack is declared with `MODULE hello_triangle` so it cooks after the lib (the
  prefab validates against the module's reflected `Spinner`).
- `OnInitialize` still generates + adopts the primitive mesh exactly as before
  (`Primitives::Icosphere` → `Mesh::Create` → `GetAssetManager().Adopt(...)`), but no
  longer builds the entity by hand. Instead it creates the empty world, loads + spawns
  the prefab, and **assigns the adopted mesh to the spawned renderer**:
  `m_Scene = Scene::Create(GetTypeRegistry());`
  `auto prefab = GetAssetManager().LoadSync<Prefab>(<prefab id>);`
  `auto roots = prefab->SpawnInto(*m_Scene, GetAssetManager());`
  then `m_Scene->Get<MeshRenderer>(roots[0]).Mesh = adoptedSphere;`. The `Transform`
  and `Spinner` come **from the prefab**; only the mesh — a runtime resource — is wired
  in code. The `Spinner` is driven over a query exactly as before; the entity's
  *structure and authored values* now come from cooked data, not code.

Mint the real `AssetId` (and any new `TypeId`s) with `vengc generate-id` /
`generate-type-id` once the build is green, replacing placeholders (working norm).

## Verification

- Clean build (`-j 2`); `ctest` green across `unit`/`death`/`cooker`/`gpu`.
- **Validation gate:** `cmake -B build-debug -DVE_DEBUG=ON` then `ctest --test-dir
  build-debug -L validation` — the render path is materially unchanged, so the
  allowlist stays empty (it may not be widened).
- **Smoke:** the launcher writes a correct-sized 1280×720 RGB PPM (≈ 2,764,816
  bytes) and exits 0 — through the full `dlopen` → load prefab → spawn → draw chain.
- **`smoke_golden`:** the pose **and the geometry are unchanged** — the same
  `Primitives::Icosphere` is generated, adopted, and drawn; only the entity's
  `Transform`/`Spinner` now come from a spawned prefab. So the capture **must not
  move** and the golden is **not** regenerated: a green `smoke_golden` against the
  existing `tests/golden/hello_triangle_scene.png` is the proof the data-driven path
  renders byte-for-byte identically. (If it moves, something — the prefab's authored
  `Transform`, the post-spawn mesh assignment — diverged from the hand-built world and
  must be reconciled, not blessed.)

## Acceptance

Clean build; full `ctest` green; validation gate clean (allowlist empty); smoke PPM
correct-sized and exit 0; `smoke_golden` green **against the unchanged golden** (the
geometry is identical — not regenerated). Commit: `Plan 04: Prefab asset loader +
SpawnInto + prefab-driven sample — uniform Load path, reference remap, add_asset_pack
MODULE edge`.
