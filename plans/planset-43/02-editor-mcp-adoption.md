# Plan 02 — editor + MCP adoption

**Goal:** the remaining three forks fold in. The editor's `PrefabSerialize` and
`LevelEditorPanel`'s config round-trip write (and, where they read, read) through the shared
walker, so a saved prefab/level authors enums by name — the write inverse of Plan 01's
readers. MCP's `ReflectToJson` becomes a thin wrapper over the walker, and its enum output
changes shape to the bare enumerator name. Depends on Plan 00; **sequenced after Plan 01**
(the editor must write what the cooker now reads — see the README Dependencies section).

## The starting point

- [PrefabSerialize.cpp](../../editor/src/PrefabSerialize.cpp) (~450 lines): the editor's
  scene→JSON writer — enum as integer ("the inverse of BindField's low-byte enum write"),
  live-`Entity`→prefab-local-index mapping, `AssetHandle` as the raw id. **It is a
  merge-writer:** it reads the existing source first and writes reflected fields *into* it,
  preserving unknown keys (comments-as-keys, hand-authored structure, future fields).
- [LevelEditorPanel.cpp](../../editor/src/panels/LevelEditorPanel.cpp) `ReadConfigObject`/
  `WriteConfigObject` (~150 lines): the editor's level-config round-trip, "mirroring the
  cooker's LevelImporter read … so the editor writes exactly what the cooker reads." Covers
  Scalar/Vector/AssetHandle/Struct only — **no `Enum` arm** — and is likewise a merge-writer
  (reads the existing file, preserves unknown keys such as the world id) with a tolerant read
  that leaves unknown source keys alone.
- [ReflectToJson.cpp](../../mcp/src/ReflectToJson.cpp) (~620 lines): both directions for the
  MCP inspect/mutate tools — enum written as `{ "value": 1, "name": "Local" }`, read
  tolerantly (object, bare name, or integer); its own hand-rolled enumerator loop.

## What lands

### 1. `PrefabSerialize` on the shared walker

- The per-FieldClass write ladder is deleted; a component serializes via the **merge-write**
  overload `JsonWriteFields(existingComponentJson, componentPtr, typeInfo, registry, hooks)`
  with `WriteReference` = the existing live-entity→prefab-local-index mapping. The merge form
  is required, not the fresh-object one — the writer must preserve the unknown keys it reads
  from source (Plan 00 grew this overload for exactly this). Enums come out as enumerator
  names, matching what Plan 01's cooker now requires.
- Any editor-side read of prefab source JSON in the same TU moves onto `JsonReadFields` with
  the inverse hook and `allowUnknownFields = true` (the editor's tolerant posture).
- **Round-trip acceptance:** open a migrated example prefab in the editor, save it, and
  re-cook — the save must be a no-op diff apart from formatting and the fields that changed
  (names in, names out; hand-authored/unknown keys preserved).

### 1b. `LevelEditorPanel`'s config round-trip on the shared walker

- `ReadConfigObject`/`WriteConfigObject` are deleted; the level-config records bind and emit
  through `JsonReadFields` (`allowUnknownFields = true`) and the merge-write `JsonWriteFields`,
  the same pair `LevelImporter` (Plan 01) and `PrefabSerialize` use. This **gains the `Enum`
  arm the panel never had**, so an enum `LevelRenderSettings` knob is now editable and its
  save authors the enumerator name — closing the "fixes that for free" claim on the editor
  side, not just the cook side.
- The panel's load-bearing comment ("mirroring the cooker's LevelImporter read … writes
  exactly what the cooker reads") is updated to point at the shared walker rather than a
  hand-mirrored copy.

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
  "Directional"` and the recook/hot-reload loop stays live. Save a level through the level
  panel and confirm its unknown keys (the world id, hand-authored structure) survive the
  round-trip untouched.

## Out of scope

- `NodeGraphSerialize` and the spelling migrations (Plan 03) — the graph serializer is not a
  fork of this walker (fixed-capacity pin property records, not offset-based structs) and is
  handled there.
