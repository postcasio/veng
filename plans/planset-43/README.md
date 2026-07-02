# planset-43 — JSON serialization unified: one reflection walker, enums by name

**Phase goal:** one implementation of JSON⇄reflection, and one enum convention across every
JSON surface. Today the tree carries **four near-identical hand-rolled JSON⇄reflection
walkers** — the cooker's [PrefabImporter](../../cooker/src/Importers/PrefabImporter.cpp) and
[LevelImporter](../../cooker/src/Importers/LevelImporter.cpp) (read), the editor's
[PrefabSerialize](../../editor/src/PrefabSerialize.cpp) (write), and the MCP server's
[ReflectToJson](../../mcp/src/ReflectToJson.cpp) (both) — ~2,200 lines whose mechanical core
(Scalar/Vector/Quaternion/Matrix/String/Struct recursion) is the same code four times, forked
by copy-paste. The binary side never had this problem: `Veng/Reflection/Serialize.h`
(`WriteFields`/`ReadFields`) is the one shared walker. This planset gives the JSON direction
the same treatment — a shared, policy-hooked walker in `Veng/Reflection/` — and uses the
consolidation to settle the enum convention the forks diverged on: **every enum in asset JSON
serializes as a string, spelled exactly as the C++ enumerator**, parsed and emitted through
standard shared functions.

The divergence is live in the assets: a prefab authors `"Type": 0` / `"Space": 1` /
`"Tier": 1` (integers), an input map authors `"kind": "Axis2D"` (a name), a node graph
authors `"Provenance": 1` (an integer), MCP emits `{ "value": 1, "name": "Local" }` (an
object), a `.buildcfg` authors `"ASTC4x4Srgb"` (a name), and a `.vmat` authors
`"postprocess"` (a lowercase string matching no C++ spelling). After this planset they are
all enumerator names: `"Type": "Directional"`, `"Space": "World"`, `"Provenance": "Exposed"`,
`"domain": "PostProcess"`.

## Why now

- **The machinery half-exists and already declares the intent.** `Veng/Reflection/EnumName.h`
  ships `ParseEnum<T>`/`EnumeratorName<T>` over the `VE_ENUM` enumerator tables, documented
  as *"JSON authors an enum by name (never ordinal)"* — and the input-map path (cooker
  importer + editor panel) already conforms. Every enum appearing in asset JSON is already
  `VE_ENUM`-reflected. What's missing is only the **runtime-typed** (`TypeInfo`-based)
  overloads the reflection-walking serializers need, and a shared walker to put them in.
- **Each new consumer re-forks the walker.** LevelImporter is a visibly trimmed copy of
  PrefabImporter's `BindField`; MCP re-implemented both directions; the next JSON surface
  would fork it a fifth time. The enum inconsistency is the direct symptom — four copies made
  four independent calls. Consolidating first means the enum rule is implemented **once**.
- **The gaps are already biting.** LevelImporter omits `Enum`/`Reference`/`Variant`/`Array`
  entirely — an enum field in level config is a hard "unsupported field class" error today,
  so a `LevelRenderSettings` knob can't be an enum. The shared walker fixes that for free.

## The unifying design

### One walker in `Veng/Reflection/`, beside the binary one

`Veng/Reflection/JsonSerialize.h` — `JsonReadFields` / `JsonWriteFields`, the JSON analogue
of `Serialize.h`'s `ReadFields`/`WriteFields`: a recursive, tolerant, name-keyed walker over
`FieldDescriptor`/`TypeInfo` covering **all** of Scalar / Vector / Quaternion / Matrix /
String / Enum / AssetHandle / Reference / Struct / Variant / Array. The consumers keep only
what is genuinely theirs, as **policy hooks**:

- **AssetHandle validation** — the prefab importer validates a nonzero id against the pack
  resolver (cook-time type check); everyone else accepts the raw id. A
  `ValidateAssetId(u64, TypeId) → VoidResult` hook, default-accepting.
- **Entity references** — the prefab importer maps to prefab-local indices, the editor's
  writer inverts that from live entities, MCP uses its own entity addressing. A
  `ReadReference`/`WriteReference` hook pair; unset → `Reference` is an error (LevelImporter's
  current posture, now deliberate).
- **Error context** — the walker reports **dotted field paths** ("`Light.Type`: expected an
  enumerator name"); each consumer prepends its own located prefix (file/entity/section), so
  the located-error UX is preserved without a formatting hook.

This lives **in `libveng`** (decision below), so cooker, editor, and mcp all consume the one
walker, exactly as they consume the one binary serializer.

### Enums by name, through standard functions

`EnumName.h` grows the runtime-typed siblings of the templated pair, and the walker (plus
every remaining hand-parsed enum site) goes through them:

```cpp
/// The authored enumerator name of a value, via the type's VE_ENUM table.
[[nodiscard]] string EnumeratorName(const TypeInfo& info, i64 value);
/// The inverse; exact, case-sensitive. nullopt when the name matches no enumerator.
[[nodiscard]] optional<i64> ParseEnumValue(const TypeInfo& info, string_view name);
/// Size-aware load/store of an enum field's backing bytes (info.Size).
[[nodiscard]] i64 LoadEnumBits(const void* fieldPtr, const TypeInfo& info);
void StoreEnumBits(void* fieldPtr, const TypeInfo& info, i64 value);
```

The convention: **write the exact C++ enumerator spelling; read strings only** (the hard
cut — an integer where an enum name is expected is a located cook/parse error, and the
assets migrate in the same pass). Matching stays exact and case-sensitive. The hand-written
constexpr tables that already conform (`CompressionRole`/`CompressionFormat`, kept
header-inline for the veng-free cook bootstrap) stay as they are.

### The spelling migrations

Three JSON string forms predate the convention and move to the exact C++ spellings:

- **Material `"domain"`** — `"surface"`/`"postprocess"` → `"Surface"`/`"PostProcess"`
  (`MaterialDomain` gains a `VE_ENUM`; the importer, `GraphShaderSource`, and
  `MaterialCompile`'s writer go through the shared functions).
- **Pack-manifest `"type"`** — `"texture"`/`"material_instance"`/`"vertex_layout"`/… →
  `"Texture"`/`"MaterialInstance"`/`"VertexLayout"`/… (the hand table in
  `assetpack/src/AssetType.cpp` respells; assetpack sits below the reflection layer, so it
  keeps a hand table — only the spellings change).
- **Texture raw `"compression"`** — `"astc"`/`"bc7"`/`"none"` → `"ASTC"`/`"BC7"`/`"None"`,
  matching the importer's `TextureCodec` enumerators (BC5/BC4 stay role-table-only, as
  documented).

### nlohmann/json becomes a PUBLIC dependency of `libveng`

The walker's API names `json` types, so the engine's deliberate JSON-free posture ends —
**a decided trade** (see decisions). nlohmann joins glm/fmt/ImGui as a PUBLIC dep:
`include_hygiene` compiles public headers with it, the SDK export/install carries it
(`find_dependency` in `veng-config` + an installed copy for the install-prefix mode), and
the consumers' now-redundant PRIVATE links are dropped or kept harmlessly. The
`sdk_conformance_*` tests are the acceptance gate that all three consumption modes still
resolve it.

## Plans

| # | Plan | Summary | Status |
|---|------|---------|--------|
| 00 | The engine JSON serializer + enum-name core | nlohmann → PUBLIC on `veng` (build tree, SDK export/install, include_hygiene); `Veng/Reflection/JsonSerialize.h` (`JsonReadFields`/`JsonWriteFields`, all FieldClasses, the AssetHandle/Reference policy hooks, dotted-path errors); the runtime-typed `EnumeratorName`/`ParseEnumValue`/`LoadEnumBits`/`StoreEnumBits` in `EnumName.h`. Enums by name from day one. Unit-tested round-trip over a reflected fixture type. | proposed |
| 01 | Cooker adoption: prefab + level | `PrefabImporter` and `LevelImporter` drop their `BindField`s for the shared walker (pack-resolve validation and entity-index remap as hooks; level config gains Enum/Variant/Array for free). Hard-cut enum strings; migrate the prefab JSON assets (both examples, cooker fixtures). Depends on 00. | proposed |
| 02 | Editor + MCP adoption | `PrefabSerialize` (write inverse via the walker, live-entity `WriteReference` hook) and MCP's `ReflectToJson` (both directions) move onto the shared walker; MCP enum output becomes the bare name string. Depends on 00. | proposed |
| 03 | Graph enums + the spelling migrations | `NodeGraphSerialize` enum properties by name (+ both sample graphs); `MaterialDomain` `VE_ENUM` + `"Surface"`/`"PostProcess"` across importer/codegen/editor + every `.vmat`; `AssetType` manifest spellings + every `.vengpack.json`; the `"compression"` escape hatch → `TextureCodec` spellings + fixtures. Depends on 00 (independent of 01/02). | proposed |
| 04 | Docs, template co-migration check, roadmap pass | Root/engine/cooker/editor/mcp `CLAUDE.md` updates (nlohmann PUBLIC, the walker, the enum convention); `docs/guides/` pass (consuming-veng's dependency list, asset-format examples); the full verification band incl. SDK conformance; planset status. The closer. Depends on 00–03. | proposed |

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = implemented, migrated, verified, committed.

## Dependencies

- **00 → {01, 02, 03} → 04.** Plan 00 is the foundation; 01, 02, and 03 are independent of
  each other (different files, different assets) and can run in parallel; 04 closes.
- The asset migrations ride the plan that hard-cuts their reader (01: prefab JSONs; 03:
  graphs, `.vmat`s, pack manifests, `.tex.json`s) — **both examples co-migrate in the same
  plan as the breaking change**, per the working norms; the template's breakage surfaces in
  `sdk_conformance_*`, not the in-tree build.
- Builds on the reflection layer (`VE_ENUM` enumerator tables, `FieldClass`, the binary
  `Serialize.h` precedent) and touches no cooked binary format — every change here is source
  JSON + serializer code; blobs, TOCs, and the reflection wire encoding are untouched.

## The decisions this planset settles

- **The walker lives in `libveng`, and nlohmann/json goes PUBLIC.** Considered homes: the
  cooker (rejected — `libveng_editor` links only `veng`+`graph` and `veng_mcp` only `veng`;
  a cooker home would drag assimp/Slang/texture-encoder link deps into the editor framework
  and into a lib shipped games link), a new tiny shared lib (workable, the `veng::graph`
  precedent, but another target for ~one header), and the engine itself. **Decided: the
  engine** — the walker sits beside the binary serializer it mirrors, every consumer already
  links `veng`, and the cost is nlohmann joining the PUBLIC dep set. The runtime still never
  *parses* JSON in the load path — cooked blobs stay binary; this is a library-surface
  change, not a loader change.
- **Hard cut, no integer fallback.** Readers accept enumerator names only; every JSON asset
  in the tree migrates in the same plan as its reader. A leftover integer is a loud, located
  error naming the field. No dual-form tolerance to carry forever.
- **All three legacy spellings migrate.** `"domain"`, the pack-manifest `"type"`, and the
  raw `"compression"` codec all move to exact C++ enumerator spellings — full consistency,
  no "established keyword vocabulary" carve-out.
- **MCP emits bare enumerator names.** The `{ value, name }` object shape goes; read and
  write are the one convention. Agent-facing output changes shape — noted in `mcp/CLAUDE.md`.
- **Exact, case-sensitive matching; write-side fallback stays.** `EnumeratorName`'s
  documented decimal-string fallback for an out-of-range value remains (a corrupt value stays
  readable); `ParseEnumValue` rejects it on read, which is the correct loud failure.

## What remains future

- **`Renderer::Format` and the vertex-layout name table.** `VertexLayoutSource.cpp`
  hand-matches format names that already match the C++ spellings; folding the giant
  `Renderer::Format` enum into `VE_ENUM` is churn with no current payoff. Revisit if a
  second consumer needs the table.
- **Schema-versioned JSON migrations.** The hard cut is a one-time manual migration; a
  general versioned-migration mechanism for source JSON is deliberately not built.
- **A JSON Schema / editor-completion surface** generated from the reflection tables — a
  natural later consumer of the unified walker, not part of this pass.
