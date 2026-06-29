# Plan 01 — explicit default-instance ids; drop the `(type, id)` overload

**Goal:** replace planset-38 Plan 05's parent-id overload with explicit `MaterialInstance` ids. Every
parent material declares a minted **`defaultInstance`** id; the cook emits a companion zero-override
`MaterialInstance` at that id; every material reference (in every cooked pack and every C++ literal) is
rewritten to name the default-instance id. Then the runtime overload that made the old scheme work — the
composite `(type, id)` cache key and `AssetManager::Resolve`'s default-instance bridge — is **deleted**,
restoring the "one id ⇒ one asset of one type" invariant. Depends on planset-38 (Plans 05 + 06).

## The starting point (what Plan 05 left)

- `AssetManager` keys its cache by a composite `CacheKey{ AssetType, AssetId }` with a `CacheKeyHash`
  (`engine/include/Veng/Asset/AssetManager.h`), so `Material@id` and `MaterialInstance@id` coexist.
  `CachedEntry(AssetId)` scans the map for the first entry matching the id across types
  (`engine/src/Asset/Prefab.cpp` rehydrate relies on this).
- `AssetManager::Resolve` (`engine/src/Asset/AssetManager.cpp`) carries the **default-instance rule**:
  when `type == AssetType::MaterialInstance && found->Type == AssetType::Material`, it routes to
  `MaterialInstanceLoader::LoadDefaultInstance`.
- `MaterialInstanceLoader::LoadDefaultInstance`
  (`engine/src/Asset/Loaders/MaterialInstanceLoader.{h,cpp}`) builds the parent `Material` inline from
  its blob, `Adopt`s it into an **id-less** handle, and wraps it in a zero-override `MaterialInstance`.
- Every material reference field is `AssetHandle<MaterialInstance>` but the cooked data stores the
  **parent material id**.

## What lands

### 1. A declared default-instance id on the parent material

- A parent `*.vmat.json` gains an optional **`defaultInstance`** key: a minted `AssetId` (decimal in the
  pack JSON). It is the id of the zero-override instance every direct reference to this material uses.
- The cook emits, beside the `Material` blob, a **zero-override `MaterialInstance`** blob at the
  `defaultInstance` id — `parent` = this material's id, `overrides` = empty — through the Plan 06
  `MaterialInstanceImporter` (or, equivalently, the importer's blob writer invoked from the material
  cook). The emitted blob is byte-identical to what `LoadDefaultInstance` synthesized, so the resident
  param block is unchanged.
- Validation: a material that is referenced as an instance **must** declare `defaultInstance`; the
  cooker errors (located `Result`) if a reference names a material id with no companion default instance.

### 2. Rewrite every reference to the default-instance id

Every place that stored a parent material id in an `AssetHandle<MaterialInstance>` slot is repointed to
the `defaultInstance` id:

- **Cooked packs** — `hello-triangle`, `template`, the engine `core` pack (`engine/assets/core`), and
  the cooker/gpu **test fixtures**: mesh `*.mesh.json` material lists, prefab/level material fields, and
  any pack manifest material reference. Add the `defaultInstance` id to each material and rewrite its
  referrers.
- **C++ material-id literals** — the `Primitives` factory defaults, the `MeshSource` default material
  fields, and the engine's fallback/built-in material (`engine/src/Scene/…`, the primitive builders).
  Each hardcoded `AssetId{0x…ULL}` that named a `Material` becomes the corresponding `defaultInstance`
  id (uppercase hex `0x…ULL` per house style).
- The minted ids are produced with `vengc generate-id` (with `--reference <pack.json>` per pack so they
  cannot collide), replacing clearly-marked placeholders.

### 3. Delete the overload

With every reference naming a real `MaterialInstance` archive entry, the bridge is dead weight:

- **Cache key → id-only.** `AssetManager`'s `std::unordered_map<CacheKey, …>` reverts to
  `std::unordered_map<AssetId, …>`; `CacheKey`/`CacheKeyHash` are deleted; the two `m_Cache.find/insert`
  sites and `CachedEntry(AssetId)` go back to a direct `id` lookup (the cross-type scan is gone). Verify
  no remaining caller depends on two types sharing an id.
- **Resolve bridge deleted.** The `type == MaterialInstance && found->Type == Material` branch in
  `AssetManager::Resolve` is removed; a `MaterialInstance` request now resolves only a real
  `MaterialInstance` archive entry (a missing one is an ordinary `NotFound`).
- **`LoadDefaultInstance` deleted.** `MaterialInstanceLoader` keeps only its normal `Load`
  (cooked-blob → instance); the id-less-parent synthesis path and its declaration are removed.

## Files (sketch — the agent confirms against the tree)

- `engine/include/Veng/Asset/AssetManager.h`, `engine/src/Asset/AssetManager.cpp` — revert cache key,
  delete the resolve bridge, restore `CachedEntry`.
- `engine/src/Asset/Loaders/MaterialInstanceLoader.{h,cpp}` — delete `LoadDefaultInstance`.
- `engine/src/Asset/Prefab.cpp` — `CachedEntry(id)` call site (now a direct lookup).
- `engine/src/Scene/Primitives.*`, the `MeshSource`/`MeshRenderer` defaults, the fallback material —
  repoint C++ material-id literals.
- `cooker/src/Importers/MaterialImporter.*` / `MaterialInstanceImporter.*` — emit the companion default
  instance from the parent's `defaultInstance` key; validate referenced materials declare one.
- `engine/assets/core/**`, `examples/hello-triangle/assets/**`, `examples/template/assets/**`,
  `tests/**` fixtures — add `defaultInstance` ids and rewrite references.
- Docs: `engine/CLAUDE.md` (the default-instance is now a cooked asset, not a load-time synthesis; the
  cache key is id-only again), `assetpack/CLAUDE.md` / `cooker/CLAUDE.md` (the `defaultInstance` key).

## Examples to co-migrate

Both `hello-triangle` and `template` declare `defaultInstance` ids on their materials and rewrite their
mesh/prefab/level references. Neither authors an override instance for this plan (Plan 06's sample
already proves the override path); the change here is purely default-instance ids + reference rewriting.

## Verification

- Clean build; full `ctest` green. Add a test asserting (a) a material id and its `defaultInstance` id
  are distinct and both resolve to their own typed asset, and (b) a `MaterialInstance` request for an id
  that is a bare `Material` now returns `NotFound` (the bridge is gone).
- `HT_SMOKE` writes the 2,764,816-byte PPM and **`smoke_golden` holds** — a cooked zero-override
  instance packs the same bytes the synthesized default did, so the image does not move.
- **Validation gate** (`build-debug -L validation`) clean — the loader/resolve change is exactly where a
  Vulkan validation error would hide.
- Grep proves no `CacheKey`, no `LoadDefaultInstance`, and no `type == … MaterialInstance && … Material`
  branch remain.

## Risks

- **A missed reference** (a fixture mesh, a prefab field, a C++ literal) still naming a bare material id
  now fails to load (`NotFound`) instead of silently bridging — which is the point, but it means the
  migration must be exhaustive. The cooker's "referenced material must declare `defaultInstance`"
  validation turns a missed source reference into a loud cook error; a missed C++ literal surfaces as a
  failing gpu/smoke test. Lean on both.
- **Pack-id collisions** when minting across many packs — always mint with `--reference` for the target
  pack.
