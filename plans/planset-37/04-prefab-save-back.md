# Plan 04 — prefab save-back (`Scene` → `.prefab.json`)

**Goal:** make the prefab editor **authoritative** — write the edited live `Scene` back to its
`.prefab.json` source. A reflection-driven `Scene` → JSON writer emits every entity + component +
field through the name-keyed reflection walk, round-tripping the **preserve-unknown-keys** way
(the texture/material/level editor idiom) so hand-authored fields and formatting survive. Save
(Ctrl+S) writes the active document's source and clears the dirty flag. **Depends on Plan 03**
(the dirty flag follows the undo stack; save persists whatever the prior plans produced). A
`Scene` is runtime-only — this writes a **`.prefab.json`**, never a cooked scene asset.

## Why it is its own plan

Save-back is the reverse of the cook's read path and the meaty net-new piece: the cooker reads
`.prefab.json` (`PrefabImporter`) and `Prefab::SpawnInto` reads the cooked blob, but nothing
serializes a *live* `Scene` to JSON source. It is cleanly separable — it consumes the selection /
mutation / undo machinery the prior plans built and adds only the writer + the save command — and
it is the last step because it persists their combined result.

## What lands

- **A reflection-driven `Scene` → JSON writer (editor-side).** Walks the document's entities (the
  spawned roots and their `Hierarchy` subtrees, in a stable order), and for each entity emits its
  stable id key (below) plus its components via `Scene::ForEachComponent` → the type's
  `FieldDescriptor` walk, mapping each reflected field to JSON by `FieldClass`. Each `FieldClass`
  arm is the **exact inverse** of the cooker's `PrefabImporter::BindField` read arm: asset-handle
  fields write their `AssetId` (decimal, JSON convention); `Reference` fields write the intra-prefab
  entity index (the same remapping `SpawnInto` reverses on load); `Variant` writes the
  `{ "type": <qualified name>, "value": {…} }` form the cooker parses; `Array` writes a JSON
  array. Because the read side lives cooker-private in `libveng_cook` and this writer lives
  editor-private, the symmetry is **hand-matched, not structural** — so the writer is validated
  arm-by-arm against `BindField`'s `FieldClass` switch (every arm enumerated), and the option of
  hoisting the per-`FieldClass` JSON mapping into one shared location both link is weighed. A field
  at its default value may be omitted (the loader's schema tolerance fills it), matching how a
  hand-authored source reads.

- **A stable per-entity id key for round-trip alignment.** `.prefab.json` `entities` is a
  *positional* array with no per-entity identity, so a naive patch cannot tell which source object
  a live entity corresponds to once entities are added, deleted, or reordered — it would either
  regenerate the array (dropping every entity's hand-authored extras) or misattribute them by
  position (and silently rewrite every index-based `Reference`). So the schema gains a **stable,
  optional per-entity id key**: the writer matches a live entity to its source object by id and
  patches that object in place; a live entity with no source match (newly created) is appended with
  a fresh id; a source object with no live match (deleted) is dropped. New entities (the explorer's
  create/duplicate) mint an id at creation. The key is additive — the cooker reads and preserves
  it, and an absent id falls back to positional order, so existing sources still load.

- **Preserve-unknown-keys round-trip.** Within each entity, save **patches** rather than
  regenerating: component/field keys the writer understands are rewritten, and any key it does not
  (comments-as-keys, future fields, hand-authored extras) is preserved in place — the same nlohmann
  patch-and-preserve the texture (`*.tex.json`), material (`*.vmat.json`), and level
  (`*.level.json`) editors already do, now keyed per entity rather than over flat top-level keys.
  The writer lives in the editor exe (its own nlohmann), keeping `libveng`/`libveng_editor`
  JSON-free.

- **The save command + dirty flag.** `PrefabEditorPanel` gains `Save()` (Ctrl+S, and a
  File/document menu item) writing the active document's source path; it consumes Plan 03's
  dirty signal (non-empty-since-save) to enable the action and an unsaved-changes marker in the
  title, and clears it on a successful write. The write is **atomic** — serialize to a temp sibling
  then `rename` over the target (atomic on the same filesystem) — so a failed or interrupted write
  never truncates the only copy of hand-authored work (the prevailing editor idiom truncates the
  target in place, acceptable for a regenerated `*.vmat` but not for irreplaceable prefab source).
  Saving does **not** recook — the existing cook-on-demand / hot-reload loop re-reads the changed
  source on its own debounce, the way the other editors already behave (save persists source;
  preview rides the recook).

- **The level editor saves through the same writer.** `LevelEditorPanel` derives from
  `PrefabEditorPanel` and edits the level's referenced world prefab through the inherited
  viewport/explorer/inspector, so its entity edits (pick/gizmo/undo from Plans 01–03 light up there
  for free) persist through this same `Save()` into the referenced world `.prefab.json`. The level
  editor's own `SaveConfig()` (the `*.level.json` systems/config round-trip) is unchanged and
  orthogonal — entity edits go to the prefab, level config to the level file.

- **Hierarchy + ordering fidelity.** The writer emits children under their parent in
  `ForEachChild` order so a save→cook→spawn round-trip reproduces the authored hierarchy and
  sibling order exactly (the intrusive `Hierarchy` links are derived, so only `Parent`/order are
  persisted, as today).

- **A round-trip test.** A `cooker`/`unit`-band test (no ICD) builds a `Scene` (or loads a fixture
  prefab and spawns it), writes it back, and asserts the re-read source cooks to an equivalent
  blob. The fixture exercises **every `FieldClass`** (scalar / vector / variant / array / asset
  handle / reference) so each writer arm is checked against the cook read, not just the trivial
  case; and it covers the structural cases that break naive patching — an **injected unknown key
  survives** a write, and an entity **delete + reorder** re-aligns by id (preserving each surviving
  entity's unknown keys and keeping its references correct). This pins the save↔cook symmetry and
  the id-keyed alignment.

## Decisions

1. **Save writes `.prefab.json`, never a cooked scene.** A `Scene` is runtime-only; the authored
   document is the prefab. There is no scene asset type and none is introduced (the roadmap is
   corrected to say so).
2. **Reflection-driven writer, validated against the read path.** The same `FieldDescriptor` walk
   the cooker reads drives the write, so a new component is covered without touching the writer.
   But the per-`FieldClass` JSON mapping is mirrored across two libraries (cooker-private read,
   editor-private write), so symmetry is hand-matched, not automatic — every arm is validated
   against `PrefabImporter::BindField` and pinned by the all-`FieldClass` round-trip test, and
   hoisting the mapping into one shared location is the durable fix if drift bites.
3. **Patch-and-preserve, keyed by a stable entity id, written atomically.** Reuse the established
   preserve-unknown-keys round-trip so hand-authored content is never destroyed — but key it to a
   stable per-entity id (additive to the schema) so add/delete/reorder re-aligns rather than
   clobbers, since `.prefab.json` entities are a positional array. Write to a temp file + `rename`
   so a failed save never corrupts the source. Keep JSON out of `libveng`/`libveng_editor` by
   writing in the exe, exactly as the other asset editors do.
4. **Save persists source; it does not recook.** Consistent with every other editor — the
   cook-on-demand loop owns the recook/hot-reload; save's job is to write the source. The dirty
   flag is the undo stack's "changed since save," so it is correct after undo/redo too.
5. **Stable, hierarchy-ordered entity emission.** Persist parent + sibling order (the derived
   links are rebuilt on spawn), so a round-trip is faithful and diffs are stable.

## Files

| File | Change |
|---|---|
| `editor/src/PrefabSerialize.{h,cpp}` (new) | The `Scene` → JSON writer (reflection walk per component, each `FieldClass` arm the inverse of `PrefabImporter::BindField`) + the id-keyed patch-and-preserve merge + the atomic temp-then-`rename` write. |
| `editor/src/panels/PrefabEditorPanel.{h,cpp}` | `Save()` (Ctrl+S + menu); consume Plan 03's dirty flag; unsaved marker in the title; clear on write. Inherited by `LevelEditorPanel`, so a level's entity edits save through it into the referenced world `.prefab.json`. |
| `cooker/src/Importers/PrefabImporter.cpp` | Read + preserve the stable per-entity id key (absent → positional fallback). |
| `editor/src/EditorHost.{h,cpp}` | File-menu Save + the shortcut, dispatched to the focused document (the Plan 03 focus seam). |
| `tests/cooker/…` | A save→cook round-trip equivalence test covering every `FieldClass`, an unknown-key-preservation test, and a delete+reorder id-alignment test (no ICD). |

## Verification

- Clean build; `ctest` green — the new save→cook round-trip tests pass (no ICD), the existing
  prefab cook/spawn tests still pass.
- Editor-run check: edit a prefab (move via gizmo, edit fields, reparent, add a component), Save,
  reopen the document → the edits are present; an unknown key hand-added to the `.prefab.json` is
  still there after a save, including after deleting and reordering other entities; the unsaved
  marker appears on edit and clears on save; undo before save returns the dirty flag to clean. The
  same editing + Save in the level editor persists entity edits into the referenced world
  `.prefab.json`.
- `smoke_golden` does **not** move.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
