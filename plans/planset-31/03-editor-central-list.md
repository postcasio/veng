# Plan 03 — the editor on the central list

**Goal:** migrate the editor's hand-rolled viewports onto the engine drive-list, and retire the
editor's hand-rolled present path. Each render-owning panel — `SceneViewportPanel`, `MaterialPreview`,
and the asset-editor previews — stops owning a bare `SceneRenderer` and driving its own `Execute` in
`OnRender`; it holds a `Unique<Viewport>` from `Viewport::Create` and registers it
(`host.RegisterViewport(*m_vp)`, an `Offscreen` viewport), feeds its region from the ImGui content
rect, pushes a `ViewState`, and lets the **engine** render it. `EditorHost` registers no `Presented` viewport, so the managed gather produces a cleared
assembly target and the composite runs with **zero placements** (the dockspace is opaque ImGui). This
is the payoff of the central-list decision — `EditorPanel`'s scene-execute responsibility disappears,
and "0..N viewports including zero" becomes the drive-list length.

`EditorHost` today does **not** use `SwapChainCompositePass` at all: it owns a hand-rolled
`m_BlitPipeline` + `m_PresentGraph` that blits only the ImGui output to the swapchain, and overrides
`OnRender` wholesale to drive it. That path is **deleted** here — `EditorHost` runs the engine render
phase (Plan 02) like any app and relies on the managed gather + composite tail. The editor's ImGui
output now flows through the composite's display-transfer encode rather than a passthrough blit; on
the editor's normal SDR swapchain this is the same result (`SrgbNonlinear` passthrough encode).

**Depends on Plan 00 (the `Viewport` type) and Plan 02 (the drive-list + managed composite).**

## Why it is its own plan

It is the heaviest plan and the one the central-list decision was made for. The bulk is relocating
`SceneViewportPanel`'s per-panel logic — debounced resize, deferred `Configure`, the one-frame
camera latency, the ImGui-texture re-fetch — onto the shared `Viewport`, then letting the host
drive the render instead of the panel. Isolating it from the sample migration (Plan 02) keeps the
editor's larger churn off the golden-risk path, and lets the engine-driven loop be proven on the
simple game first.

## What lands

- **`SceneViewportPanel` owns a `Viewport`, not a `SceneRenderer`.** It keeps its `EditorCamera`,
  its `m_SettingsDirty`, and its `m_Exposure`/`m_BloomIntensity`, but the renderer, sampler, the
  debounced resize, and the `Execute`/barrier move behind the `Viewport`:
  - `OnUI` reads `UI::ContentRegionAvail()` (and the panel's window position, in framebuffer pixels)
    and calls `viewport.SetRegion({ .Offset = panelOrigin, .Extent = panelSize })` — the viewport
    owns the debounced resize, so the panel's own `m_PendingExtent` plumbing goes away. It computes
    the camera as today (producing `m_View`, the one-frame-latency source) and pushes it:
    `viewport.SetViewState({ .World = m_Ctx.Scene, .Camera = m_View, … })`. `Configure` is requested
    on the viewport when dirty, and the ImGui texture is re-fetched from `viewport.GetOutput()` after
    a region resize or `Configure` invalidates it — the same re-fetch rule, now reading the viewport.
  - **`OnRender(cmd)` loses its scene-execute body** — the engine renders the registered viewport.
    The panel no longer records the scene render itself.
  - The toolbar's debug-view/battery controls drive `viewport.Configure`; the Stats-style readouts
    read `viewport.GetRenderer()`. The region the panel now feeds the viewport is what Plan 06's
    `ScreenToWorldRay` mapping consumes for entity picking (wired into selection in a later editor
    pass, not here; this plan does not call the Plan 06 API, so the two stay independent).

- **`EditorPanel::OnRender` is retired as the scene-render seam.** The engine drives every viewport,
  so the "record-my-scene-here" contract documented in `editor/CLAUDE.md` is replaced by
  "own-a-`Viewport`-and-register-it." Every `OnRender` override is accounted for in this pass:
  - `SceneViewportPanel::OnRender` — its scene-execute body is removed.
  - `AssetEditorPanel::OnRender` — the forwarding chain that calls `child->OnRender(cmd)` is removed
    for viewport-owning children (so a preview is never both centrally driven *and* forwarded —
    nothing double-driven).
  - `MaterialEditorPanel` / `MaterialPreview` — see below.

  `EditorPanel::OnRender` itself is **removed**, not narrowed: the survey covers every current
  override and none records non-viewport out-of-graph work that needs a pre-ImGui host step. (If a
  future panel does, it is a separately-added host-ordered hook, not this contract.)

- **Registration through the host, cleanup by RAII.** `EditorHost` *is* an `Application`, so it owns
  `RegisterViewport(Viewport&)` directly. A render-owning panel reaches it by **constructor injection
  of an `Application&`** — the same mechanism panels already use for `Context&`/`AssetManager&`/
  `ImGuiLayer&` today (the base `EditorPanel` holds no host reference, and gains none). The host
  threads `*this` into the per-asset-type panel factories (which already forward `m_Context`/
  `m_Assets`/`m_ImGui`); each render-owning panel's constructor takes the `Application&` and stores
  it, then **holds a `Unique<Viewport>` member** (`Viewport::Create`) and registers it on
  construction/open (`m_App.RegisterViewport(*m_vp)`). Closing a document destroys the panel, the
  member drops, and `~Viewport` removes the engine's pointer automatically (no explicit unregister in
  any panel destructor). Closing a document takes its viewports out of the drive-list (the "down to
  zero" case); opening a second prefab editor adds another set. Only render-owning panels
  (`SceneViewportPanel`, `PrefabEditorPanel`, `MaterialEditorPanel`) take the `Application&`;
  non-rendering panels (`AssetBrowserPanel`, `ConsolePanel`) are untouched. The host registers no
  `Presented` viewport, so the composite runs with **zero placements**.

- **`MaterialPreview` and the previews** follow the same move, with one wrinkle: `MaterialPreview` is
  **not** an `EditorPanel` — it is a helper owned by `MaterialEditorPanel` and driven today through
  the panel's `OnRender` forwarding. It holds an `Offscreen` `Viewport` (`Viewport::Create`) and is
  fed its region/view as before, but registration is threaded down through its owner:
  `MaterialEditorPanel` (which now takes the `Application&`) calls `RegisterViewport(*preview.viewport)`
  on the preview's behalf, and the forwarding `OnRender` for it is removed. Its output is sampled into
  its ImGui image via `GetOutput()`.

## Decisions

1. **The panel owns the `Viewport`; the host drives it.** Lifetime stays with the document (close
   ⇒ destroy ⇒ unregister), but the render is central. This is the literal meaning of the
   central-list decision: central *driving*, local *ownership*.

2. **The one-frame latency is preserved, not fixed — and now covers the region too.** The editor
   camera *and* the panel's content rect are computed in the UI pass and consumed by the engine
   render that already ran this frame — the existing, documented behavior, now applying to the region
   as well as the camera. Pushing both through `SetRegion`/`SetViewState` changes nothing about the
   timing; it routes the same values through the viewport. The gather reads the same `GetRegion()`
   the producing `Render` used, so a mid-resize-drag frame never places a texture into a newer rect.

3. **Zero-placement editor composite.** The dockspace is opaque, so the editor needs no scene plate
   behind ImGui — it registers no `Presented` viewport and relies on Plan 01's empty-placement path.
   This drops the editor's reason to keep a "primary" scene render aimed at the swapchain.

4. **`EditorPanel` loses a responsibility, not its existence.** The base class stays (title, `OnUI`,
   the dockspace seams); only the "record your scene render in `OnRender`" duty is removed, since
   the engine now owns that for every viewport uniformly.

## Files

| File | Change |
|---|---|
| `editor/src/panels/SceneViewportPanel.{h,cpp}` | Take an `Application&` in the constructor; hold a `Unique<Viewport>` (`Viewport::Create`) and register it (`m_App.RegisterViewport(*m_vp)`); `OnUI` feeds `SetRegion` from the content rect, pushes `SetViewState`, requests `Configure`, re-fetches the texture; remove the `Execute`/barrier from `OnRender`. |
| `editor/src/material/MaterialPreview.{h,cpp}` *(and the other previews)* | Hold an `Offscreen` `Viewport` (`Viewport::Create`); feed the region; push the preview view. (Registered on its behalf by its owning panel — `MaterialPreview` is not an `EditorPanel`.) |
| `editor/src/panels/MaterialEditorPanel.{h,cpp}` | Take an `Application&`; register the preview's viewport on its behalf; drop the `OnRender` forwarding to the preview. (`PrefabEditorPanel` similarly gains the `Application&`.) |
| `editor/include/VengEditor/EditorPanel.h` | Remove the scene-render `OnRender` seam; update the doc. (Base class still holds no host reference.) |
| `editor/src/EditorHost.{h,cpp}` | Thread `*this` (`Application&`) into the render-owning panel factories alongside the existing `m_Context`/`m_Assets`/`m_ImGui` forwarding; **delete** the hand-rolled present path (`m_BlitPipeline`, `m_PresentGraph`, `BuildPresentGraph`, the `OnRender` override, its swapchain callback) and run the engine render phase + managed gather/composite (zero placements). |

## Verification

- Clean build; `ctest` green.
- The editor exe runs (manual): open the sample prefab and level documents — the viewport renders
  through the engine drive-list; debug-view, battery toggles, camera fly, and Play/Stop behave as
  before. Open a second document (second viewport in the list), close it (back down), close all
  (zero viewports, zero-placement composite still draws the dockspace).
- The material editor's preview renders through its registered `Viewport`.
- `validation_gate` green under `build-debug` running the editor path — no unbound descriptor from
  the zero-placement composite, no double-driven viewport.
- `smoke_golden` / `hello_triangle_launcher_smoke` unaffected (the editor migration touches no
  runtime sample path).

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
