# Plan 01 — cooker adoption: prefab + level importers

**Goal:** the cooker's two reflection walkers become call sites of the shared one.
`PrefabImporter` and `LevelImporter` drop their hand-rolled `BindField`s for
`JsonReadFields` + hooks; enum fields in prefab/level JSON become enumerator-name strings
(hard cut); every prefab JSON in the tree migrates in the same pass. Depends on Plan 00b.

## The starting point

- [PrefabImporter.cpp](../../cooker/src/Importers/PrefabImporter.cpp) `BindField` (~250
  lines): all FieldClasses, enum **as integer** (`"expected an integer enum value"`),
  AssetHandle validated against the pack resolver, Reference remapped to prefab-local entity
  indices, located errors carrying file/entity/component context.
- [LevelImporter.cpp](../../cooker/src/Importers/LevelImporter.cpp) `BindField` (~150
  lines): the trimmed copy — Scalar/Vector/AssetHandle/Struct only; Enum/Reference/Variant/
  Array are "unsupported field class in level config"; no resolve validation.
- Live integer enums in assets: `"Type"` (`LightType`) and `"Tier"` in
  `examples/hello-triangle/assets/prefabs/*.prefab.json`, `"Type"`/`"Space"`
  (`MotionSpace`) in `examples/template/assets/prefabs/scene.prefab.json`,
  plus `tests/cooker/fixtures/prefabs/`.

## What lands

### 1. PrefabImporter on the shared walker

- `BindField` (and its per-class ladder) is deleted; component binding becomes
  `JsonReadFields(componentPtr, typeInfo, componentJson, registry, hooks)` with:
  - `ValidateAssetId` = the existing pack-resolve type check (`AssetTypeForHandleField` /
    `AssetTypeAccepted` against `resolve(id)`), now reporting asset types **by name**
    (`ToString(type)`, not `static_cast<u32>`).
  - `ReadReference` = the existing prefab-local entity-index mapping.
- The importer keeps everything that is genuinely its own: the entity/component table walk,
  `TypeId` resolution against the reflected registry, intra-prefab reference bookkeeping,
  and the `WriteFields` blob emission. Located errors keep their exact
  file/entity/component prefix, now wrapping the walker's dotted field path.

### 2. LevelImporter on the shared walker

- Same replacement; the level config records (`GameModeConfig`, `LevelRenderSettings`, the
  session seed) bind through the identical walker and **gain Enum/Variant/Array support**
  (Reference stays unset → still an error, now a deliberate policy rather than a trimmed
  copy). The "unsupported field class in level config" arm disappears.

### 3. The asset migration (hard cut)

- Every prefab JSON with an integer enum migrates to the enumerator name:
  - `examples/hello-triangle/assets/prefabs/` — `"Type": 0` → `"Type": "Directional"`
    (and Point/Spot where used), `"Tier": 1` → `"Tier": "Local"` / `0` → `"Server"`.
  - `examples/template/assets/prefabs/scene.prefab.json` — `"Type": 0` → `"Type":
    "Directional"`, `"Space": 1` → `"Space": "World"`.
  - `tests/cooker/fixtures/prefabs/` and any other fixture with an enum field; a fixture
    that *tests* the malformed case updates its expected error to the new message.
- Sweep for stragglers: `rg '"(Type|Tier|Space|Mode|Kind|Phase)":\s*[0-9]' --glob
  '*.prefab.json' --glob '*.level.json'` (and the level fixtures) must come back empty.
- Both examples co-migrate here, per the working norms; the template's cook is exercised by
  the SDK conformance tests.

## Verification

- `build-debug` clean; full `ctest` green — in particular the `cooker` suite (fixtures
  re-cook, malformed-enum fixtures produce the new located error) and `smoke_golden` /
  `hello_triangle_launcher_smoke` (the migrated prefabs cook and render identically — this
  change must not move the golden).
- A deliberate spot check: temporarily reintroduce one integer enum in a scratch prefab and
  confirm the cook fails with a located dotted-path error naming the field.

## Out of scope

- The editor's write side and MCP (Plan 02); graph/domain/manifest spellings (Plan 03).

Note: Plan 01 opens a window where the editor still *writes* integer enums while the cooker
rejects them, so **01 and 02 are sequenced (01 then 02, ideally one session)** — see the
README's Dependencies section. This is a scheduling constraint, not a scope boundary.
