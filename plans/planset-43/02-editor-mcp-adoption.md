# Plan 02 — editor + MCP adoption

**Goal:** the remaining two forks fold in. The editor's `PrefabSerialize` writes (and, where
it reads, reads) through the shared walker, so a saved prefab authors enums by name — the
write inverse of Plan 01's reader. MCP's `ReflectToJson` becomes a thin wrapper over the
walker, and its enum output changes shape to the bare enumerator name. Depends on Plan 00;
pairs with Plan 01 (the editor must write what the cooker reads).

## The starting point

- [PrefabSerialize.cpp](../../editor/src/PrefabSerialize.cpp) (~450 lines): the editor's
  scene→JSON writer — enum as integer ("the inverse of BindField's low-byte enum write"),
  live-`Entity`→prefab-local-index mapping, `AssetHandle` as the raw id.
- [ReflectToJson.cpp](../../mcp/src/ReflectToJson.cpp) (~620 lines): both directions for the
  MCP inspect/mutate tools — enum written as `{ "value": 1, "name": "Local" }`, read
  tolerantly (object, bare name, or integer); its own hand-rolled enumerator loop.

## What lands

### 1. `PrefabSerialize` on the shared walker

- The per-FieldClass write ladder is deleted; a component serializes via
  `JsonWriteFields(componentPtr, typeInfo, registry, hooks)` with `WriteReference` = the
  existing live-entity→prefab-local-index mapping. Enums come out as enumerator names,
  matching what Plan 01's cooker now requires.
- Any editor-side read of prefab source JSON in the same TU (the load/round-trip half, if
  present) moves onto `JsonReadFields` with the inverse hook.
- **Round-trip acceptance:** open a migrated example prefab in the editor, save it, and
  re-cook — the save must be a no-op diff apart from formatting (names in, names out).

### 2. MCP `ReflectToJson` on the shared walker

- `ValueToJson`/`ValueFromJson` and their per-class ladders reduce to `JsonWriteFields`/
  `JsonReadFields` calls plus MCP's own entity-addressing hooks; the hand-rolled
  `EnumToJson`/`EnumFromJson` are deleted.
- **Output shape change:** an enum field emits the bare enumerator name string (`"Tier":
  "Local"`), not the `{ value, name }` object. The read side follows the hard cut: an
  enumerator name only (the old object/integer tolerance goes — one convention, one code
  path). Tool descriptions that document the field encoding update with it.
- The MCP loopback test updates its expected shapes; a mutate-with-integer case now expects
  the error.

## Verification

- `build-debug` clean; `ctest` green — the editor suite, the MCP loopback suite, and the
  Plan 01 cooker suite together (the round-trip closes: editor save → cook → load).
- Manual: `veng-editor --project examples/hello-triangle/project.veng`, edit + save a
  prefab with a `Light` (enum field), confirm the JSON on disk carries `"Type":
  "Directional"` and the recook/hot-reload loop stays live.

## Out of scope

- `NodeGraphSerialize` and the spelling migrations (Plan 03) — the graph serializer is not a
  fork of this walker (fixed-capacity pin property records, not offset-based structs) and is
  handled there.
