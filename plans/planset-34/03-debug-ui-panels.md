# Plan 03 — à-la-carte debug UI panels

**Goal:** pull the sample's hand-written **renderer** debug UI — the stats read-out, the frame-time
graph, and the render-settings editor — into reusable `Veng::UI` helpers a game composes, so they are
engine surface, not copy-paste per app. The game-specific UI (Scene window, input/capture chrome)
stays in the sample. Sequences after **00–02** (all rewrite parts of
[`main.cpp`](../../examples/hello-triangle/main.cpp)).

## What is being extracted

The sample's [`main.cpp`](../../examples/hello-triangle/main.cpp) draws the whole debug overlay by
hand against `Veng::UI`: a Stats window (allocation tier, cull funnel, draw counts, render scale), a
Frame Time graph (a GPU-frame-time ring + plot), and a forest of render-settings checkboxes/sliders
that mutate `SceneRendererSettings` and call `Configure`. None of it is reusable — the editor and any
future game re-author the same widgets.

## What lands

Three à-la-carte helpers under `Veng::UI` (`engine/include/Veng/UI/`,
[engine/src/UI/](../../engine/src/UI/)), each authored against the existing `Veng::UI` vocabulary and
imgui-free in its signatures (within the `include_hygiene` guarantee):

- **`UI::RendererStatsPanel`** — `RendererStatsPanel(const Renderer::Viewport&)`, reading the viewport's
  `SceneRenderer` for the read-only stats: allocation tier index + scale + auto/static, render scale,
  the cull funnel (`GetLastVisibleCount` → `GetFrustumSurvivedCount` → `GetLastDrawnCount`,
  `GetLastGpuSurvivorCount`), broadphase rebuild + node count. Pure read-out; no return.

- **`UI::FrameTimeGraph`** — a small **stateful** widget owning its GPU-frame-time ring buffer (the
  one ImGui-pattern departure from the stateless wrappers: a `FrameTimeGraph` value the caller holds
  across frames). `Push(f32 ms)` + `Draw()` (or a `Draw(viewport)` that reads
  `Context::GetLastGpuFrameTimeMs()` itself). Plots the history with min/avg/max.

- **`UI::RenderSettingsEditor`** — `[[nodiscard]] bool RenderSettingsEditor(SceneRendererSettings&,
  SceneView&)`, drawing the renderer toggles/sliders against the `SceneRendererSettings` and the
  per-frame `SceneView` knobs (exposure, bloom, the DRS/tier controls) in one panel. Returns the
  `changed` bool per the editable-widget idiom, so the **caller**
  decides whether to call `Viewport::Configure` (a topology change) versus just letting per-frame
  values ride — the engine helper does not own the reconfigure, matching the lifetime split where
  `Configure` is the owner's call.

## Decisions

1. **À-la-carte, not one `DebugOverlay`.** Three composable helpers a game arranges in its own
   windows, not a single turnkey call. A game wants to place the stats in its own window, skip the
   settings editor in a shipping build, or embed the frame-time graph in a profiler — composition the
   monolith would deny.

2. **The settings editor returns "changed"; the caller reconfigures.** `Configure` recreates
   resources and recompiles the graph — an owner-lifetime operation. The helper reports the edit and
   leaves the `Configure`/no-`Configure` decision to the caller, exactly as every editable `Veng::UI`
   widget returns a bool.

3. **The frame-time graph is the one stateful helper.** A history plot needs a ring buffer; rather
   than push that state onto the caller as a raw array, the helper is a small value type the caller
   persists — still imgui-free in its surface, consistent with the `Veng::UI` stateful-widget-class
   direction (future area 12).

4. **Scope is the renderer-facing debug UI.** The sample keeps its own game-specific UI (the Scene
   window, input/capture chrome). Only the renderer stats / frame time / render settings — the parts
   any game wants — move into the engine.

## Verification

Clean build, `ctest`, `smoke_golden` + `validation_gate` green (smoke is headless; the panels draw
only with ImGui on). The windowed sample shows the same stats/graph/settings, now via the engine
helpers, with the per-app UI noticeably thinner.
