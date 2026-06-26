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

- **An immediate-mode `DebugDraw` accumulator, per `SceneView`.** The accumulator is owned by the
  `SceneRenderer` (one per managed viewport) and reached through the `SceneView` channel — the same
  handle the app uses to push its `ViewState`. It must live there, not on `Context`, because the
  flush pass and the depth target it tests against are per-`SceneRenderer`: a single global
  accumulator could not composite correctly into multiple viewports (split-screen) with different
  cameras and depth. A thin `Context`-level convenience that forwards to the primary viewport serves
  single-viewport callers, but the canonical home is the `SceneView`. The accumulator gathers
  primitives for the frame and clears each frame:
  - `DrawLine(a, b, color)` and the line-built helpers (`DrawBox(aabb)`, `DrawSphere`, `DrawFrustum`,
    `DrawTransform`/axes) for wireframe gizmos.
  - `DrawBillboard(worldPos, size, TextureHandle, color)` — a camera-facing textured quad. The
    billboard takes a **bindless `TextureHandle`**, so the engine provides the mechanism and the
    consumer supplies the icon — the engine ships no icon content.

- **A `DebugDrawScenePass` inside `SceneRenderer`.** A `ScenePass` contributed into the renderer's
  internal compiled graph, gated by a `SceneRendererSettings` toggle (a `Configure` recompile knob).
  It runs at **full output extent, into the LDR scene color after the terminal tonemap**, so debug
  geometry is rendered at native resolution and composites over the final image with exact,
  exposure-independent colors — the right posture for overlay content (crisp lines, sharp icons, a
  gizmo color that reads the same regardless of scene exposure). It is **not** depth-hardware-tested
  against the scene; instead the shader **samples the g-buffer depth** to compute the occluded fade
  (next bullet). The g-buffer depth lives at the dynamic-resolution sub-rect, so the sample remaps the
  output UV by the tonemap pass's `RenderScaleUV`/`maxValidUV`
  ([`SceneRenderer.cpp`](../../engine/src/Renderer/SceneRenderer.cpp), the same constants tonemap
  applies to upscale its sub-rect source) — one line of UV math, no new attachment. Debug geometry is
  trivially cheap to rasterize, so rendering it at full res rather than the DRS sub-rect costs
  effectively nothing; subjecting it to DRS downscaling would only blur high-frequency overlay content
  to save fill-rate it never consumed. Lines are a per-frame dynamic vertex buffer (ring-buffered per
  frame-in-flight, or retired through the engine's transfer-timeline path, so the GPU never reads a
  buffer being rewritten); billboards are an instanced draw reading the accumulator.

- **Depth-aware with a dim occluded fallback, via a single depth-sampling shader.** A gizmo behind
  geometry draws **faded** rather than hidden — you keep sight of a light behind a wall but it reads
  as occluded. The shader samples the g-buffer depth once (at the remapped sub-rect UV above),
  compares it to the fragment's depth, and attenuates the color on the occlusion test, in **one
  pass** — not a two-pass `LEqual`/`Greater` pair. The fade is a soft effect, so sampling the coarser
  sub-rect depth is plenty precise. Debug-vs-debug geometry is not hardware depth-sorted (that would
  need a dedicated full-res depth attachment); it rides on draw order and blending, which is fine for
  wireframe and billboards.

- **An editor icon pack.** A new cooked pack shipped with the editor (the editor framework already
  cooks its own assets through the cook-on-demand path; this is a small built-in pack of light/camera
  icon textures), mounted by `EditorHost`. The icons resolve to `TextureHandle`s the editor passes to
  `DrawBillboard`.

- **Light/camera gizmos in the viewport.** `SceneViewportPanel`
  ([`editor/src/panels/SceneViewportPanel.cpp`](../../editor/src/panels/SceneViewportPanel.cpp)) walks
  the scene each frame and pushes one `DrawBillboard` per `Light` / `Camera` (icon by component kind),
  plus optional wireframe gizmos (a `Light`'s range sphere / spot cone via `DrawSphere`/`DrawFrustum`,
  a `Camera`'s frustum). Click-to-select is **out of scope** for this plan, which delivers
  visibility, not manipulation: billboard hit-testing against the icon quads rides with the later
  manipulation-gizmo editor plan (decision 4). The existing `ScreenToWorldRay` picking seam is where
  that follow-on extends, named here so it is cheap to add then.

## Decisions

1. **A `ScenePass` in `SceneRenderer`, at full output extent after tonemap.** Debug-draw is part of
   the viewport output and depth-aware against the scene, so it lives in the deferred graph behind a
   settings toggle — runtime-usable, not editor-only. It renders into the LDR scene color at full
   output resolution after tonemap, not into the HDR sub-rect before it: overlay content wants exact,
   exposure-independent colors and native-resolution crispness, and it is too cheap to rasterize for
   the DRS sub-rect's fill-rate savings to be worth blurring it. The depth-aware fade samples the
   sub-rect g-buffer depth with the tonemap pass's `RenderScaleUV`/`maxValidUV` remap — one line of
   shader math, correct under dynamic resolution, no new attachment. (Sampling depth in-shader rather
   than hardware-testing is what lets a full-res pass read a sub-rect depth target; it also gives the
   soft occluded fade for free.)

2. **Depth-tested with a dim occluded fallback, one pass.** Occluded gizmos fade rather than vanish —
   you never lose track of a hidden light, but depth is still honored. A single depth-sampling shader
   attenuates on the occlusion test; the two-pass `LEqual`/`Greater` form is rejected as twice the
   draws for the same result.

3. **The engine provides the mechanism; the editor provides the icons.** `DrawBillboard` takes a
   bindless `TextureHandle`; the icon textures ship in an **editor** pack, never in `libveng`. This
   keeps the engine content-free and matches the cook-on-demand boundary (`libveng_cook` is editor-exe
   only).

4. **Lines + billboards only; visibility, not manipulation.** No text rendering, no interactive
   transform-gizmo handles (translate/rotate widgets), and no billboard click-to-select in this plan —
   those are larger and separable. This delivers *visibility* of non-mesh objects, the stated need;
   manipulation gizmos and icon picking are a later editor plan, extending the `ScreenToWorldRay`
   seam.

## Verification

Clean build, `ctest`, `validation_gate` green (the new pass runs under the gpu band). `smoke_golden`:
debug-draw is a `Configure` toggle **off by default**, so the sample's golden capture is unchanged.
The pass is exercised by a debug-on render in a gpu test that runs **with dynamic resolution active**,
proving the sub-rect depth remap is correct (a gizmo at a known depth occludes and fades as expected
when the DRS scale is below 1.0, not just at native — i.e. the `RenderScaleUV` sample lands on the
right depth texel). In the editor, a scene with a light and a camera shows their icons in the
viewport, faded when behind geometry, sharp when not.
