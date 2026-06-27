# Plan 02 — hand-rolled manipulation gizmos

**Goal:** a **hand-rolled** translate / rotate / scale gizmo on the active selection, drawn
through the per-viewport `DebugDraw` channel and interacted with by **analytic ray-vs-handle**
hit-testing over `Viewport::ScreenToWorldRay` — no ImGuizmo dependency. Dragging a handle mutates
the selected entity's `Transform`; the edit is committed as **one** undo command on release
(the command type lands in Plan 03; this plan applies the transform directly and exposes the
commit seam). **Depends on Plan 01** (a selection to act on, and the shared `ScreenToWorldRay`
call site at the viewport).

## Why it is its own plan

Manipulation is the second of the planset's two picking mechanisms (analytic ray vs. known
handle geometry, the manipulation case) and the visible payoff of selection. It is self-contained
editor work — a gizmo renderer + an interaction state machine — that the undo stack (Plan 03)
then wraps. Keeping it separate from selection (Plan 01) keeps each plan's input model clean:
01 owns "a click selects," 02 owns "a drag transforms."

## What lands

- **A gizmo model — `EditorGizmo` (editor-side).** Holds the current mode
  (`Translate`/`Rotate`/`Scale`), the space (World v1; Local a named follow-on), and the active
  drag state (which handle, the grab point, the start `Transform`). Operates on
  `PrefabEditContext::Active` (with multi-select pivoting on the active entity's world position;
  applying to the whole selection is a follow-on). Placed at the entity's **world** position from
  `ComputeWorldMatrices`, writing back through the scene `Transform` accessor each frame (so the
  spatial-version bump fires, per the `Scene` contract).

- **Drawn through `DebugDraw`.** The gizmo's axes (translate arrows), rings (rotate), and boxes
  (scale) are pushed as lines/quads into the viewport's `DebugDraw` accumulator each frame — the
  same per-viewport channel the light/camera gizmos use — so it composites depth-aware into the
  scene with no new pass. The hovered/active handle is highlighted (color shift).

- **Analytic ray interaction.** On mouse-move the panel builds the cursor ray
  (`Viewport::ScreenToWorldRay`) and hit-tests it against the handle geometry analytically (ray vs.
  the axis cylinders / ring tori / plane quads — known shapes, cheap, no id buffer), producing a
  hovered handle for highlight. On press over a handle the drag begins; on drag the new
  `Transform` is solved from the ray against the handle's constraint (axis translate = closest
  point on the axis line to the ray; plane translate = ray∩plane; rotate = angle swept in the
  ring plane; scale = projected axis delta); on release the drag ends. A press **not** over a
  handle falls through to Plan 01's click-to-select (the two share the content-rect mouse path,
  gizmo-first).

- **The commit seam.** While dragging, the `Transform` is updated live (immediate visual
  feedback). On release the panel calls a commit hook with the start and final `Transform` — in
  this plan a direct no-op beyond the already-applied value; Plan 03 replaces the hook body with
  pushing one `EditTransform` command spanning the whole drag (so a drag is one undo step, not
  one per frame).

- **Mode keys + toolbar.** W/E/R (or a toolbar segment in the existing viewport overlay) select
  translate/rotate/scale; the gizmo is hidden when nothing is selected.

## Decisions

1. **Hand-rolled, no ImGuizmo.** Consistent with the editor's vendor discipline (imnodes is the
   only editor-private vendor lib, src-only); a gizmo is a few hundred lines over the
   `DebugDraw` + `ScreenToWorldRay` primitives already in hand, and avoids a new dependency and
   its ImGui-version coupling.
2. **Manipulation is analytic ray, not the id buffer.** A gizmo needs sub-pixel axis
   discrimination and live per-frame hover; an async id-buffer readback would lag the hover and
   waste a pick per frame. The handles are known shapes, so ray-vs-handle is exact and immediate.
3. **Drawn through the existing per-viewport `DebugDraw` channel.** No new render pass — depth
   awareness, split-screen correctness, and the LDR composite are already handled by
   `DebugDrawScenePass`. Screen-space-constant handle sizing is a named follow-on (v1 sizes the
   gizmo in world units scaled by camera distance).
4. **Commit on release = one undo step.** The transform is applied live for feedback but recorded
   as a single command over the whole drag (Plan 03), so undo reverts the drag, not a frame of it.
   The seam is exposed here so 03 is a body swap, not a rewire.
5. **World space, active-entity pivot, v1.** Local-space handles and a true multi-entity pivot are
   named follow-ons; v1 manipulates the active entity in world space (the common case), keeping
   the interaction math small.

## Files

| File | Change |
|---|---|
| `editor/src/EditorGizmo.{h,cpp}` (new) | The gizmo model + the analytic ray-vs-handle hit-test + the per-mode transform solve. |
| `editor/src/panels/SceneViewportPanel.{h,cpp}` | Own an `EditorGizmo`; draw it via `DebugDraw`; route the content-rect mouse (hover/press/drag/release) gizmo-first, falling through to click-select; mode keys + toolbar; the commit hook (direct apply, the Plan 03 seam). |
| `editor/src/panels/PrefabEditContext.h` | (If needed) a small accessor for the active entity's world transform used by the gizmo placement. |

## Verification

- Clean build; `ctest` green (editor-only, not in the smoke path).
- Editor-run check: with an entity selected, the gizmo appears at its world position; W/E/R switch
  mode; hovering a handle highlights it; dragging an axis/plane/ring/scale-box moves/rotates/scales
  the entity live and the inspector's `Transform` values track it; releasing leaves the entity at
  the dragged transform; clicking off a handle still selects (Plan 01 fall-through).
- `smoke_golden` does **not** move.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
