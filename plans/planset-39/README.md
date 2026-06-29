# planset-39 — explicit material-instance ids (every reference names a real instance)

**Phase goal:** retire the parent-id **overload** planset-38 Plan 05 introduced — where one `AssetId`
names both a `Material` (the parent) and a `MaterialInstance` (its implicit zero-override default), kept
apart only by a composite `(type, id)` cache key and a resolve-time bridge. This planset takes the
**explicit-id** path Plan 05 chose against: **mint a fresh `MaterialInstance` id for every material**,
emit a real default-instance asset at that id, **rewrite every material reference in every cooked pack
(and every C++ literal) to use it**, and then **delete** the composite cache key and the
default-instance fallback. After this, one `AssetId` names exactly one asset of one type again, and a
material reference always resolves to a real `MaterialInstance` archive entry — no special case in the
hot resolve path.

## Why undo the overload

Plan 05 retyped every material reference (the mesh material list, the `MeshSource` shape fields,
`Primitives`, the components, prefab/level reflected fields) from `AssetHandle<Material>` to
`AssetHandle<MaterialInstance>`, but left the **id values** in already-cooked content pointing at the
parent `Material` ids. To keep that content loading without re-minting or re-cooking, Plan 05 made a
parent-material id *also* serviceable as a `MaterialInstance` request:

- `AssetManager::Resolve` carries a default-instance rule — a `MaterialInstance` request that finds a
  bare `Material` archive entry routes to `MaterialInstanceLoader::LoadDefaultInstance`, which builds
  the parent and wraps it zero-override.
- Because `Material@id` and `MaterialInstance@id` can then both be live for one id, the asset cache is
  keyed by **`(type, id)`** rather than by `id` alone.

That bought zero migration churn at the cost of three standing compromises: **(1)** one id legitimately
names two assets, breaking the "one id ⇒ one asset" invariant the rest of the cache, the prefab
rehydrate path (`CachedEntry(id)`, which must scan across types), and tooling assume; **(2)** a
type-mismatch branch sits in the per-load resolve path; and **(3)** the default-instance object is
synthesized at load with an **id-less** parent handle, a second way a `Material` enters memory that the
normal id-cached loader path does not produce. The explicit-id model removes all three: the default
instance becomes an ordinary cooked asset with its own id, references name it directly, and the resolve
path and cache key go back to being type-agnostic.

This is a deliberate trade of **authoring/format churn now** for a **simpler, unambiguous runtime
invariant**. The cost is real — every material gains a companion default-instance asset and every
reference is rewritten — which is exactly why Plan 05 deferred it. This planset pays it.

## The model — a declared default-instance id per material

A parent material's `*.vmat.json` gains a **`defaultInstance`** id (a freshly minted `AssetId`,
decimal in the pack JSON). Cooking the material emits, beside the `Material` blob, a companion
**zero-override `MaterialInstance`** blob at that id (`parent` = the material, empty `overrides`) through
the Plan 06 `MaterialInstanceImporter` path. Every reference that used to name the material id is
rewritten to name the `defaultInstance` id. The parent material id is then referenced **only** by an
instance's `parent` field and by the editor/preview — never by a mesh, prefab, level, or primitive.

An authored override instance (`*.vmatinst.json`, Plan 06) is unchanged: it already has its own id and
names its parent explicitly. This planset only makes the **implicit** default explicit.

## Plans

| # | Plan | Summary | Status |
|---|---|---|---|
| 01 | [Explicit default-instance ids; drop the `(type, id)` overload](01-explicit-default-instance-ids.md) | A parent `*.vmat.json` declares a minted `defaultInstance` id; the cook emits a companion zero-override `MaterialInstance` at it. Every material reference in every pack (hello-triangle, template, core, test fixtures) and every C++ material-id literal (`Primitives`, `MeshSource` defaults, the fallback material) is rewritten to the default-instance id. The composite `(type, id)` cache key reverts to id-only (`CacheKey`/`CacheKeyHash` deleted, `CachedEntry(id)` a plain lookup), and `AssetManager::Resolve`'s default-instance branch + `MaterialInstanceLoader::LoadDefaultInstance` are deleted. Both examples co-migrate; `smoke_golden` holds (a real zero-override instance packs the same bytes the bridge did). | proposed |

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = implemented, migrated, verified, committed.

## Dependencies

- **Depends on planset-38 being complete** — specifically Plan 05 (the `MaterialInstance` runtime +
  the overload this planset removes) and Plan 06 (the `MaterialInstanceImporter` that cooks the
  companion `*.vmatinst.json` / the cooker default-instance emit this planset drives). Planset-39 starts
  only after planset-38 lands.
- Plan 01 stands alone within this planset.

## The decision this planset settles

- **One `AssetId` names one asset of one type.** The parent-id overload (a `Material` id doubling as a
  `MaterialInstance` request) is removed; a material reference names a real, distinct
  `MaterialInstance` id. The asset cache key and the resolve path are type-agnostic again, and the
  prefab rehydrate path's cross-type `CachedEntry(id)` scan collapses back to a direct lookup.
- **The default instance is a cooked asset, not a load-time synthesis.** A material declares a minted
  `defaultInstance` id; the cook emits a zero-override `MaterialInstance` at it. There is no implicit
  bridge and no id-less parent handle — a default instance loads through the same path as an authored
  override instance.

## What remains future

- **Cooker auto-mint of the default-instance id.** This planset declares the `defaultInstance` id in
  the material source (minted by hand via `vengc generate-id`). Having the cooker mint and record it
  automatically (so a material need not declare one) is an ergonomic follow-on, deferred until the
  explicit model proves out.
- **Dropping the default instance for unreferenced materials.** A material referenced only as a
  parent (never directly) needs no companion default instance; pruning those is a cook-time
  optimization left for later.
