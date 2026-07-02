# Plan 01 — the two reflective walkers

**Goal:** the two generic walkers that (de)serialize `AssetHandle` fields read and write the
hex-string form. This is the highest-leverage plan: the engine's `JsonSerialize` walker covers
**every** prefab/level component `AssetHandle` field, and the graph walker covers **every**
node-graph asset property — so two small edits convert the entire reflected asset-reference
surface. Depends on Plan 00.

## The starting point

- [JsonSerialize.cpp](../../engine/src/Reflection/JsonSerialize.cpp) — the shared JSON⇄reflection
  walker (planset-43). `ReadAssetHandle` (line ~125) requires `is_number_unsigned()` and reads
  `get<u64>()`; on the **read** side `null`/absent means "no asset" (id 0). `WriteAssetHandle`
  (line ~406) today returns the raw `u64` as a JSON number — **including id 0 as the number `0`;
  it never emits `null`.**
- [NodeGraphSerialize.cpp](../../graph/src/NodeGraphSerialize.cpp) — the node-property walker.
  Write (the `case FieldClass::AssetHandle`, ~line 171): returns the `u64` id as a number, `null`
  for 0. Read (~line 252): `value.is_number_unsigned() ? value.get<Veng::u64>() : 0`.

The read sides treat `null`/absent as id 0; `JsonSerialize`'s write side does **not** emit `null`
today. This plan makes that write side emit `null` for id 0 (matching the read side) and the hex
string otherwise — a deliberate, small behavior change, not a preservation.

## What lands

### 1. `JsonSerialize.cpp` — `ReadAssetHandle` / `WriteAssetHandle`

- **Read:** replace the `is_number_unsigned()` guard with an `is_string()` guard, and the
  `get<u64>()` with `ParseAssetId(value.get<string>())`. A non-string, or a string
  `ParseAssetId` rejects, becomes the dotted-path error the walker already formats
  (`"{}: expected a hex-string AssetId"`). `null` still stores id 0 and returns. The existing
  `hooks.ValidateAssetId` call on a nonzero id is unchanged.
- **Write:** replace `return id;` with `return id == 0 ? Json(nullptr) : Json(FormatAssetId(AssetId{id}));`
  — id 0 now serializes as `null` (it previously serialized as the number `0`), nonzero as the
  hex string. This is the intended behavior change, matching `null`-for-invalid to the read side.
- `#include <Veng/Asset/HexId.h>`.

### 2. `NodeGraphSerialize.cpp` — the `AssetHandle` case

- **Write** (the `case FieldClass::AssetHandle`): keep the `id == 0 → null`, change the
  nonzero return from the numeric `id` to `FormatAssetId(AssetId{id})`.
- **Read:** replace `value.is_number_unsigned() ? value.get<Veng::u64>() : 0` with: `null`/absent
  → 0; a string → `ParseAssetId(...).value_or(0)`. Keep the file's tolerant posture (a malformed
  property reads as the zero default, no error) — concretely, mirror the `case FieldClass::Enum`
  read arm a few cases up, which falls through to the zero-initialized default on an unmatched
  value. Do **not** make this stricter than that sibling.
- `#include <Veng/Asset/HexId.h>`.

### 3. `mcp/src/ReflectToJson.cpp` — delete the decimal-string re-encoding layer

This is walker-adjacent, not editor code, and it **breaks the moment section 1 lands** — so it
belongs here, not in Plan 03. The MCP library wraps the shared walker with `ConvertAssetHandles` /
`ConvertAssetHandleValue` ([ReflectToJson.cpp:50-152](../../mcp/src/ReflectToJson.cpp)), called from
`FieldsToJson` (`toWire=true`, line ~144) and `JsonToFields` (`toWire=false`, line ~152). Today it
re-encodes every `AssetHandle` between the walker's raw JSON **number** and MCP's public **decimal
string** (`toWire`: `is_number_unsigned()` → `to_string`; fromWire: `from_chars` back to a number).
Once section 1 makes the walker speak hex strings, this layer is not merely redundant — it is
**actively wrong**:

- **Write:** `toWire`'s `is_number_unsigned()` branch never fires (the walker now emits a string),
  so the wire form silently becomes the hex string with no conversion — a quiet contract change.
- **Read:** fromWire parses the agent's string to a JSON **number** and hands it to `JsonReadFields`,
  which after section 1 **rejects numbers** — breaking every `entity.set_field` /
  `entity.add_component` / `world.load_prefab` carrying an `AssetHandle`, and reddening
  `mcp_mutation` / `mcp_world` / `editor_mcp_conformance`.

**Delete `ConvertAssetHandles` and `ConvertAssetHandleValue` entirely**; have `FieldsToJson` return
`JsonWriteFields(...)` directly and `JsonToFields` pass `source` straight to `JsonReadFields`. MCP's
wire form for an `AssetHandle` becomes the same canonical hex string as everywhere else — no
per-library re-encoder. In the same plan:

- Update [mcp/CLAUDE.md](../../mcp/CLAUDE.md): the "`AssetHandle` → the referenced `AssetId` as a
  decimal string" prose (in "Reflection is the (de)serializer") becomes the canonical hex string.
- Update any `mcp_mutation` / `mcp_world` fixture or inline JSON that feeds a decimal-string id to
  the hex string (test code accompanying the reader change), so the `mcp` suite stays green.

No `#include <Veng/Asset/HexId.h>` is needed here — the layer is removed, not reimplemented.

### A known gap left out: reflected `ActionId` scalar fields

`ActionId` is a reflected `FieldClass::Scalar` leaf ([Actions.h:229](../../engine/include/Veng/Input/Actions.h))
used by `InputAction.Id`, `Binding.Action`, and `ActionSample.Id` (the last inside the `PlayerInput`
component). A reflected `ActionId` field therefore serializes through the walker's **scalar** path
(`ReadScalar`/`WriteScalar`, keyed on the field's `TypeId`), **not** the `AssetHandle` path this plan
converts. **This is deliberately left numeric here** because no checked-in asset populates one today
(`player.prefab.json`'s `PlayerInput` is empty), so nothing is broken and no fixture migrates. It is
called out so the gap is on the record: the day a reflected `ActionId`/`SystemId` scalar field is
actually persisted, it needs a scalar-path case routing `TypeIdOf<ActionId>()` through
`FormatHexId`/`ParseHexId` (distinct from the plain-`u64` numeric branch) — a small, separate change,
not silent breakage. The inputmap's own `id`/`action` fields are a *different*, hand-parsed file
format and **are** covered (Plan 02/03).

## Migration note

The **fixtures and sample graphs** these walkers read are migrated in Plan 04, not here — this
plan changes only the reader/writer code. But the two directions must stay mutually consistent
after this plan so a round-trip test passes: if any unit test in `tests/unit/json_serialize.cpp`
or `tests/editor/node_catalog_serialize.cpp` constructs an inline JSON document with a numeric
`AssetHandle` value, update that inline literal to the hex string in this plan (it is test code,
not a checked-in asset), so the suite stays green between 01 and 04.

`tests/editor/node_catalog_serialize.cpp`'s `ReadAssetIdProperty` helper reads the id back as a
`u64` for comparison — that reads the *deserialized* field bytes (offset 0 of the handle), not
the JSON, so it is unaffected; only any inline *input* JSON it builds needs the string form.

## Verification

- `build-debug` clean; `ctest` green — specifically `json_serialize` (the round-trip over a
  reflected fixture with an `AssetHandle` field), `node_catalog_serialize` (the graph
  property round-trip), and the `mcp_*` suites (`mcp_mutation` / `mcp_world`) now that the
  `ReflectToJson` layer is removed and its fixtures speak hex.
- Spot check: a scratch prefab component JSON with a numeric handle value now fails to load
  with the dotted-path error; the same value as a hex string loads.

## Out of scope

- The cooker's non-walker importers and the manifest (Plan 02) — those hand-read ids outside
  these two walkers.
- The editor write panels and `editor/src/EditorMcp.cpp`'s own tool-argument parsing (Plan 03) —
  only the `mcp/src` walker-wrapping layer moves here.
- Migrating checked-in `.prefab.json` / `.frag.graph.json` assets and fixtures (Plan 04).
