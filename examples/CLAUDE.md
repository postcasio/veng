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
  hosted/joined world stays the sole world. Two views is well within both fixed ceilings — 32 view slots
  per frame (`MaxViewsPerFrame`) and 16 presented placements (`MaxPresented`).
- **It is the live consumer of nested prefabs.** The physics stack's five cubes are five nesting
  entities in `prefabs/scene.prefab.json`, each naming `prefabs/physics_cube.prefab.json` as its
  body and overriding only the `Name` and `Transform` that place it — the authored composition the
  scene previously spelled as five copies of the same mesh/rigid-body/collider subtree.
- **It is the live consumer of mesh sockets.** `OnWorldLoaded` loads `meshes/socket_slab.gltf`'s
  cooked mesh — a slab whose model authors two named empties — places it beside the physics stack,
  and parents a cube to its `Mount_Top` socket through `AttachToSocket`. Nothing in the C++ names
  the cube's place: the socket's authored position, orientation and scale are what the attach
  writes, so moving the empty in the model moves the cube. It is also the visual proof in the
  golden capture.
- The `HT_SMOKE` capture and the `smoke_golden` / `hello_triangle_launcher_smoke` tests are the
  tree's verification floor — see below, and the validation gate in the
  [root CLAUDE.md](../CLAUDE.md), "Verification".
- **It is the live consumer of both rumble paths.** The pad's A is bound to `Jump`, and the
  `ControlSystem` fires `assets/haptics/jump.rumble.json` as a one-shot on the seat each time Jump
  triggers (owned by its scene, so a pause holds it); the socket slab carries a looping
  `RumbleSource` (`assets/haptics/engine.rumble.json`, authored stopped) that the same system
  switches on while a seat drives the slab, played by the level's `HapticsSystem`. The Gamepads
  debug window offers the jump clip to play on any pad. Both clips are placeholders that touch the
  motors, not tuned effects — see [engine/src/Haptics/CLAUDE.md](../engine/src/Haptics/CLAUDE.md).
- **It is the live consumer of a generator source.** `OnWorldLoaded` adds an entity carrying an
  `AudioSource` whose runtime-only `Generator` is the sample's `ToneGenerator`, so the tone belongs to
  the world (held by its pause, stopped with it) while the app sweeps its frequency through the
  reference it keeps; the code-built `CreateClip` chirp on Key::G stays an application-scope one-shot.
- Its MCP wiring (`StartMcpServerIfRequested`, env-gated behind `HT_MCP`; the fixed-port
  `hello_triangle-run` / editor convenience targets) is the worked MCP reference — see
  [mcp/CLAUDE.md](../mcp/CLAUDE.md).

### The `HT_SMOKE` capture and the two smoke tests

**The `HT_SMOKE` capture is golden-checked.** Smoke mode renders a fixed pose
(`HelloTriangleApp::SmokeAngle`), so the capture is reproducible run to run; the windowed app still
rotates by accumulated wall-clock `delta`. The `smoke_golden` ctest renders the scene headless and
fuzzy-compares it against `tests/golden/hello_triangle_scene.png`
(`ctest --test-dir build-debug -R smoke_golden`). It is labelled `gpu` and skips cleanly with no
Vulkan ICD. The capture runs through the **launcher** (which `dlopen`s `libhello_triangle`), the
real shipping path. If a deliberate render change moves the capture, regenerate the golden:

```sh
HT_SMOKE=/tmp/ht.ppm build-debug/examples/hello-triangle/hello_triangle-launcher
sips -s format png /tmp/ht.ppm --out tests/golden/hello_triangle_scene.png
```

The capture is a 1280×720 RGB PPM (≈ 2,764,816 bytes).

**`hello_triangle_launcher_smoke` covers the shipping path automatically.** It runs
`hello_triangle-launcher` under `HT_SMOKE` and asserts exit 0 — the one test exercising the full
`dlopen` → `VengModuleRegister` → registry → `Run()` chain end to end. Labelled `gpu`
(`SKIP_RETURN_CODE 77`), it skips with no device and runs under the validation gate like the rest
of the `gpu` band. The launcher + lib + project + pack are a **relocatable set**: copy the
launcher, `libhello_triangle.*`, `project.vengproj`, and `sample.vengpack` into a fresh directory
and run from an unrelated working directory — everything resolves beside the launcher, so it still
writes a correct-sized PPM and exits 0.

## `template/` — the minimal sample, consumed out-of-tree

The smallest correct app a new developer copies, and the **out-of-tree** consumption exemplar: a
**standalone** project that discovers veng with `find_package(veng)` and is **removed from the
engine build** (`add_subdirectory(template)` is not called) — only the SDK conformance tests
(`sdk_conformance_install` / `sdk_conformance_buildtree`, the `gpu` band) build it, so a template
breakage surfaces there, not in a plain `cmake --build`. It renders no golden, so instead of a
smoke/PPM capture its conformance tests configure + build it standalone, **run
`template-launcher` under `TEMPLATE_SMOKE`** (windowless, a fixed handful of frames, exit 0) and
require the marker line it logs once its prefab-authored game-defined asset resolved, then probe
`veng-editor --version`.

The engine bootstraps everything from cooked data — it reads the cooked project, mounts the packs
it names, loads the **startup level** (a world `Prefab`: a `Camera`, a directional `Light`, a
cube whose mesh is an inline `CubeShape` recipe and which carries a `ConstantMotion` to spin, a
**`GuiSurface`** diegetic panel, a **`CaptureSurface`** mirror, a second, on-demand
`CaptureSurface` whose `Output` is `SceneLighting` — an environment probe lighting the scene, which
authors no `Sky` — and a screen-space **`GuiOverlay`** HUD), owns the running scene + simulation,
ticks the level's system set (the engine `ConstantMotionSystem`), and pushes the resolved camera each
frame — the cube, panel, mirror, probe, and HUD are authored data driven by the engine, not built in
code. On top of that, `main.cpp` layers a
**thin `Application` subclass** doing the three things data cannot:

- it binds the primary `GuiOverlay` HUD its view-model (the one thing the engine cannot do from
  data alone),
- it plays its composed `DemoSynth` generator from an `AudioSource` on an entity it adds to the
  world in `OnWorldLoaded` (a generator is runtime-only, so it is attached in code, not authored),
  driving the synth's cutoff from `OnUpdate` through the reference it keeps, and
- it toggles a **secondary overlay level** on a key by adding or removing a **`LevelOverlay`**
  component on an entity of the managed world; the engine opens the level as a world of its own and
  presents it over the managed one at the next frame. The overlay is a live sub-scene with its own
  input seat, its own `systems` (the builtin `DeviceAssignmentSystem` / `InputMappingSystem` and
  `ConstantMotionSystem`), and an `Interactive` `GuiOverlay` HUD whose driver stamps an
  `ExitRequest` in the overlay's scene from an `onClick` button — which ends the overlay, not the
  app, and takes the request away. The requesting entity carries a snapshot of the primary scene's
  state and names itself as the request's `Seed`, so the snapshot is copied into the overlay before
  it starts; `PauseOpener` holds the managed world paused for the overlay's lifetime. The runner
  ticks the overlay world and the engine pushes its camera, so the opener writes no per-frame
  overlay code at all.

So the module registers its HUD and emblem view-model types, the overlay's snapshot component, the
marker-beacon component, and two Gui drivers — the least game code that still exercises `GuiOverlay` binding and `LevelOverlay` end to
end. Its pack carries the prefabs + levels, so the cook reflects `libtemplate` via
`MODULE template` (its components beside the engine builtins).

## Graph-sourced sample shaders

Both samples' fragment shaders are **graph-sourced**: hello-triangle's `brick` and the template's
`flat` fragment each name a `*.frag.graph.json` node graph as their `*.shader.json` source (the
authored graph, no hand-authored `.slang`), cooked into SPIR-V by the shared `veng::graph` emit
walk ([graph/CLAUDE.md](../graph/CLAUDE.md)). The core engine shaders (`tonemap`, `surface.vert`,
the lighting and post passes) stay hand-authored `.slang`: the embedded core pack is cooked by the
veng-free `veng_cook_bootstrap` that breaks the `veng → core-pack cook → cooker → veng` cycle, and
that bootstrap cannot link the `veng::graph` walk (which links `veng::veng`) — so a core shader
cannot be graph-sourced.
