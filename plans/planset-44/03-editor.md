# Plan 03 — editor writers + readers

**Goal:** the editor is the *authoring* side — its writes are what put the new hex-string form
on disk, and its reads must accept it. Every panel that serializes an id and every editor path
that parses one converts through the Plan 00 codec. Depends on Plan 00; sequenced after Plans
01–02 (it shares the walker and the cooker-side conventions).

## Write sites (produce the new on-disk form)

Each currently assigns a `.Value` (a JSON number); change to `FormatAssetId(...)` /
`FormatHexId(...)`:

- [ProjectSettingsPanel.cpp:253](../../editor/src/panels/ProjectSettingsPanel.cpp) —
  `project["startupLevel"] = m_Settings.StartupLevel.Value` → `FormatAssetId(...)`.
- [MaterialEditorPanel.cpp:345,378](../../editor/src/panels/MaterialEditorPanel.cpp) —
  `doc["defaultInstance"] = m_DefaultInstanceId.Value` (two sites) → `FormatAssetId(...)`.
- [MaterialInstanceEditorPanel.cpp:220,233](../../editor/src/panels/MaterialInstanceEditorPanel.cpp) —
  `doc["parent"] = m_ParentId.Value` and `overrides[slot.Name] = slot.Texture.Value` →
  `FormatAssetId(...)`.
- [InputMappingEditorPanel.cpp:171,187](../../editor/src/panels/InputMappingEditorPanel.cpp) —
  `entry["id"] = static_cast<u64>(action.Id)` and `entry["action"] = static_cast<u64>(binding.Action)`
  → `FormatHexId(static_cast<u64>(...))` (**`ActionId`**).
- [LevelEditorPanel.cpp:175](../../editor/src/panels/LevelEditorPanel.cpp) — `SaveConfig` writes
  each `SystemId` numerically (`systems.push_back(sysId)`, `m_Systems` is `vector<SystemId>`) →
  `FormatHexId(static_cast<u64>(sysId))` (**`SystemId`**). Easy to miss: `LevelEditorPanel` is
  neither a material nor an inputmap panel, but it owns the level `systems[]` array.

## Read sites (accept the new form)

Each currently guards `is_number_unsigned()` + `get<u64>()`; convert to `is_string()` +
`ParseAssetId`/`ParseHexId`, preserving the existing absent/invalid fallback:

- [EditorHost.cpp:167](../../editor/src/EditorHost.cpp) — `startupLevel`.
- [AssetSourceIndex.cpp:37,50](../../editor/src/AssetSourceIndex.cpp) — the manifest `id`
  (mirrors the cooker's `Cooker.cpp` manifest read).
- [MaterialEditorPanel.cpp:94-96](../../editor/src/panels/MaterialEditorPanel.cpp) — the
  constructor's `defaultInstance` read, **and** the `shaders.vertex`/`shaders.fragment` reads at
  lines ~170-176. This panel is also a *write* site (below), which is exactly why its reads are
  easy to skip — but if they stay numeric, opening a migrated `.vmat.json` reads them blank and
  the mint-backfill (lines ~390-399) **mints a fresh duplicate `defaultInstance` id** on the next
  save. Convert all three to `is_string()` + `ParseAssetId`.
- [LevelEditorPanel.cpp:140](../../editor/src/panels/LevelEditorPanel.cpp) — `LoadConfig` reads
  each `systems[]` entry with `is_number_unsigned()` + `get<u64>()`. Left numeric, an opened
  migrated level loads an **empty** systems list and `SaveConfig` then writes it back — silently
  destroying the authored `systems` array. Convert to `is_string()` + `ParseHexId` → `SystemId`.
- [PrefabSerialize.cpp:220](../../editor/src/PrefabSerialize.cpp) — this is the entity **local
  id** (`EntityIdKey`), an index, **not** an `AssetId` — **leave numeric**. (Called out
  explicitly so the sweep does not convert it. The component `AssetHandle` fields
  `PrefabSerialize` round-trips go through the Plan 01 `JsonSerialize` walker.)
- [MaterialInstanceEditorPanel.cpp:89,100,105](../../editor/src/panels/MaterialInstanceEditorPanel.cpp) —
  `parent` and texture overrides (bare-value and `{ "id": … }` object forms).
- [InputMappingEditorPanel.cpp:95](../../editor/src/panels/InputMappingEditorPanel.cpp) — action
  `id` (and the binding `action` read, if separate) — **`ActionId`**.
- [EditorMcp.cpp](../../editor/src/EditorMcp.cpp) — the editor's MCP tools, two distinct edits:
  - **The input side.** `EditorMcp.cpp` already defines its **own** file-local
    `ParseAssetId(const Json&)` helper (line ~141), which is *not* the codec's
    `Veng::ParseAssetId(string_view)` — same name, different signature, both visible once
    `<Veng/Asset/HexId.h>` is included. Rewrite that helper's **body** to guard `is_string()` and
    delegate to the codec, **deleting** its base-10 `from_chars` + `is_number_unsigned()` numeric
    fallback (the hard cut — no numeric acceptance), and update its stale doc comment ("decimal
    string or number" → "canonical hex string"). Do not rename or duplicate it; its call sites
    (lines ~454/615/687) are unchanged.
  - **The output side.** Convert **all four** `to_string(id->Value)` output sites to
    `FormatAssetId` — lines ~392 (`editor.list_assets` rows, the tool an agent hits first to
    discover ids), ~461, ~629, ~701 — so an agent always gets back the same hex token it passes
    in (round-trip symmetry, not just input tolerance). Record the shape change in editor docs
    (Plan 04); the `mcp/CLAUDE.md` prose is handled in Plan 01 with the `ReflectToJson` deletion.

`FieldWidget.cpp` (line ~70) and `AssetChip.cpp` / `AssetBrowserPanel.cpp` compare `.Value`
integers in-memory for selection — **not serialization** — and are untouched.

## Includes

`#include <Veng/Asset/HexId.h>` in each touched `.cpp`. The editor links `libveng` and
`assetpack`, so the header resolves.

## Migration note

The editor has no headless smoke/PPM path; its conversion is verified by the editor unit suite
(`tests/editor/`) and by the panels round-tripping. Any inline JSON in `tests/editor/` that
feeds a numeric id into a migrated read path updates to the hex string in this plan (test code,
not a checked-in asset). Checked-in editor fixtures move in Plan 04.

## Verification

- `build-debug` clean; `ctest` green for the `editor` suite once Plan 04's fixtures land. At
  this plan's boundary, the tree compiles `-Werror`.
- Manual round-trip: open an authored `.vmat.json` / `.vmatinst.json` / `.level.json` /
  `project.veng` / `.inputmap.json` in the editor, save, and confirm the written file carries
  `"0x…"` hex strings and re-opens losslessly. Cover **every** panel that reads an id, not just
  the three write formats — the silent-blanking failures (material `defaultInstance`/`shaders`,
  level `systems[]`) only show up when you re-open a *migrated* asset in its panel.

## Out of scope

- The reflective walkers (Plan 01) and the cooker importers (Plan 02).
- The full asset/fixture/doc/C++-literal migration and the verification band (Plan 04).
