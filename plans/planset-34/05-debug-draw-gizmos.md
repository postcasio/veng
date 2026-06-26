# Plan 05 — engine debug-draw + editor gizmos

**Goal:** give the engine a general immediate-mode debug-draw API (lines + textured billboards) that
renders **inside** the deferred pipeline, depth-aware, and use it in the editor to draw icons for
non-mesh objects — lights and cameras — so they are visible and selectable in the viewport. Heaviest
plan; independent; lands last.

## Why engine-side, not an editor overlay

The viewport shows only meshes; a `Light` or `Camera` entity has no visual, so you cannot see or click
one. There is no debug-draw or billboard infrastructure anywhere in the engine or editor today. Making
it a **general engine API** (rather than an editor-only ImGui-draw-list overlay) means it renders as
part of the viewport output, shares the scene depth buffer (so gizmos occlude correctly), and is
reusable at runtime — a game can draw debug lines/icons with the same call. The editor is the first
consumer, not the owner.

## What lands

- **An immediate-mode `DebugDraw` accumulator.** A per-frame API on the engine (reachable through the
  `Context` or the `Viewport`/`SceneView` channel) that accumulates primitives for the frame and
  clears each frame:
  - `DrawLine(a, b, color)` and the line-built helpers (`DrawBox(aabb)`, `DrawSphere`, `DrawFrustum`,
    `DrawTransform`/axes) for wireframe gizmos.
  - `DrawBillboard(worldPos, size, TextureHandle, color)` — a camera-facing textured quad. The
    billboard takes a **bindless `TextureHandle`**, so the engine provides the mechanism and the
    consumer supplies the icon — the engine ships no icon content.

- **A `DebugDrawScenePass` inside `SceneRenderer`.** A `ScenePass` contributed into the renderer's
  internal compiled graph, gated by a `SceneRendererSettings` toggle (a `Configure` recompile knob).
  It runs **after tonemap, into scene color**, so debug geometry composites over the final image.
  Lines are a per-frame dynamic vertex buffer; billboards are an instanced draw reading the
  accumulator. It **reads the scene depth target** for depth-aware rendering (one of the depth
  textures the engine already samples), so it needs no new attachment beyond what the g-buffer depth
  provides.

- **Depth-tested with a dim occluded fallback.** A gizmo behind geometry draws **faded** rather than
  hidden — the best-UX option: you keep sight of a light behind a wall but it reads as occluded. This
  is the two-pass / depth-aware-blend form (a full-strength depth-`LEqual` pass plus a dimmed
  depth-`Greater` pass, or a single depth-sampling shader that attenuates on the occlusion test).

- **An editor icon pack.** A new cooked pack shipped with the editor (the editor framework already
  cooks its own assets through the cook-on-demand path; this is a small built-in pack of light/camera
  icon textures), mounted by `EditorHost`. The icons resolve to `TextureHandle`s the editor passes to
  `DrawBillboard`.

- **Light/camera gizmos in the viewport.** `SceneViewportPanel`
  ([`editor/src/panels/SceneViewportPanel.cpp`](../../editor/src/panels/SceneViewportPanel.cpp)) walks
  the scene each frame and pushes one `DrawBillboard` per `Light` / `Camera` (icon by component kind),
  plus optional wireframe gizmos (a `Light`'s range sphere / spot cone via `DrawSphere`/`DrawFrustum`,
  a `Camera`'s frustum). Selection already has the `ScreenToWorldRay` picking seam; billboard
  hit-testing against the icon quads is the natural follow-on so a click selects the light — included
  if it falls out cheaply, otherwise noted.

## Decisions

1. **A `ScenePass` in `SceneRenderer`, not a post-viewport overlay.** Debug-draw is part of the
   viewport output and depth-aware against the scene, so it lives in the deferred graph behind a
   settings toggle — runtime-usable, not editor-only. (Confirmed direction.)

2. **Depth-tested with a dim occluded fallback.** Occluded gizmos fade rather than vanish — you never
   lose track of a hidden light, but depth is still honored. The extra draw/blend is worth the UX.

3. **The engine provides the mechanism; the editor provides the icons.** `DrawBillboard` takes a
   bindless `TextureHandle`; the icon textures ship in an **editor** pack, never in `libveng`. This
   keeps the engine content-free and matches the cook-on-demand boundary (`libveng_cook` is editor-exe
   only).

4. **Lines + billboards only.** No text rendering, no interactive transform-gizmo handles (translate/
   rotate widgets) in this plan — those are larger and separable. This delivers *visibility* of
   non-mesh objects, the stated need; manipulation gizmos are a later editor plan.

## Verification

Clean build, `ctest`, `validation_gate` green (the new pass runs under the gpu band). `smoke_golden`:
debug-draw is a `Configure` toggle **off by default**, so the sample's golden capture is unchanged;
the pass is exercised by a debug-on render in a gpu test. In the editor, a scene with a light and a
camera shows their icons in the viewport, faded when behind geometry, sharp when not.
