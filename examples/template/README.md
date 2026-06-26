# template

The minimal veng game: the smallest app that opens a window and renders a lit cube,
with its world authored as **data** — a Level that references a world Prefab.
**Copy this directory to start a new veng game.**

Where `hello-triangle` is the engine's *maximal* sample — every battery, the full
debug UI, custom components and systems, gameplay — this is its *minimal* counterpart:
no debug UI, no custom component or system, the smallest correct app. Both are migrated
in the same pass as every breaking change, so the template is the standing check that a
newcomer's starting point still compiles and runs.

`main.cpp` is the whole game, and there is almost nothing to it: a single
`VengModuleRegister` that registers a **bare `Application`** (no subclass) with a
**managed primary viewport** and a **game world**:

```cpp
ApplicationInfo{
    .Name = "Template",
    .WindowInfo = { .Extent = {1280, 720}, .Title = "veng — Template" },
    .ManagedViewport = ManagedViewportInfo{},
    .World = GameWorldInfo{ .Project = "project.vengproj" },
}
```

The engine does the rest. With `World` set it reads the **cooked project**
(`ExecutableDirectory()`-relative, so the launcher + module + project + pack move as one
directory), mounts the packs it names, loads the **startup level** the project declares
(`project.veng`'s `startupLevel`), spawns its world **`Prefab`** into a `Scene`
that owns the level's `SceneSimulation`, and each frame ticks the simulation and pushes
the resolved camera into the managed viewport. So there is **no** lifecycle override, no
per-frame code, no `SceneRenderer`/composite/ImGui wiring — the world is authored as data
and driven entirely by the engine. A game that needs to customize the loaded world
overrides `Application::OnWorldLoaded`; the minimal one (this) overrides nothing.

The world is authored as **cooked assets** under `assets/`, not built in code:

- `prefabs/scene.prefab.json` — three entities: a `Camera` framed on the origin, a
  directional `Light`, and a cube whose mesh is an inline `CubeShape` recipe (built into
  a `Mesh` at spawn — no cooked mesh asset, no `.obj`) referencing the flat material;
- `levels/scene.level.json` — references the prefab as its `world`, with an empty
  system set and a small render subset;
- `materials/cube.vmat.json` + `shaders/flat.frag.slang` — the cube's flat surface
  material and its fragment shader.

Because the pack carries a prefab and a level, the cook reflects `libtemplate`'s types
and systems (`MODULE template` in `CMakeLists.txt`) — here only the engine builtins.
It ships a `project.veng` at the example root (listing its pack under `assets/` and a
`*.buildcfg` per ship target under `configs/` — macOS / Windows / Linux), so a copy starts
with the per-platform cook already wired: a bare `cmake --build` cooks the host-matching
configuration, and `cook-all-packs` builds them all. The texture codec each role resolves
to is edited in the editor's **Project Settings** panel.

Build and run (from the repo root):

```sh
cmake -S . -B build
cmake --build build --target template-launcher
./build/examples/template/template-launcher
```

For the richer surface — the batteries, the debug UI, build configurations, custom
components and systems, gameplay — read `examples/hello-triangle` and the editor's
**Project Settings** panel.
