# Wiring a level

A **`Level`** is the unit you assemble a playable game from. It is a thin asset
that references a **world prefab** and adds the data that is *not* reusable-recipe
data: the ordered active system set, the game-mode config, and render settings.
Loading a level starts the game — it spawns the world, builds the simulation from
the system set, and seeds the game-mode `Session`. That is authored data, not
`main.cpp` code.

This guide is the `Level` concept from the author's side. For how to write the
systems a level activates, see
[Writing gameplay systems](writing-gameplay-systems.md).

---

## Why a level is not a prefab

A **prefab** is a *reusable recipe*: a tree of entities and components that
spawns the same way every time, anywhere, any number of copies. A crate, a
character, a whole environment — each is a prefab you can instantiate freely.

A **level** is the *once-loaded playable unit*: this world, with this game mode,
running this set of systems, rendered with these settings. Loading it twice does
not make sense the way spawning a prefab twice does.

veng keeps these distinct rather than overloading "prefab" to mean both. A level
**references** a world prefab by id and **reuses** prefab serialization for the
world — it does not embed a second copy of the world format. The world stays a
reusable recipe; the level wraps it with the level-scoped wiring around it. (It is
named `Level`, not `Scene`, to avoid colliding with the runtime `Scene` — the ECS
world a level spawns *into*.)

---

## What a level holds

The asset is
[`engine/include/Veng/Asset/Level.h`](../../engine/include/Veng/Asset/Level.h),
and its `*.level.json` source carries four pieces. hello-triangle's
[`sample.level.json`](../../examples/hello-triangle/assets/levels/sample.level.json):

```json
{
  "world": 11611391513566245589,
  "systems": [
    8128120177403478945,
    2030943125819365810,
    4323789676874032403,
    13095149762400407006,
    5673716953921245912
  ],
  "gameMode": {
    "PlayerPrefab": 13493236524696338033,
    "ScoreToWin": 0
  },
  "render": {
    "Exposure": 2.5,
    "Bloom": true,
    "BloomIntensity": 1.5,
    "Shadows": true,
    "AO": true
  }
}
```

- **`world`** — the `AssetId` of the world prefab the level spawns. The level keeps
  this handle resident for its lifetime, and the prefab's embedded asset references
  (meshes, materials) resolve as ordinary load-time dependencies.
- **`systems`** — the ordered active `SystemId` set, in run order. These ids name
  registered systems from the `SystemRegistry` catalog; the simulation runs exactly
  this set, in this order (honoring the Sim/View phase split). The ids in JSON are
  decimal; in C++ they are uppercase-hex `0x…ULL` literals. The five above are, in
  order, the spawn rule, control, movement, spinner, and camera-rig systems.
- **`gameMode`** — the `GameModeConfig` seeded onto the `Session` entity at load:
  the `PlayerPrefab` a spawn rule instantiates and the mode parameters
  (`ScoreToWin`) the rule systems read. Selecting a different mode is choosing a
  different config plus a different registered rule set — no C++ path picks the
  mode.
- **`render`** — a `LevelRenderSettings` subset of the renderer's knobs (exposure,
  bloom, shadows, SSAO). These are a *reflected, tolerantly-serialized* struct, not
  a renderer type — a new field never invalidates existing level blobs, and the
  renderer stays untouched. The app maps them onto its `SceneRendererSettings` and
  per-frame `SceneView` at load.

The game mode itself is **rule systems over a `Session` component** — not an
object, no registry, no ABI bump. The `Session`
([`Components.h`](../../engine/include/Veng/Scene/Components.h)) holds the mode's
phase/score/timer; a spawn-on-start rule, a scoring rule, a win-condition rule are
just `Phase::Sim` systems reading and writing it. hello-triangle's
[`SpawnPlayerRule`](../../examples/hello-triangle/main.cpp) is the worked example:
at `OnStart` it spawns the config's `PlayerPrefab` when the `Session` is `Playing`,
and tears it down at `OnStop` or when the session ends.

---

## The load-to-play flow

Loading a level and starting play is a short, explicit sequence. From
hello-triangle's `OnInitialize`
([`main.cpp`](../../examples/hello-triangle/main.cpp)):

```cpp
// 1. Load the level like any other asset.
const AssetResult<AssetHandle<Level>> level =
    GetAssetManager().LoadSync<Level>(AssetId{0x95C2E76206A11F08ULL});
VE_ASSERT(level.has_value(), "{}", level.error().Detail);

// 2. Seed the renderer from the level's render subset, before the renderer exists.
const LevelRenderSettings& render = level->Get()->GetRender();
m_SceneSettings.Bloom = render.Bloom;
m_SceneSettings.Shadows = render.Shadows;
m_SceneSettings.AO = render.AO;
m_Exposure = render.Exposure;
m_BloomIntensity = render.BloomIntensity;

// ... create the SceneRenderer with m_SceneSettings ...

// 3. Start the game: spawn the world, build the simulation, seed the Session.
LevelInstance instance = level->Get()->LoadInto(GetAssetManager(), GetSystemRegistry());
m_Scene = std::move(instance.World);
m_Simulation = std::move(instance.Simulation);

// 4. Start the simulation (the spawn rule fires here, at OnStart).
m_Simulation->Start(*m_Scene,
                    SystemContext{.Assets = GetAssetManager(), .Input = GetInput()});
```

`Level::LoadInto(AssetManager&, const SystemRegistry&)` does the assembly:

1. creates a fresh `Scene`,
2. spawns the world prefab into it (`Prefab::SpawnInto`),
3. builds a `SceneSimulation` from the level's ordered `SystemId` set, resolving
   each id against the catalog and honoring the Sim/View phases, and
4. creates one `Session` entity carrying a `Playing` `Session` plus the level's
   game-mode config.

It returns a `LevelInstance { Unique<Scene> World; Unique<SceneSimulation> Simulation; }`
— the bundle the app owns and drives. The simulation comes back *not yet started*;
the caller calls `Start`. From there the app ticks `m_Simulation->Update(...)` each
frame and renders `m_Scene` — the same `SceneSimulation` driver the editor's Play
mode uses, so a level plays identically in the editor and the shipped runtime.

---

## Authoring a level in the editor

The `LevelEditorPanel`
([`editor/src/panels/LevelEditorPanel.h`](../../editor/src/panels/LevelEditorPanel.h))
is the authoring surface. It derives from the prefab editor, so the world prefab is
edited through the full scene surface (viewport, explorer, inspector) without
reimplementing scene editing — and it adds two level-scoped children:

- a **systems panel** listing the `SystemRegistry` catalog with an enable toggle,
  drag-reorder over the active set, and phase labels — writing the level's ordered
  `systems` list, and
- a **settings panel** drawing the `gameMode` and `render` config through the
  shared reflection inspector.

Editing the level recooks the `*.level.json` off the render thread and hot-reloads
the asset behind its stable handle, round-tripping the JSON (patching the known
keys, preserving any unknown ones). **Play runs exactly the level's ordered system
set** — distinct from a bare prefab document, which Plays the "all registered"
convenience set.

That is the loop: lay out the world as a prefab, choose and order the systems,
tune the game mode and render settings, and Play. The level is where a game stops
being engine primitives and becomes an assembled, authored thing.

---

## Where to go next

- **[Writing gameplay systems](writing-gameplay-systems.md)** — the systems a
  level activates, the Sim/View phases, and the Input → Intent → Movement pattern.
- The generated API reference (`cmake --build build --target docs`) documents
  `Level`, `LevelInstance`, `Session`, and `GameModeConfig` in full.
