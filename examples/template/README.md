# template

The minimal veng game: the smallest app that opens a window and renders a lit,
rotating cube. **Copy this directory to start a new veng game.**

Where `hello-triangle` is the engine's *maximal* sample — every battery, the full
debug UI, multiple build configurations — this is its *minimal* counterpart: no
debug UI, no tuning panels, no custom component or system, the smallest correct
app. Both are migrated in the same pass as every breaking change, so the template
is the standing check that a newcomer's starting point still compiles and runs.

`main.cpp` is the whole game, read top to bottom:

- mounts the template's cooked pack (`ExecutableDirectory()`-relative, so the
  launcher + module + pack move as one directory);
- builds its world **in code** — three entities: a `Camera`, a directional
  `Light`, and the cube;
- the cube's mesh is a **primitive recipe** (`CubeShape`) built into a resident
  `Mesh` with `AssetManager::BuildSync<Mesh>` — no cooked mesh asset, no `.obj`;
- **rotates the cube inline in `OnUpdate`** by mutating its `Transform.Rotation`;
- opts into the engine-owned **managed primary viewport**
  (`ApplicationInfo::ManagedViewport`) and pushes the resolved camera each frame
  via `GetPrimaryViewport()->SetViewState(...)` — the plug-and-play render path,
  with no `SceneRenderer`, composite, or ImGui wiring of its own.

The asset pack (`assets/`) is one trivial flat surface material plus its fragment
shader; the cube is a recipe and the world is built in code, so there is nothing
else to cook. It ships **no `project.veng`**, cooking via the zero-config codec
default — the template demonstrates the smallest app, not the build-configuration
surface.

Build and run (from the repo root):

```sh
cmake -S . -B build
cmake --build build --target template-launcher
./build/examples/template/template-launcher
```

Smoke-test mode: set `TEMPLATE_SMOKE=<path.ppm>` and the app renders a few frames
at a fixed angle, dumps the scene image to that path as a binary PPM, and exits 0
— the headless path the `template_launcher_smoke` ctest drives.

```sh
TEMPLATE_SMOKE=/tmp/cube.ppm ./build/examples/template/template-launcher
```

For the richer surface — the batteries, the debug UI, build configurations, the
ECS/systems/prefab/level story — read `examples/hello-triangle` and the editor's
**Project Settings** panel.
