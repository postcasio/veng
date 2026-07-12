# examples — the two co-migrated consumption exemplars

The engine ships **two** sample game modules. Together they are the dual-mode conformance check:
**every breaking engine change migrates both in the same pass** (the Working norms rule in the
[root CLAUDE.md](../CLAUDE.md)), and they are the **only** consumption exemplars a veng test,
example, or golden may depend on. Both are a **game module + launcher** built by `veng_add_game`
(see [engine/CLAUDE.md](../engine/CLAUDE.md)), each with a root `project.veng` listing its
pack(s) under `assets/` and its per-platform `*.buildcfg` ship targets under `configs/`
(macOS / Windows / Linux).

## `hello-triangle/` — the maximal sample, consumed in-tree

The canonical **maximal** sample and the smoke test: every renderer battery, the full debug UI, a
cooked prefab/level world, and an opt-in multiplayer mode (the networking consumption exemplar —
see [engine/src/Net/CLAUDE.md](../engine/src/Net/CLAUDE.md)). It is the **in-tree** consumption
exemplar, built as part of the engine tree via `add_subdirectory`.

- `veng_add_game` builds `libhello_triangle` (shared, the app) plus `hello_triangle-launcher`
  (the exe that `dlopen`s it).
- **It is the live consumer of the flat-peer-world path.** Windowed and offline, it configures a
  second managed viewport — a corner picture-in-picture (viewport 1) — and in `OnWorldLoaded` opens
  a **second world** through `GetWorldRunner().OpenWorld` spawning the same startup level, binding it
  to that viewport (`SetViewportWorld(1, …)`). The single `WorldRunner` then ticks both worlds each
  frame and each managed viewport pulls its own world's camera, so the two worlds run as flat peers
  (their spinners drift apart as each ticks on its own clock) — the multi-world path exercised by a
  real app, not only the tests. Smoke configures a single viewport and opens one world, so the
  golden capture (viewport 0's output) is byte-identical; net launches skip the second world so the
  hosted/joined world stays the sole world. Two views is well within the fixed 16-simultaneous-view
  ceiling (`MaxViewsPerFrame` / `MaxPresented`).
- The `HT_SMOKE` capture and the `smoke_golden` / `hello_triangle_launcher_smoke` tests are the
  verification floor — the runbook (including golden regeneration) is in the
  [root CLAUDE.md](../CLAUDE.md), "Verification".
- Its MCP wiring (`StartMcpServerIfRequested`, env-gated behind `HT_MCP`; the fixed-port
  `hello_triangle-run` / editor convenience targets) is the worked MCP reference — see
  [mcp/CLAUDE.md](../mcp/CLAUDE.md).

## `template/` — the minimal sample, consumed out-of-tree

The smallest correct app a new developer copies, and the **out-of-tree** consumption exemplar: a
**standalone** project that discovers veng with `find_package(veng)` and is **removed from the
engine build** (`add_subdirectory(template)` is not called) — only the SDK conformance tests
(`sdk_conformance_install` / `sdk_conformance_buildtree`, the `gpu` band) build it, so a template
breakage surfaces there, not in a plain `cmake --build`. It has no smoke/PPM path; its
conformance tests configure + build it standalone and probe `veng-editor --version`.

The engine bootstraps everything from cooked data — it reads the cooked project, mounts the packs
it names, loads the **startup level** (a world `Prefab`: a `Camera`, a directional `Light`, a
cube whose mesh is an inline `CubeShape` recipe and which carries a `ConstantMotion` to spin, a
**`GuiSurface`** diegetic panel, a **`CaptureSurface`** mirror, and a screen-space **`GuiOverlay`**
HUD), owns the running scene + simulation, ticks the level's system set (the engine
`ConstantMotionSystem`), and pushes the resolved camera each frame — the cube, panel, mirror, and
HUD are authored data driven by the engine, not built in code. On top of that, `main.cpp` layers a
**thin `Application` subclass** doing the two things data cannot:

- it binds the primary `GuiOverlay` HUD its view-model (the one thing the engine cannot do from
  data alone), and
- it opens a **secondary overlay level** on a key through `LevelOverlay` — a preset over
  `WorldRunner::OpenWorld` that opens an owned, runner-ticked world plus the overlay policy. The
  overlay is a live sub-scene with its own input seat, its own `systems` (the builtin
  `DeviceAssignmentSystem` / `InputMappingSystem` plus its one driving system), and an `Interactive`
  `GuiOverlay` HUD with an `onClick` button that dismisses it, populated at open through
  `LevelOverlayInfo::Populate` with a snapshot of the primary scene's state (dismissable by the key
  or the button, the covered world named by `CoveredWorld` frozen by a refcounted
  `WorldRunner::PauseScope` for the overlay's lifetime). The runner ticks the overlay world and the
  engine pushes its camera, so the opener writes no per-frame overlay code — only the dismiss drain.

So the module registers a HUD view-model type, two small overlay components, and one
overlay-driving `SceneSystem` — the least game code that still exercises `GuiOverlay` binding and
`LevelOverlay` end to end. Its pack carries the prefabs + levels, so the cook reflects
`libtemplate` via `MODULE template` (its overlay components + system beside the engine builtins).

## Graph-sourced sample shaders

Both samples' fragment shaders are **graph-sourced**: hello-triangle's `brick` and the template's
`flat` fragment each name a `*.frag.graph.json` node graph as their `*.shader.json` source (the
authored graph, no hand-authored `.slang`), cooked into SPIR-V by the shared `veng::graph` emit
walk ([graph/CLAUDE.md](../graph/CLAUDE.md)). The core engine shaders (`tonemap`, `surface.vert`,
the lighting and post passes) stay hand-authored `.slang`: the embedded core pack is cooked by the
veng-free `veng_cook_bootstrap` that breaks the `veng → core-pack cook → cooker → veng` cycle, and
that bootstrap cannot link the `veng::graph` walk (which links `veng::veng`) — so a core shader
cannot be graph-sourced.
