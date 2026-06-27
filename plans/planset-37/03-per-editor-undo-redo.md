# Plan 03 — per-editor undo/redo command stack

**Goal:** a command/transaction **undo stack owned per `AssetEditorPanel`**, so each open
document undoes independently. An `EditorCommand` (`Apply`/`Revert`) captures every document
mutation — gizmo + inspector transform edits, hierarchy reparent/reorder, add/remove/reset
component, field edits — and the explorer, inspector, and gizmo route their mutations through it
instead of touching the `Scene` directly. Ctrl/Cmd+Z / Ctrl/Cmd+Shift+Z and an Edit menu drive it.
**Depends on Plan 02** (the gizmo drag is one of the commands; its commit-on-release seam is
where the transform command is pushed).

## Why it is its own plan

Undo/redo is the planset's one genuinely new subsystem and the one with a real design surface
(per-document vs. global, what is a command, how a drag coalesces). It is also pervasive — it
retrofits *every* existing mutation path in the scene-editing panels. Landing it after the
mutation sources exist (selection in 01, gizmo in 02, the inspector/explorer already shipped)
means each command type wraps a real, working edit rather than a hypothetical one. If the retrofit
proves too large for one commit, it splits cleanly along **value-edit commands**
(transform/field/component) vs. **structural commands** (reparent/reorder/create/destroy/duplicate,
which carry the subtree-snapshot and exact-handle-respawn complexity); the chain is already serial,
so a 03a/03b split costs no parallelism.

## What lands

- **`EditorCommand` + `CommandStack` (editor-side).** `EditorCommand` is an abstract
  `{ Apply(ctx), Revert(ctx), Title() }`; `CommandStack` holds an undo deque + a redo deque,
  `Push(cmd)` (applies, clears redo), `Undo()`/`Revert` the top, `Redo()` re-applies, with a depth
  cap. The stack operates over the document's `PrefabEditContext` (the `Scene*` + `AssetManager*`),
  so a command re-runs against the live document.

- **Owned per document.** `CommandStack` is a member of `AssetEditorPanel` (the base every
  document editor derives from, so every editor inherits per-instance undo), **not** a host global
  — two open prefab editors undo independently. "Document" throughout means one `AssetEditorPanel`
  instance.

- **A focused-document routing seam in the host.** `EditorHost` today has no Edit menu, no
  shortcut handling, and no active-document concept — only File→Exit and the Window panel toggles.
  This plan adds the routing: the host tracks the **focused `AssetEditorPanel`** (resolved from
  ImGui window focus / a host-held active-editor pointer) and dispatches the Edit-menu actions and
  the undo/redo shortcuts to *that* document's `CommandStack`. It is net-new infrastructure, not a
  one-line forward, and Plan 04's Save reuses the same seam.

- **The command set.** One command type per mutation, each capturing enough to revert:
  - `EditTransform { Entity, Before, After }` — the gizmo's commit-on-release (Plan 02's hook
    body becomes "push one of these over the whole drag") and the inspector's `Transform` edits.
  - `EditField { Entity, TypeId, fieldPath, beforeBytes, afterBytes }` — a generic reflected-field
    edit, captured from the inspector's `DrawFieldWidget` changed-bool by snapshotting the
    component's serialized bytes before/after (reusing the name-keyed reflection record), so it
    covers every `FieldClass` without a per-field command. Apply/Revert **deserialize** the bytes
    back through the same loader path the prefab read uses — not a raw memcpy — so an `AssetHandle`
    field re-resolves through the `AssetManager` (a stale cache handle is never reinstated) and a
    `Reference` field is remapped to the live `Entity`; and where the changed component carries a
    mesh source (`MeshRenderer`), Apply/Revert re-run `PrefabEditContext::ResolveEntity`, exactly
    as the live inspector edit does, so the derived mesh re-streams on undo/redo too.
  - `AddComponent` / `RemoveComponent` / `ResetComponent { Entity, TypeId, bytes }`.
  - `Reparent { Entity, oldParent, oldNextSibling, newParent, newNextSibling }` and
    `Reorder` — over the intrusive `Hierarchy` ops (`SetParent`/`MoveBefore`), capturing the
    sibling neighbor so order is restored exactly.
  - `CreateEntity` / `DestroyEntity { subtree snapshot }` / `DuplicateEntity` — destroy captures
    the recursive subtree (it is `O(subtree)` recursive, per the `Scene` contract) so undo
    re-spawns it; the same `ResolveEntity` rebuild a duplicate runs is re-run on undo-spawn. The
    re-spawn **restores the exact entity handle** (slot index + generation), not a freshly-recycled
    one, so that other stack entries that captured the entity (`EditTransform`/`Reparent`/`EditField`)
    and any `Reference` field pointing at it stay valid after an undo→redo cycle. If `Scene` cannot
    yet re-spawn at an exact handle, this plan adds that capability (a destroy/respawn that preserves
    the slot+generation); a fresh-handle respawn would leave dangling captures and is not acceptable.

- **The panels route through the stack.** `PrefabExplorerPanel`'s queued structural ops,
  `InspectorPanel`'s add/remove/reset + field edits, and the gizmo commit all build a command and
  `Push` it rather than mutating the `Scene` inline. The explorer already **queues** edits and
  applies them after the snapshot walk — the queue becomes "push these commands after the walk,"
  preserving the no-mutate-mid-iteration contract. `ResolveEntity` (mesh-source rebuild) is
  re-run by the relevant commands' `Apply`/`Revert`, so undo/redo of a recipe edit re-streams the
  mesh exactly as the live edit does.

- **Selection is not a command.** Selecting/deselecting is editor state, not a document mutation,
  so it never enters the stack. `Prune` still drops stale selection after a structural undo.

- **Shortcuts + menu + dirty flag.** Ctrl/Cmd+Z undo / Ctrl/Cmd+Shift+Z redo on the focused
  document (matching the Ctrl+S of Plan 04's save); an Edit menu shows the next undo/redo
  `Title()`. The stack exposes a **dirty** signal (non-empty-since-save) that Plan 04's save
  consumes and clears.

## Decisions

1. **Per-document stacks, not a global one.** Each `AssetEditorPanel` owns its history, matching
   the editor's per-instance-dockspace isolation — undo in one prefab editor never affects
   another. The focused document receives the shortcuts.
2. **A generic byte-snapshot `EditField` over per-field commands.** Capturing the component's
   serialized bytes before/after (the existing name-keyed reflection record) makes one command
   cover every `FieldClass` — scalars, vectors, variants, arrays, asset handles — without a
   command type per widget, and stays correct as new field classes land. The snapshot is applied
   by **deserializing** through the loader path (re-resolving `AssetHandle`s and remapping
   `Reference`s), never a raw memcpy, so the handle/reference classes round-trip safely; a
   resolver-bearing component (`MeshRenderer`) re-runs `ResolveEntity` on Apply/Revert.
3. **Reparent/reorder capture the sibling neighbor.** The intrusive `Hierarchy` is ordered, so
   reverting a move must restore both parent and sibling position; the command stores the old
   `NextSibling` (and parent) to `MoveBefore` back exactly.
4. **A drag is one command.** The gizmo applies live but commits a single `EditTransform` on
   release (Plan 02's seam), so undo reverts the whole drag. Cross-edit transaction grouping is out
   of scope (README *What remains future*).
5. **Commands re-run `ResolveEntity` where a mesh source is touched.** A recipe edit's undo/redo
   must re-stream the derived mesh, so the relevant commands call the same
   `PrefabEditContext::ResolveEntity` the live edit path uses — no special undo handling of the
   async mesh build.
6. **Structural commands respect the no-mutate-mid-iteration rule.** The explorer already defers
   edits past its snapshot walk; pushing commands keeps that deferral, so the `Scene` contract is
   never violated by a command applied mid-draw.

## Files

| File | Change |
|---|---|
| `editor/src/EditorCommand.h` (new) | `EditorCommand` interface + the concrete command types. |
| `editor/src/CommandStack.{h,cpp}` (new) | The undo/redo deques, `Push`/`Undo`/`Redo`, depth cap, dirty signal. |
| `editor/src/AssetEditorPanel.{h,cpp}` | Own a `CommandStack` per document; expose it to children + the host. |
| `editor/src/panels/PrefabExplorerPanel.{h,cpp}` | Build + push hierarchy/create/destroy/duplicate commands instead of inline mutation (keeping the post-walk deferral). |
| `editor/src/panels/InspectorPanel.{h,cpp}` | Push add/remove/reset + `EditField` (byte-snapshot) commands from the changed-bool path. |
| `editor/src/panels/SceneViewportPanel.{h,cpp}` | The gizmo commit hook pushes one `EditTransform` per drag. |
| `editor/src/EditorHost.{h,cpp}` | Net-new: track the focused `AssetEditorPanel`; add an Edit menu (undo/redo with `Title()`) and Ctrl/Cmd+Z / Ctrl/Cmd+Shift+Z handling, dispatched to that document's stack. |

## Verification

- Clean build; `ctest` green (editor-only).
- Editor-run check: move an entity with the gizmo, undo → it returns; redo → it moves back. Edit
  an inspector field, reparent in the hierarchy, add/remove a component, delete a subtree,
  duplicate — each undoes and redoes, including a recipe-source edit re-streaming the mesh.
  Undo across the cap is bounded; redo is cleared by a fresh edit. Two open prefab editors undo
  independently. Selection is untouched by undo (beyond `Prune` dropping a destroyed entity).
- `smoke_golden` does **not** move.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
