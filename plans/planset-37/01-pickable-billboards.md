# Plan 01 — pickable billboards + viewport click-to-select

**Goal:** make the editor's light/camera **billboards** selectable through the same id pass as
meshes, and wire a **viewport click → selection**. The billboard pass writes the owning entity
id into the id target with a **fixed min-size proxy footprint** (not the icon's art alpha), so a
point-gizmo icon is a predictable, forgiving, art-independent hit target; the editor tags its
gizmo billboards with their entity ids and routes a click through `Viewport::Pick` into the
shared `PrefabEditContext` selection — synced with the hierarchy explorer and the inspector.
This closes **selection** end to end. **Depends on Plan 00** (the id target + the `Pick` seam).

## Why it is its own plan

Plan 00 lands the id mechanism with meshes as the first consumer; this plan widens it to
billboards (entities with no mesh, whose only viewport presence is a debug icon) and is the
first plan to touch editor selection. The billboard half is a small engine change
(`DebugDraw` + the billboard pass); the wiring half is editor-side
(`SceneViewportPanel`/`PrefabEditContext`). Keeping them in one plan but separate from 00 keeps
00 engine-only and lets this plan own the "what does a click select" decision in full.

## What lands

- **`DebugBillboard` / `DrawBillboard` carry an optional pick id.** `DebugBillboard` gains a
  pick-id field (the owning entity's **pick id** = packed index + 1, 0 = not pickable), and
  `DebugDraw::DrawBillboard(worldPosition, size, texture, color, pickId = 0)` takes it. A
  billboard pushed with `pickId == 0` is decorative (drawn, never written to the id target); a
  non-zero id makes it pickable. Lines are never pickable.

- **A dedicated billboard id-write, in the geometry-pass timeframe, with a hard depth discard.**
  The id target is written by the geometry pass (Plan 00) and is consumed before lighting; the
  decorative billboard render runs in `DebugDrawScenePass`, which composites into the LDR color
  **after tonemap** and is **not** hardware depth-tested (it samples g-buffer depth and *fades* an
  occluded fragment). That fade path is wrong for an id write and runs too late (the id target is
  no longer bound). So the pickable id write is a **separate gated pass** that runs while the
  `EntityId` target is still bound (alongside the geometry pass, before lighting): for each
  pickable billboard it rasterizes the proxy footprint, samples the scene depth, and **discards**
  (does not fade) an occluded fragment, writing the pick id only where the icon is in front — so an
  icon behind geometry is not picked and an icon in front wins. The decorative `DebugDrawScenePass`
  render is unchanged; this is an id-only sibling.

- **The footprint is a fixed min-size proxy, not the art alpha.** The written footprint is **not**
  the icon art's alpha — it is a centered proxy: the billboard's quad clamped to a **minimum pixel
  radius** (a small fixed screen-space size, so a spindly icon is still a comfortable target),
  configurable per billboard kind via a field on the record (e.g. a `PickRadius`/`PickShape`),
  defaulting to a centered disc of that minimum radius (a clamped quad is the per-kind alternative).

- **The editor tags its gizmo billboards.** `SceneViewportPanel::PushGizmos` already pushes a
  billboard per `Light`/`Camera`; it now passes each entity's `pickId` (index + 1) so the icons
  are pickable. (Its current doc comment — *"Click-to-select is out of scope;
  `Viewport::ScreenToWorldRay` is the seam a later billboard-picking follow-on extends"* — is
  this plan; update it to describe the delivered id-pass picking.)

- **Viewport click → selection.** `SceneViewportPanel` enables `SceneRendererSettings::Picking`
  on its viewport and, on a left-click inside the content rect that is not consumed by a gizmo
  drag (Plan 02) or camera control, calls `Viewport::Pick(windowPoint, …)`; the callback updates
  the shared `PrefabEditContext` selection — plain click `SelectOnly`, Ctrl-click `Toggle`, a
  `nullopt` (background) click clears (unless additive). Because the explorer and inspector
  already read `PrefabEditContext::Selection`/`Active`, the click selects in the viewport and the
  hierarchy highlight + inspector follow with no extra wiring. The pick is async (resolves a frame
  or two later through the continuation pump); a transient "pick in flight" guard avoids
  double-issuing on a held button, and the selection is applied through Plan 00's scene-epoch +
  caller-liveness guard so a resolve that lands after a Play/Stop scene swap or a document close is
  dropped rather than applied to the wrong (or a destroyed) `PrefabEditContext`.

## Decisions

1. **Billboards are pickable through the id pass, not a ray.** A billboard's id write is
   depth-correct and screen-space-sized for free, so it composes with mesh picking in one
   readback; ray-vs-quad would have to replicate the screen-space sizing and resolve overlap by
   hand. (Gizmo *handles*, Plan 02, are the ray case — they are manipulation, not selection.)
2. **Proxy footprint, not art alpha.** Icons are point gizmos marking a transform, not surfaces;
   tying the hit target to the art silhouette makes a thin/weird icon hard and unpredictable to
   click, and the bare bounding quad picks dead corners. A fixed min-size centered proxy is
   predictable and forgiving, and survives an art redesign. Strict alpha-test is reserved as an
   opt-in per billboard kind for a rare genuinely-pictorial billboard.
3. **Selection is editor state in `PrefabEditContext`.** The click feeds the *same* selection the
   hierarchy drives — one selection model, two input sources — so the inspector/explorer need no
   change. Selection is not undoable (Plan 03).
4. **Async pick, one in flight.** The click records a request and the callback applies the
   result; a guard prevents issuing a second pick while one is pending, so a held mouse button
   does not queue a burst.

## Files

| File | Change |
|---|---|
| `engine/include/Veng/Renderer/DebugDraw.h` | Add the pick-id (and proxy `PickRadius`/`PickShape`) field to `DebugBillboard`; add the `pickId` parameter to `DrawBillboard`. |
| `engine/src/Renderer/DebugDraw.cpp` | Carry the id/proxy through the accumulator. |
| `engine/src/Renderer/DebugDrawScenePass.{h,cpp}` | The decorative billboard render is unchanged; under `Picking` a sibling id-only write runs in the geometry-pass timeframe (while the `EntityId` target is bound), rasterizing each pickable billboard's min-size proxy footprint and **discarding** (not fading) the occluded fragment by a scene-depth sample. |
| `engine/assets/core/shaders/debug_billboard.*` | The id-write variant (gated) emitting the proxy footprint with a hard depth discard. |
| `editor/src/panels/SceneViewportPanel.{h,cpp}` | Enable `Picking`; tag gizmo billboards with entity pick ids; route a content-rect click → `Viewport::Pick` → `PrefabEditContext` selection; update the `PushGizmos` doc comment. |

## Verification

- Clean build; `ctest` green (no new render output — the editor is not in the smoke path).
- Manual / editor-run check: clicking a mesh selects its entity; clicking a light or camera
  **icon** selects that entity (including when the icon is small/thin — the proxy makes it a
  comfortable target); clicking empty space clears; Ctrl-click toggles; the hierarchy highlight
  and inspector follow every viewport click.
- An icon **behind** geometry is not picked through the geometry (depth-tested); an icon in front
  is.
- `smoke_golden` does **not** move (`Picking` is editor-only; the shipping path is untouched).

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
