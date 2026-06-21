# Plan 06 — the Level asset

**Goal:** make wiring a game an **authored artifact**, not C++. A thin **`Level`** asset references
a world prefab and carries the level-scoped data — the game-mode/`Session` config, the ordered set
of systems to run, and render settings — and a **level loader** turns it into a running game: spawn
the world prefab into a fresh `Scene`, build a `SceneSimulation` from the level's system set (Plan
05's id-list construction), seed the `Session`. The sample app stops hand-spawning its world and
hand-registering its simulation in `OnInitialize` and instead **loads a Level**.

## Why it is its own plan

This is the keystone that turns Plans 00–05's primitives into something you *wire up* rather than
hardcode. It is the home the level-scoped data from Plan 04 (the game mode) and Plans 02/03/05 (the
system set) has been missing. It depends on all of 00–05 and is the integration point they converge
on. It is split from the **system catalog** (Plan 05) so the new on-disk format, the loader, the
cooker importer, and the sample migration are reviewed apart from the engine-only registry rework
the catalog already landed and verified.

## The prefab dual-purpose problem this fixes

A prefab today is both a **reusable recipe** (instanced N times, carries no singletons) and **the
loaded world** (loaded once, carries singletons — the camera/`Viewer`, the `Session`,
environment). Those are different identities, and conflating them leaves level-scoped data —
which game mode runs, which systems are active, render settings — with no home but the app's C++.
The **`Level`** is that home, and it does it by *reference*, reusing the prefab machinery rather
than duplicating it (the confirmed wrapper-by-reference model, not Unity-style embedding).

```
Level (AssetType::Level, cooked from a .level.json)
├─ World    : AssetId   → the root prefab (the layout; still edited in the prefab surface)
├─ GameMode : Session config (which rule set + params)        ← Plan 04's data, hoisted here
├─ Systems  : ordered [SystemId]  (which systems run, in order, within the Sim/View phases)
└─ Render   : SceneRendererSettings + environment             (incremental; a first-cut subset)
```

It is named **`Level`**, not `Scene`, because `Scene` is the runtime ECS world (scope: avoid the
collision).

## What lands

- **The `Level` asset.** `AssetType::Level` appended to the enum (after `Prefab`,
  [AssetType.h:26](../../assetpack/include/Veng/Asset/AssetType.h:26)); a `CookedLevelHeader` +
  blob in `assetpack`. A `*.level.json` source carries `world` (prefab `AssetId`), `gameMode`
  (the `Session`/rule config — Plan 04's `GameModeConfig` data), `systems` (an ordered list of
  `SystemId` — Plan 05's ids), and a `render` subset. It loads through the identical
  `AssetManager::Load`/`LoadSync` path as every other asset; its `world` prefab and any embedded
  asset refs resolve as ordinary load-time dependencies. The game-mode/render config is embedded via
  the **tolerant reflection record** (name-keyed `WriteFields`/`ReadFields`, like prefab
  components), so adding a `SceneRendererSettings` field does not invalidate existing `.level`
  blobs.

- **The level loader.** A runtime `Level` type (the loaded asset) plus a load-into-play path:
  given an `AssetManager` and a fresh `Scene`, it spawns the `world` prefab (`Prefab::SpawnInto`),
  constructs a `SceneSimulation` from the level's **ordered `SystemId` set** (Plan 05 resolves ids →
  factories, in the level's order, honoring the Sim/View phases), and creates + seeds the
  **`Session`** entity from the level's `gameMode` config. It returns the bundle the app drives (the
  `Scene` + the `SceneSimulation`). This is "starting the game."

- **The cooker validates the level.** A `LevelImporter` (cooker-side) validates the `.level.json`
  against the `dlopen`ed module's **registered systems** (each `systems` id must resolve) and the
  referenced **world prefab**, and validates the `gameMode` config as a reflected struct — exactly
  the way `PrefabImporter` validates components against reflected types, reusing the same
  module-reflection relaxation. The cook host constructs and populates a `SystemRegistry` (via the
  same `VengModuleRegister` call) so the importer can resolve `systems` ids — extending the prefab
  relaxation to reflect a second registry (still GPU-free, the no-device contract holds). An unknown
  `SystemId`, a missing world prefab, or a malformed game-mode field is a located cook-time error.

- **The sample loads a Level.** `OnInitialize` stops hand-spawning `scene.prefab.json` and
  hand-constructing the `SceneSimulation`; it loads a new `sample.level.json` whose `world` is the
  existing scene prefab, whose `systems` names the sample's systems (control, movement, the
  rules/spawn, the camera rig), and whose `gameMode` carries the `Session` config. The app becomes
  *load this level, drive its bundle*.

## How this absorbs Plan 04's Session

Plan 04 establishes the `Session` + rule-system **mechanism** and bootstraps the `Session`/config
authored in the world prefab (the only place it *can* live before the `Level` exists). This plan
makes the **`Level`** the authoritative home for that game-mode config (it is level-scoped, not
world-content), and the **loader creates and seeds the `Session`** from it. So the sample's
game-mode config relocates once, from the world prefab to the level — Plan 04's mechanism is
unchanged and no rule-system code moves; only the JSON key's file changes. This one re-touch of the
sample is the accepted cost of landing the `Session` mechanism (Plan 04) before its level-scoped
home; the keystone is where level-scoped data belongs, not a second authoring system.

## Decisions

1. **`Level` wraps a world prefab by reference.** It reuses prefab serialization and the prefab
   editing surface; it does not embed world entities (the confirmed model). The layout stays a
   prefab; the `Level` adds the thin level-scoped layer.

2. **Selection/order comes from Plan 05; the `Level` stores it.** The `Level`'s `systems` list is
   the ordered `SystemId` set Plan 05's `SceneSimulation` construction consumes; system *params*
   stay components on world settings entities (Plan 05 decision 2). The `Level` adds no new config
   mechanism.

3. **The Level is the home for level-scoped data.** Game mode, active systems, render/environment
   — the data that is *not* reusable-recipe data — lives on the `Level`, seeded into the runtime
   `Scene`/`Session` at load. This is what makes "wiring a game" an authored artifact.

4. **Tolerant embedded config, not a fixed snapshot.** The `gameMode`/`render` config rides the
   name-keyed reflection record, so the actively-churning `SceneRendererSettings` can gain fields
   without a `CookedLevelHeader` version bump invalidating old blobs. A header `Version` frames the
   blob and is rejected on mismatch; the embedded reflected records evolve tolerantly within it.

5. **No module-ABI bump.** The `Level` is an asset, the loader is engine code, the importer reuses
   the existing module-reflection relaxation. Nothing touches `VengModuleHost` /
   `VENG_MODULE_ABI_VERSION`.

## Files

| File | Change |
|---|---|
| `assetpack/include/Veng/Asset/AssetType.h` | Append `Level` (after `Prefab`). |
| `assetpack/include/Veng/Asset/CookedBlobs.h` (or matching) | `CookedLevelHeader` + the level blob layout (world id, system-id list, tolerant game-mode + render reflection records). |
| `engine/include/Veng/Asset/Level.h` + loader (`engine/src/…`) | The runtime `Level` type + the load-into-play path (spawn world, build sim from the id set, seed `Session`). |
| `cooker/src/Importers/LevelImporter.*` | Validate `.level.json` against the module's systems + the world prefab + the reflected game-mode config; the cook host populates a `SystemRegistry` for id resolution. |
| `examples/hello-triangle/assets/sample.level.json` + manifest + `main.cpp` | A level referencing the scene prefab; the app loads it instead of hand-spawning + hand-registering the sim. |
| `tests/…` | Level cook/load round-trip; loader builds the named system set + seeds the `Session`; an unknown `SystemId` is a cook error. |

## Verification

- Clean build; `ctest` green across the unit/death/cooker bands and the `gpu` band where present.
- **Cooker:** `vengc cook --module` cooks `sample.level.json`, validating its system ids + world
  prefab + game-mode config; an unknown id / missing prefab / bad field is a located error
  (a cooker test pins this).
- `hello_triangle_launcher_smoke` exits 0 and writes a correct-sized PPM through the full
  `dlopen` → **load level** → spawn world → build sim → render chain.
- `smoke_golden`: unchanged if the level loads the same world + camera the sample authored before
  (the world prefab is identical; only the load path differs). Re-bless only if the loader's
  spawn/Session seeding moves the capture, with the change stated.
- The relocatable trio still resolves and runs (launcher + lib + pack, now including the level).

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
