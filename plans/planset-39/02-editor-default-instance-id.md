# Plan 02 — the editor generates the `defaultInstance` id on material create/save

**Goal:** make the `defaultInstance` id Plan 01 requires *automatic* for editor-authored materials.
Plan 01 leaves the id hand-minted (`vengc generate-id` → pasted into the `*.vmat.json`) and the
cooker **errors** if a referenced material lacks one. That hand-mint stays the floor for hand-authored
content, but a material created or edited through the material editor should never need it: the editor
mints the id and writes it into the `.vmat.json` itself. Depends on Plan 01 (the `defaultInstance` key
and the cook-time requirement it introduces).

## The starting point (what Plan 01 leaves)

- A parent `*.vmat.json` carries an optional **`defaultInstance`** key — a minted `AssetId`, decimal
  in the pack JSON — and the cook errors (located `Result`) when a material referenced as an instance
  declares none.
- The material editor (`MaterialEditorPanel`, `editor/`) already round-trips a `.vmat.json`
  **preserving unknown keys** (planset-14's preserve-unknown-keys save path, reused by planset-15's
  graph embed under `"_editor"`), so adding a top-level key on save is a solved mechanism.
- The editor exe links `libveng_cook`, which owns the same id-generation `vengc generate-id` calls.
  The editor reaches cooking through an injected **`CookBackend`** (planset-14) — the seam an id
  generator belongs on, so `libveng_editor` stays cook-free.

## Why editor-at-authoring, not cooker-at-cook

Minting the id at **cook** time is rejected: a cooked id must be **stable and present in source**
because references name it, so a cook-time mint would either write the id back into the source during
the build (the build mutating checked-in source — the cook stops being a pure source→binary function)
or *derive* it (an invented id that lives nowhere in source, against the house "never invent an id"
rule, and one a cross-pack referrer cannot name). Generating it at **authoring** time keeps the id a
real `generate-id` mint written into source — identical in kind to every other id in the tree — and
leaves the cook a pure function. The only thing that changes versus a human pasting it is *who runs
the minter*.

## What lands

### 1. An id generator on the cook backend

- `CookBackend` gains an id-mint entry point — `AssetId GenerateAssetId(span<const path> referencePacks)`
  — wrapping the cooker's existing `generate-id` logic, taking the project's packs as `--reference`
  inputs so a freshly minted id cannot collide with an existing one.
- The editor passes the open project's pack manifests as the references, the same set the cook reads.

### 2. The material editor fills the key

- On **new-material create**, the panel mints a `defaultInstance` id and writes it into the new
  `.vmat.json` before the first save.
- On **save of a material lacking the key** (a legacy hand-authored material opened in the editor),
  the panel backfills it — mint, write through the preserve-unknown-keys round-trip — so opening and
  saving an old material is sufficient to migrate it.
- The id is surfaced **read-only** in the material inspector (it is identity, not a tunable), beside
  the material's own id.

## Files (sketch — the agent confirms against the tree)

- `editor/include/.../CookBackend.h` + its concrete impl in the editor exe — the `GenerateAssetId`
  entry point over the cooker's id generator.
- `editor/src/.../MaterialEditorPanel.*` — mint-on-create, backfill-on-save, the read-only id row.
- `cooker/` — expose the `generate-id` routine as a callable (it is a CLI command today) so the
  backend can invoke it in-process.
- Docs: `editor/CLAUDE.md` (the material editor mints the default-instance id; the hand path is the
  fallback), `cooker/CLAUDE.md` (the id routine is callable, not CLI-only).

## Examples to co-migrate

No content change required — both examples' materials already carry hand-minted `defaultInstance` ids
from Plan 01. This plan is exercised by **authoring**: creating a new material in
`hello_triangle-editor` produces a `.vmat.json` with a `defaultInstance` id and no manual step.

## Verification

- Create a new material through the editor; assert the written `.vmat.json` carries a `defaultInstance`
  id and that a subsequent cook does **not** raise Plan 01's "must declare `defaultInstance`" error.
- Open a material with the key stripped, save it, confirm the key is backfilled and unknown keys (the
  `"_editor"` graph block) survive the round-trip.
- Minted ids are unique against the reference packs (a generate-with-reference test).

## Risks

- **Collision** if the references are incomplete — the editor must pass the *full* project pack set as
  `--reference`, the same set the cook uses. A mint with no references is the failure mode to guard.
- **In-process id generation** must match the CLI `generate-id` exactly (same entropy source, same
  format); a divergence would mint ids the CLI considers malformed. Share the one routine, do not
  reimplement.
