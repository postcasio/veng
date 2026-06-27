# planset-37 — scene editor interaction: id-buffer picking, gizmos, undo/redo, prefab save-back

**Phase goal:** close the scene editor's authoring loop. The prefab editing *surface* is
delivered ([editor/CLAUDE.md](../../editor/CLAUDE.md)) — `PrefabEditorPanel` spawns a prefab
into a live `Scene` and hosts a viewport + hierarchy explorer + reflection inspector over one
shared `PrefabEditContext` — but the *interaction* is still hierarchy-and-inspector only: you
cannot click an entity in the viewport, you cannot drag a handle to move one, nothing is
undoable, and nothing saves back to source. This planset adds the four pieces that make the
editor authoritative: **pick → manipulate → undo → save**.

This is the remaining slice of [future area 6](../future/README.md#6-editor-application)
**sub-area D** (the scene editor). Its gates are all met — the runtime `Scene`/`Camera`
(planset-10), the cooked **prefab** asset + module reflection (planset-11), the `SceneRenderer`
(planset-12), and the editor shell + reflection inspector (planset-14) — and the
**pointer-to-world seam is delivered**: planset-31's `Viewport::WindowToViewport` /
`ScreenToWorldRay` already turn a panel-content-rect click into a world ray. (A `Scene` is a
**runtime-only** construct; the authored asset is the **`Prefab`**, and a `Level` references a
prefab — so save-back round-trips a `.prefab.json`, **not** a cooked scene asset.)

## The shape — selection vs. manipulation, the two picking mechanisms

The keystone decision is that the editor has **two** spatial-picking mechanisms, split by what
they answer:

```
  Selection  ("which thing did I click")   →  GPU id-buffer pass + async readback
    meshes AND light/camera billboards          discrete, click-driven, tolerates a frame
                                                 of readback latency, wants pixel-perfect +
                                                 depth-correct

  Manipulation ("which handle am I dragging")  →  analytic ray vs. known handle geometry
    gizmo axes/rings/planes                       per-frame, live hover, zero latency, over
                                                  shapes the editor itself defines
```

**Selection uses an id buffer, not a ray**, and this is the more correct technique for an
editor: veng's `Mesh` is **immutable and GPU-only** (vertex data is not retained CPU-side after
upload), so CPU ray-triangle picking would force retained geometry copies, while CPU ray-AABB
is imprecise (you hit the box, not the silhouette). An id pass sidesteps both — pixel-perfect
for free, depth-test resolves overlap, no CPU geometry. Its one cost is a GPU→CPU readback,
paid the veng-idiomatic async way (transfer-timeline copy + continuation-pump resolve), and
click-driven so a frame of latency is invisible. **Manipulation stays analytic** because a
gizmo needs sub-pixel axis discrimination and live hover with no readback round-trip.

**Billboards go in the id pass, not the ray method.** A light or camera has no mesh — its only
viewport presence is the editor's debug billboard, so the billboard *is* its selectable
surface. The billboard pass writes the owning entity id into the same id target, depth-tested,
so a click resolves whatever is on top — mesh or icon. Crucially the billboard's id footprint
is a **fixed min-size proxy** (a centered disc / clamped quad), **not** the icon's art alpha:
icons are point gizmos, so a predictable, art-independent hit target is the right UX — strict
alpha-discard would make a thin/spindly icon a hard, unpredictable target, and the bare
bounding quad would pick dead corners. The default proxy is a centered disc of a fixed minimum
screen-space radius; a clamped quad is the per-kind alternative.

## Plans

| # | Plan | Summary | Status |
|---|---|---|---|
| 00 | Entity-id picking pass + `Viewport::Pick` seam | An optional, off-by-default engine **id pass** (a new `Format::R32Uint` target, **pick id** = packed entity index + 1 written, depth-tested) in `SceneRenderer`, gated by a `SceneRendererSettings` flag so the shipping path is untouched; an async readback (transfer-timeline copy → continuation-pump resolve) behind a `Viewport::Pick(windowPoint, callback)` seam returning `optional<Entity>`, with a scene-epoch + caller-liveness guard so a late resolve never lands in a swapped or torn-down scene. Meshes only; foundational, engine-only. | done |
| 01 | Pickable billboards + viewport click-to-select | The billboard pass writes the owning entity id with a **fixed min-size proxy footprint** (configurable per billboard kind), so `DebugDraw::DrawBillboard` carries an optional pick id; the editor tags its light/camera gizmo billboards with their entity ids and wires a viewport click → `Viewport::Pick` → `PrefabEditContext` selection (sync with the hierarchy/inspector). Closes selection end-to-end. Depends on 00. | done |
| 02 | Hand-rolled manipulation gizmos | A hand-rolled translate/rotate/scale gizmo on the active selection, drawn through the per-viewport `DebugDraw` channel and interacted with by analytic ray-vs-handle hit-testing over `Viewport::ScreenToWorldRay` (no ImGuizmo dependency). A drag mutates the entity's `Transform`; the edit is committed as one undo command on release. Depends on 01. | done |
| 03 | Per-editor undo/redo command stack | A command/transaction stack **owned per `AssetEditorPanel`** (each open document undoes independently). `EditorCommand` with `Apply`/`Revert`; commands for transform edits (gizmo + inspector), hierarchy reparent/reorder, add/remove/reset component, and field edits, wired into the explorer, inspector, and gizmo. Selection is editor state, not a command. Ctrl/Cmd+Z / Ctrl/Cmd+Shift+Z + Edit menu, routed to the focused document. Depends on 02. | ready |
| 04 | Prefab save-back (`Scene` → `.prefab.json`) | A reflection-driven `Scene` → `.prefab.json` writer (every entity + component + field through the name-keyed reflection walk, emitted as JSON via the editor's own nlohmann), round-tripping the **preserve-unknown-keys** way (planset-14 idiom) so hand-authored fields and formatting survive — keyed to a **stable per-entity id** so adds/deletes/reorders re-align to source rather than clobbering it. Save (Ctrl+S) writes the active document's source atomically (temp + rename); the dirty flag follows the undo stack. The level editor (which inherits the same scene-editing surface) routes its entity edits back to the referenced world `.prefab.json`. Depends on 03. | ready |

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = implemented, migrated, verified, committed.

## Dependencies

- **00** is foundational — the id pass + readback + the `Pick` seam every selection path reads.
  Engine-only (`SceneRenderer`/`Viewport`), off by default, so it stands alone.
- **01** depends on 00 (the id target + `Pick` seam it writes billboards into and reads clicks
  through). It is the first plan to touch editor selection wiring.
- **02** depends on 01 — a gizmo acts on the active selection, and selection-by-click is what
  makes it usable; it shares the per-viewport `DebugDraw` channel and the `ScreenToWorldRay`
  ray math 01 establishes at the call site.
- **03** depends on 02 — the gizmo drag is one of the commands the stack wraps, alongside the
  existing explorer/inspector mutations it retrofits.
- **04** depends on 03 — save writes whatever the prior three produced, and its dirty flag is
  driven by the undo stack.

The plans **chain** (`00 → 01 → 02 → 03 → 04`): 01–04 all touch the editor's scene-editing
files (`PrefabEditContext`, `SceneViewportPanel`, `PrefabExplorerPanel`, `InspectorPanel`), so
they merge in number order. Per [[project_megaexec_worktree_base]], `isolation: "worktree"`
branches from `origin/main` and will not see a locally-committed-but-unpushed base: only **00**
(foundational, on `origin/main`) can use `isolation: "worktree"` directly; **01 → 02 → 03 → 04**
each dispatch against a manual worktree cut from the prior plan's integration commit.

## The decisions this planset settles

- **Selection is an id buffer; manipulation is a ray.** Two mechanisms, split by latency
  tolerance and target shape. The id pass answers "which thing"; analytic ray-vs-handle answers
  "which handle." Neither tries to be the other.
- **The id pass is editor-only and off by default.** It is an optional `SceneRenderer` pass
  allocated only when the picking flag is set, re-rendering geometry only on a frame a pick is
  requested. The shipping render path never allocates it, so `smoke_golden` never moves.
- **Billboards are pickable through the id pass, with a proxy footprint.** A billboard writes
  its owning entity id as a fixed min-size proxy (not the art alpha), so a point-gizmo icon is a
  predictable, art-independent, forgiving hit target. A small screen-space readback search
  radius is the general "click-near" forgiveness on top.
- **Undo/redo is per-document, not global.** Each `AssetEditorPanel` owns its own command
  history, so two open prefab editors undo independently. Selection is editor state and is **not**
  on the stack.
- **Gizmos are hand-rolled, no ImGuizmo.** Keeps the editor's vendor discipline (like imnodes,
  no new public-header dependency) and reuses the per-viewport `DebugDraw` channel and
  `ScreenToWorldRay` already in hand.
- **Save-back round-trips the prefab source, preserving unknown keys, keyed by entity id.** A
  live `Scene` writes back to its `.prefab.json` through the reflection walk + the editor's
  nlohmann, patching known keys and preserving the rest — the texture/material/level editor idiom,
  applied to entities. Because `.prefab.json` entities are a *positional* array with no identity,
  the writer matches a live entity to its source object by a stable per-entity id key (added to
  the schema), so an add/delete/reorder re-aligns to the right source object instead of
  misattributing or dropping hand-authored content. The write is atomic (temp file + rename) so a
  failed save never corrupts the only copy of authored work. The level editor inherits the
  scene-editing surface, so its entity edits save through the same writer into the level's
  referenced world `.prefab.json` (its `*.level.json` config save is unchanged). No cooked scene
  asset exists or is introduced.
- **Additive format/enum changes only; no module-ABI bump.** The id pass appends one
  `Format::R32Uint` enumerator — a header/API addition mapped in `TypeMapping.h`, not a cooked-blob
  change. Save-back adds a stable, optional per-entity id key to the `.prefab.json` schema:
  additive, the cooker reads and preserves it, and an absent id falls back to positional order so
  existing sources still load. `VengModuleHost` and `FieldDescriptor` layout are untouched.

## What remains future

- **Triangle-precise / sub-mesh-precise selection** — the id pass picks per-entity; writing a
  sub-mesh or material-slot id is a later refinement behind the same target.
- **Marquee / multi-select via the id buffer** — reading a rectangle of the id target rather
  than one texel; the selection model already holds a multi-entity set.
- **Gizmo refinements** — screen-space-constant handle sizing, snapping (grid/angle), local vs.
  world space toggle, whole-selection manipulation (the gizmo acts on the active entity only),
  and a combined universal gizmo are extensions behind the hand-rolled base.
- **Cross-edit transaction grouping for the undo stack** — merging unrelated edits, or a
  transaction scope spanning several mutations. (Single-drag coalescing ships in Plan 03 via
  commit-on-release.)
- **Save-back to a cooked `.scene`** — explicitly **not** a thing: a `Scene` is runtime-only and
  the authored document is the prefab. (Recorded here so it does not resurface.)
