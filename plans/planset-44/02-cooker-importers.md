# Plan 02 — cooker importers + manifest

**Goal:** every hand-read and hand-written id in `cooker/src/` moves to the hex-string form
through the Plan 00 codec. This is the read side of the format — the cook is what consumes the
authored packs — plus the two places the cooker *writes* JSON. `ActionId` and `SystemId`
convert here alongside `AssetId`. Depends on Plan 00.

## The sites

Each is currently `is_number_unsigned()` + `get<u64>()` on the read side. Convert each to:
`is_string()` guard → `ParseAssetId`/`ParseHexId` → the importer's existing located error on
`nullopt`. Preserve every "absent/zero → invalid id" default exactly.

**Manifest + project ([Cooker.cpp](../../cooker/src/Cooker.cpp)):**
- pack entry `id` — **two independent reads, both must convert**: `ParseAssetPack` (line ~130,
  used for the current pack and every `--reference` pack) *and* `CookEntry` (line ~612, the
  per-asset cook path invoked from `CookPack` that re-validates `entry["id"]` on its own). These
  are separate code paths, not a call-through; converting only line 130 leaves line 612 rejecting
  every hex manifest, so **every cook of every pack fails** once Plan 04 migrates the manifests.
  The post-sweep `rg` guards check JSON, not C++ coverage, so this would surface only at `ctest`
  time — convert both here.
- `startupLevel` (line ~306) — `AssetId`; absent/zero stays the invalid id.
- `defaultInstance` (line ~676) — `AssetId`.
- pack `version` (line ~83) is **not** an id — leave numeric.

**Importers:**
- [ShaderImporter.cpp:604](../../cooker/src/Importers/ShaderImporter.cpp) — `vertex_layout` `AssetId`.
- [MaterialImporter.cpp](../../cooker/src/Importers/MaterialImporter.cpp) — `shaders.vertex` /
  `shaders.fragment` (line ~128) and a texture field's `id` (line ~345). The field `"value"`
  at line ~366 is a scalar param value, **not** an id — leave numeric.
- [MeshImporter.cpp](../../cooker/src/Importers/MeshImporter.cpp) — `materials` map **values**
  (line ~124, the ids) become hex strings; the map **keys** (submesh indices, decimal strings)
  stay as-is. `skeleton` (line ~159) — `AssetId`; note it currently guards on `is_number()`,
  which becomes `is_string()`.
- [LevelImporter.cpp](../../cooker/src/Importers/LevelImporter.cpp) — `world` (line ~103)
  `AssetId`; `systems[]` entries (line ~140) — **`SystemId`** (`FormatHexId`/`ParseHexId`).
- [InputMapImporter.cpp](../../cooker/src/Importers/InputMapImporter.cpp) — action `id`
  (line ~81) and binding `action` (line ~132) — **`ActionId`**. `control` (line ~169) is a raw
  device code — **leave numeric**.
- [MaterialInstanceImporter.cpp](../../cooker/src/Importers/MaterialInstanceImporter.cpp) —
  `parent` (line ~217), the parent's `shaders.fragment` (line ~62), and texture-override ids
  (both the bare-value form at line ~299 and the `{ "id": … }` object form at line ~304) —
  `AssetId`.
- [EnvironmentImporter.cpp:66](../../cooker/src/Importers/EnvironmentImporter.cpp) /
  [TextureImporter.cpp:500](../../cooker/src/Importers/TextureImporter.cpp) — `max_size` is a
  dimension, **not** an id — leave numeric.
- [AnimationImporter.cpp](../../cooker/src/Importers/AnimationImporter.cpp) — `clip` /
  `trimStart` / `trimEnd` are indices/frames, **not** ids — leave numeric.
- [PrefabImporter.cpp](../../cooker/src/Importers/PrefabImporter.cpp) — the entity `id`
  (line ~199), component-array `index` (line ~104), and the `strtoull` entity-key parse
  (line ~261) are all **entity-local indices** — leave numeric. Component `AssetHandle` fields
  go through the Plan 01 walker, not here.

## The two cooker JSON writes

- [MaterialCompile.cpp](../../graph/src/MaterialCompile.cpp) (in `veng::graph`, emitted by the
  cook) — `shaders["vertex"]`/`["fragment"]` (line ~367) and a texture field `entry["id"]`
  (line ~380) currently assign `.Value` (a number). Assign `FormatAssetId(...)`.
- [MaterialInstanceImporter.cpp:452](../../cooker/src/Importers/MaterialInstanceImporter.cpp) —
  the synthesized default-instance document `inst["parent"] = parentId` becomes
  `FormatAssetId(AssetId{parentId})`.

## Includes

`#include <Veng/Asset/HexId.h>` in each touched `.cpp`. `MaterialCompile.cpp` is in
`veng::graph`, which links `veng::veng` (and transitively `assetpack`), so the header resolves.

## Migration note

The cooker fixtures under `tests/cooker/fixtures/` and the checked-in packs the cook reads are
migrated in Plan 04. Between this plan and 04 the cooker suite will fail against un-migrated
numeric fixtures — that is expected. Per the README's **landing discipline**, the whole planset
lands on a feature branch and merges only once Plan 04 is green, so this red intermediate state
never reaches `main`. (A per-plan fixture co-migration can't keep every commit green anyway,
since the core manifest is read by both this plan's cooker and Plan 03's `AssetSourceIndex` —
hence the branch, not per-plan greening.)

## Verification

Deferred to the end of Plan 04 (the readers and their assets must move together). At this
plan's boundary: `build-debug` compiles clean (`-Werror`), and a hand-migrated single fixture
cooks through each touched importer.

## Out of scope

- The two reflective walkers (Plan 01).
- The editor authoring panels and MCP (Plan 03).
- The asset/fixture/doc/C++-literal migration (Plan 04).
