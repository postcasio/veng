# Plan 01 — runtime + editor in-scene camera

**Goal:** make the runtime render from a camera that lives **in the scene**, selected through a
`Viewer` seat. The sample drops its hardcoded `CameraView m_Camera` member, authors a `Camera` +
`Transform` entity plus a `Viewer` seat in `scene.prefab.json`, and resolves the view each frame
in `OnRender` via Plan 00's `ResolvePrimaryCameraView` (output-extent aspect, a default-view
fallback). The editor keeps its external `EditorCamera` for editing and gains a Play-only "render
through the scene's viewer camera" toggle over the existing Play-in-viewport clone — so the
editor can show *what the player sees*. First plan that can move `smoke_golden`.

## Why it is its own plan

It is the runtime migration — the breaking change from an app-owned `CameraView` to a
scene-sourced one — and the golden risk lives here, isolated from the additive engine seam (Plan
00) and the gameplay layers (Plans 02–04). The editor "game view" path rides along because it is
the same resolve helper applied to the editor's Play clone, and it is small.

## What lands

- **The sample authors an in-scene camera + a seat.** `scene.prefab.json`
  ([examples/hello-triangle/assets/scene.prefab.json](../../examples/hello-triangle/assets/scene.prefab.json))
  gains a camera entity (`Name`, a `Transform` posed to reproduce today's viewpoint — eye
  `(0, 10, 14)` looking at the origin, from
  [main.cpp:161](../../examples/hello-triangle/main.cpp:161) — and a `Camera` with the existing
  `FovY = 45°` / `Near = 0.1` / `Far = 100`), and a small **viewer/seat entity** carrying
  `Viewer { Camera = <the camera entity> }`. (Plan 04 moves this seat onto the spawned player;
  here it is an authored bootstrap seat.)

- **The hardcoded `CameraView` setup is removed.** The `m_Camera.SetPerspective(...)` /
  `SetView(...)` block ([main.cpp:159](../../examples/hello-triangle/main.cpp:159)) and the bare
  `CameraView m_Camera` member ([main.cpp:506](../../examples/hello-triangle/main.cpp:506)) go
  away. A new app-local **`DefaultCameraView(aspect)`** helper supplies the fixed fallback pose.
  `OnRender` resolves the view from the scene:

  ```cpp
  const f32 aspect = f32(sceneExtent.x) / f32(sceneExtent.y);   // the SceneRenderer's extent
  const CameraView camera = ResolvePrimaryCameraView(*m_Scene, aspect)
                                .value_or(DefaultCameraView(aspect));
  const Renderer::SceneView view{ .World = *m_Scene, .Camera = camera, ... };
  ```

  `aspect` comes from the scene-render extent the sample already computes
  ([main.cpp:158](../../examples/hello-triangle/main.cpp:158)) — the render target, per scope
  decision 3. The `value_or` fallback (a small fixed default pose) keeps the app rendering if the
  scene ever has no camera; the app owns that fallback.

- **Editor: an optional "view through the scene viewer" during Play.** `SceneViewportPanel` today
  always renders from its resolved `m_View` (the `EditorCamera`'s `CameraView`,
  [SceneViewportPanel.cpp:133](../../editor/src/panels/SceneViewportPanel.cpp:133)). Add a
  toolbar toggle — active only while Play is running — that switches the panel's `SceneView`
  source between the editor camera and `ResolvePrimaryCameraView(*m_Ctx.Scene, panelAspect)`. Edit
  mode and Stop always use the editor camera; Play offers either. The Play-in-viewport clone +
  system tick already exist (commit 47e4962); this only changes which `CameraView` the clone
  renders through. The previewed view is whatever the played prefab authors — the sample's scene
  prefab now authors a camera + `Viewer`, so the toggle shows it; a prefab with no viewer falls
  back to the editor camera. Previewing a fully wired game (the rule-spawned player, a `Level`'s
  systems) is the Level editor's job (Plan 07); here it is the same resolve over the prefab clone.

## The golden

The smoke path pins a fixed pose and never ticks the simulation
([main.cpp:183](../../examples/hello-triangle/main.cpp:183)), so the capture stays reproducible —
but the *camera* now comes from a `Transform` (a TRS quaternion) resolved through `WorldMatrix` +
`MakeCameraView`, where it previously came from `glm::lookAt`. The two agree mathematically (view
= inverse(world); a `lookAt` equals the inverse of the equivalent TRS up to float rounding), and
the `smoke_golden` compare is **fuzzy**, so the capture may stay within tolerance. **If it does
not, regenerate the golden in this plan** per the `CLAUDE.md` procedure:

```sh
HT_SMOKE=/tmp/ht.ppm build/examples/hello-triangle/hello_triangle-launcher
sips -s format png /tmp/ht.ppm --out tests/golden/hello_triangle_scene.png
```

The expected visual change, if any, is **none to the eye** — the same pose, only the matrix path
differs. A *visible* shift means the authored `Transform` does not match the old eye/target and
must be corrected before re-blessing.

## Decisions

1. **Camera and seat are authored in the scene prefab, not spawned in code.** They are part of
   the authored scene, persisted and editable. (Plan 04 moves the *seat* onto a rule-spawned
   player for the dynamic case; the authored camera is the static baseline.)

2. **Aspect from the scene-render extent, not the swapchain.** The sample renders the scene at a
   fixed internal `SceneRenderer` extent and composites to the swapchain
   ([main.cpp:136](../../examples/hello-triangle/main.cpp:136)); the camera's aspect must match the
   target it renders into.

3. **A `value_or` default view, not an assert, for the no-camera case.** A scene that loses its
   camera still presents something; the app owns that fallback pose.

4. **The editor default stays the editor camera.** Editing needs an external vantage; the scene
   viewer is an opt-in preview during Play only — preserving "viewport owns the `CameraView`" as
   the editor's default (scope decision 1) while making the game view reachable.

## Files

| File | Change |
|---|---|
| `examples/hello-triangle/assets/scene.prefab.json` | Add the camera entity (`Name` + `Transform` + `Camera`) and a viewer/seat entity (`Viewer { Camera }`). |
| `examples/hello-triangle/main.cpp` | Remove `m_Camera` + its hardcoded setup; resolve the view in `OnRender` via `ResolvePrimaryCameraView` (output-extent aspect, `DefaultCameraView` fallback). |
| `editor/src/panels/SceneViewportPanel.{h,cpp}` | A Play-only toolbar toggle selecting the `SceneView` source: `EditorCamera` (default) or the scene viewer. |
| `tests/golden/hello_triangle_scene.png` | Re-blessed **only if** the resolved view moves the capture beyond the fuzzy threshold. |

## Verification

- Clean build; `ctest` green across the bands.
- `hello_triangle_launcher_smoke` writes a correct-sized PPM and exits 0 through the full
  `dlopen` → resolve-camera → render chain.
- `smoke_golden`: passes against the existing golden if within tolerance; otherwise regenerated
  here with "same pose, matrix-path-only change" stated in the commit.
- Editor: edit mode renders from the orbit/fly camera; Play with the toggle off renders from the
  editor camera; Play with the toggle on renders from the authored scene viewer.
- The relocatable-trio test still passes.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
