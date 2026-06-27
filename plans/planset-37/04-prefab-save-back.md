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
  components via `Scene::ForEachComponent` → the type's `FieldDescriptor` walk, mapping each
  reflected field to JSON by `FieldClass` — the inverse of the cooker's `PrefabImporter` read and
  symmetric with the reflection serializer's name-keyed `WriteFields`. Asset-handle fields write
  their `AssetId` (decimal, JSON convention); `Reference` fields write the intra-prefab entity
  index (the same remapping `SpawnInto` reverses on load); `Variant` writes the
  `{ "type": <qualified name>, "value": {…} }` form the cooker parses; `Array` writes a JSON
  array. A field at its default value may be omitted (the loader's schema tolerance fills it),
  matching how a hand-authored source reads.

- **Preserve-unknown-keys round-trip.** Save **patches** the existing `.prefab.json` rather than
  regenerating it: entity/component/field keys the writer understands are rewritten, and any key
  it does not (comments-as-keys, future fields, hand-authored extras) is preserved in place —
  the same nlohmann patch-and-preserve the texture (`*.tex.json`), material (`*.vmat.json`), and
  level (`*.level.json`) editors already do. The writer lives in the editor exe (its own
  nlohmann), keeping `libveng`/`libveng_editor` JSON-free.

- **The save command + dirty flag.** `PrefabEditorPanel` gains `Save()` (Ctrl+S, and a
  File/document menu item) writing the active document's source path; it consumes Plan 03's
  dirty signal (non-empty-since-save) to enable the action and an unsaved-changes marker in the
  title, and clears it on a successful write. Saving does **not** recook — the existing
  cook-on-demand / hot-reload loop re-reads the changed source on its own debounce, the way the
  other editors already behave (save persists source; preview rides the recook).

- **Hierarchy + ordering fidelity.** The writer emits children under their parent in
  `ForEachChild` order so a save→cook→spawn round-trip reproduces the authored hierarchy and
  sibling order exactly (the intrusive `Hierarchy` links are derived, so only `Parent`/order are
  persisted, as today).

- **A round-trip test.** A `cooker`/`unit`-band test (no ICD) builds a `Scene` (or loads a fixture
  prefab and spawns it), writes it back, and asserts the re-read source cooks to an equivalent
  blob — and that an injected unknown key survives the write. This pins the save↔cook symmetry.

## Decisions

1. **Save writes `.prefab.json`, never a cooked scene.** A `Scene` is runtime-only; the authored
   document is the prefab. There is no scene asset type and none is introduced (the roadmap is
   corrected to say so).
2. **Reflection-driven writer, symmetric with the read path.** The same `FieldDescriptor` walk
   the cooker reads and the serializer records drives the write, so a new `FieldClass` or
   component is covered without touching the writer — and save↔cook stay symmetric by
   construction.
3. **Patch-and-preserve, in the editor's nlohmann.** Reuse the established preserve-unknown-keys
   round-trip so hand-authored content is never destroyed; keep JSON out of `libveng`/
   `libveng_editor` by writing in the exe, exactly as the other asset editors do.
4. **Save persists source; it does not recook.** Consistent with every other editor — the
   cook-on-demand loop owns the recook/hot-reload; save's job is to write the source. The dirty
   flag is the undo stack's "changed since save," so it is correct after undo/redo too.
5. **Stable, hierarchy-ordered entity emission.** Persist parent + sibling order (the derived
   links are rebuilt on spawn), so a round-trip is faithful and diffs are stable.

## Files

| File | Change |
|---|---|
| `editor/src/PrefabSerialize.{h,cpp}` (new) | The `Scene` → JSON writer (reflection walk per component) + the patch-and-preserve merge into the existing `.prefab.json`. |
| `editor/src/panels/PrefabEditorPanel.{h,cpp}` | `Save()` (Ctrl+S + menu); consume Plan 03's dirty flag; unsaved marker in the title; clear on write. |
| `editor/src/EditorHost.{h,cpp}` | File-menu Save + the shortcut, forwarded to the focused document. |
| `tests/cooker/…` | A save→cook round-trip equivalence test + an unknown-key-preservation test (no ICD). |

## Verification

- Clean build; `ctest` green — the new save→cook round-trip test passes (no ICD), the existing
  prefab cook/spawn tests still pass.
- Editor-run check: edit a prefab (move via gizmo, edit fields, reparent, add a component), Save,
  reopen the document → the edits are present; an unknown key hand-added to the `.prefab.json` is
  still there after a save; the unsaved marker appears on edit and clears on save; undo before
  save returns the dirty flag to clean.
- `smoke_golden` does **not** move.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
