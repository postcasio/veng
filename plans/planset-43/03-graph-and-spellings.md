# Plan 03 — graph enums + the spelling migrations

**Goal:** the JSON surfaces that are *not* forks of the reflection walker join the
convention. Node-graph enum properties serialize by name; the three legacy lowercase string
forms — material `"domain"`, the pack-manifest `"type"`, the raw texture `"compression"` —
move to exact C++ enumerator spellings; every affected JSON asset migrates in the same pass.
Depends on Plan 00b (the enum-name functions); independent of Plans 01/02.

## The starting point

- [NodeGraphSerialize.cpp](../../graph/src/NodeGraphSerialize.cpp) reads/writes an enum
  property as a raw `i32` (`"Provenance": 1` in both sample graphs). Graph properties are
  deliberately registry-free: "a property walk never requires a TypeRegistry lookup"
  (`NodeType.h`), so the enum table must reach the serializer another way.
- [MaterialImporter.cpp](../../cooker/src/Importers/MaterialImporter.cpp) hand-parses
  `"domain"` as `"surface"`/`"postprocess"` into a raw `u32`;
  [GraphShaderSource.cpp](../../cooker/src/Importers/GraphShaderSource.cpp) duplicates the
  parse; [MaterialCompile.cpp](../../graph/src/MaterialCompile.cpp) writes the lowercase
  form. `MaterialDomain` (`Veng/Asset/Material.h`) has no `VE_ENUM` and no name table.
- [AssetType.cpp](../../assetpack/src/AssetType.cpp) maps manifest `"type"` to lowercase
  snake_case (`"material_instance"`, `"vertex_layout"`, `"input_map"`), used by all five
  in-tree pack manifests and echoed by `vengc` output and the editor's MCP asset table.
- [TextureImporter.cpp](../../cooker/src/Importers/TextureImporter.cpp) accepts
  `"compression"`: `"astc"`/`"bc7"`/`"none"` for its private `TextureCodec` (BC5/BC4
  deliberately have no raw spelling — role-table only).

## What lands

### 1. Node-graph enum properties by name

- `NodeGraphSerialize` writes an enum property as its enumerator name and reads names only
  (the hard cut), through Plan 00b's `EnumeratorName`/`ParseEnumValue`.
- The enumerator table reaches the serializer **without a TypeRegistry**, preserving the
  documented property-walk constraint: the `NodeCatalog` records each enum property type's
  `VE_ENUM` table at registration. **Mechanism:** an enumerator-span pointer attached at the
  property-authoring helper (the authoring site has the compile-time type, so it hands the
  `VE_ENUM` span straight in) — consistent with the reflection macros' compile-time-attached
  metadata style, rather than a side-table `TypeId → span` map on the catalog. A
  `TypeRegistry`-keyed lookup (mirroring `RegisterMaterialGraphTypes`, which the editor calls
  from `EditorHost`) is a **false economy**: `GraphShaderSource.cpp`'s cook path has no
  `TypeRegistry` in scope, so it would work for editor preview but not the offline cook — the
  table must ride the catalog itself.
- Migrate both sample graphs: `"Provenance": 1` → `"Provenance": "Exposed"` in
  `examples/hello-triangle/assets/shaders/brick.frag.graph.json` and
  `examples/template/assets/shaders/flat.frag.graph.json`, plus any graph fixtures under
  `tests/`.

### 2. `MaterialDomain` — a real enum with real names

- `MaterialDomain` gains `VE_ENUM(::Veng::MaterialDomain, <minted id>)` (id via
  `vengc generate-type-id`; placeholder while implementing, per the working norms).
- `MaterialImporter` and `GraphShaderSource` parse `"domain"` through `ParseEnum
  <MaterialDomain>` — `"Surface"`/`"PostProcess"`, exact — and carry the typed enum instead
  of a raw `u32`; `MaterialCompile` writes `EnumeratorName`. The duplicated hand-parse
  disappears. (Both TUs already compile against engine headers; the `VE_ENUM` trait is
  header-only, so the veng-free bootstrap constraint is unaffected — confirm the bootstrap
  TU set still links clean.)
- Migrate every `.vmat.json` / graph-shader `.shader.json` carrying `"domain"`: both
  examples' materials + shader sources, `engine/assets/core/materials/tonemap.vmat.json`,
  and the `tests/cooker/fixtures/materials/` set (`postprocess*.vmat.json`; `bad_domain`
  updates to a value that is *still* bad under the new spellings, and its expected error
  message follows).

### 3. Pack-manifest `"type"` respelled

- The `ToString(AssetType)`/`ParseAssetType` hand table respells to the exact enumerator
  names: `"Raw"`, `"Texture"`, `"Mesh"`, `"Shader"`, `"Material"`, `"MaterialInstance"`,
  `"VertexLayout"`, `"Prefab"`, `"Level"`, `"Skeleton"`, `"Animation"`, `"Environment"`,
  `"InputMap"`. assetpack sits below the reflection layer, so it keeps a hand table — only
  the spellings change; everything downstream (`vengc` messages, `Verify`, the editor MCP
  asset table) follows automatically through `ToString`.
- Migrate all five app/engine manifests: `engine/assets/core/core.vengpack.json`,
  `tests/shaders/test.vengpack.json`, `editor/assets/icons/editor_icons.vengpack.json`,
  `examples/template/assets/template.vengpack.json`,
  `examples/hello-triangle/assets/sample.vengpack.json` — **and every `*pack*.json` manifest
  under `tests/cooker/fixtures/`** (~28 of them, cooked directly by the cooker suite, not
  merely `--reference`d — all carry lowercase `"type"` keys and all break under the hard
  cut), plus any `--reference` pack JSON elsewhere under `tests/`.

### 4. The raw `"compression"` escape hatch

- `ParseCodec` accepts `"ASTC"`/`"BC7"`/`"None"` (the `TextureCodec` enumerator spellings),
  hard cut. BC5/BC4 remain role-table-only. The editor's `TextureEditorPanel` write path (if
  it writes the raw key) and the `tests/cooker/fixtures/textures/*.tex.json` fixtures
  migrate with it.

## Verification

- `build-debug` clean; full `ctest` green — the cooker suite over the migrated fixtures, the
  graph/material tests, and `smoke_golden` + `hello_triangle_launcher_smoke` (respelled
  manifests + domains must cook byte-equivalent packs; the golden must not move).
- The bootstrap core-pack cook (`veng_cook_bootstrap`) still builds and cooks the respelled
  `core.vengpack.json` — the manifest + `"domain"` changes run through it.
- Sweep: `rg -i '"(surface|postprocess|astc|bc7|none|material_instance|vertex_layout|input_map)"'
  --glob '*.json' -g '!build*'` over the tree comes back with only deliberate survivors
  (e.g. Slang entry-point names), reviewed by hand. This regex does **not** catch the
  single-word `"type"` spellings (`"texture"`, `"mesh"`, `"prefab"`, `"level"`, `"raw"`, …),
  so also sweep every manifest `"type":` site directly — `rg '"type"\s*:\s*"[a-z]' --glob
  '*vengpack*.json' --glob '*_pack*.json' -g '!build*'` must come back empty — and rely on
  the cooker suite (which cooks the fixtures) as the loud backstop.

## Out of scope

- `Renderer::Format` / vertex-layout name tables (already exact-spelling; no `VE_ENUM`
  migration — see the planset README's future notes).
- The `.buildcfg` / `.tex.json` `"role"` tables — already conforming.
