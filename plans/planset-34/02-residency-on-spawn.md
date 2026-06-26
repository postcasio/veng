# Plan 02 — residency on spawn

**Goal:** make the engine own "wait until what I just spawned is resident," scoped to exactly that
spawn. Break `Prefab::SpawnInto` to return a `SpawnResult { vector<Entity> Roots; ResidencyBatch
Pending; }`; surface the batch through `LevelInstance`; delete the sample's hand-rolled
whole-scene `WaitForPrimitiveResidency`. Depends on **Plan 01** (a procedural mesh is now an
ordinary pending `AssetHandle<Mesh>` the batch captures uniformly).

## Why a scoped batch, not a global drain

The sample's [`WaitForPrimitiveResidency`](../../examples/hello-triangle/main.cpp) pumps the task
system until every `Primitive`'s `MeshRenderer.Mesh.IsLoaded()` — polling the whole scene to
rediscover a set the spawn already knew. After Plan 01 that exact poll has no component to key on,
and the underlying need is narrow: the **cooked** dependencies of a prefab are already resident at
spawn (eager-resolved when the prefab handle loaded — [`Prefab.cpp`](../../engine/src/Asset/Prefab.cpp)
even *asserts* it), so the only pending work after a spawn is what the spawn **built** (a recipe
source's async `Build<Mesh>`).

A global `AssetManager::WaitForPendingBuilds()` is the wrong primitive: it blocks on unrelated
streaming and conflates "I need this now" with "this is intentionally streaming in." Every mature
engine returns a **token scoped to the request** — Unreal's `FStreamableHandle`, Unity's
`AsyncOperationHandle`, Godot's per-resource threaded-load status — and builds loading screens by
aggregating the tokens they care about. veng does the same: a spawn yields the batch of exactly what
it introduced.

## What lands

- **`ResidencyBatch`.** A thin value holding the `AssetHandle`s a spawn introduced that are not yet
  resident (kept alive while pending, since a handle is refcounted indirection). It offers
  `IsResident()`, a progress read-out (resident / total, for a loading bar), and a blocking
  `WaitResident(TaskSystem&)` that owns the pump-and-sleep loop the sample hand-rolls today. Empty
  batch ⇒ already resident. `WaitResident` carries **no timeout** — a handle that never becomes
  resident hangs the loop — so it documents that contract on its declaration; a watchdog that
  `VE_ASSERT`s after a bounded number of pumps (an abort is easier to diagnose than a silent hang) is
  the conservative form, given this loop is now reachable from editor Play and any loading screen, not
  just the smoke test.

- **`SpawnInto` returns `SpawnResult`.** Break the signature: `Prefab::SpawnInto(Scene&,
  AssetManager&) → SpawnResult { vector<Entity> Roots; ResidencyBatch Pending; }`. After the populate
  pass (which, post-Plan-01, is where recipe sources and cooked handles both resolve), `SpawnInto`
  walks the spawned components for `FieldClass::AssetHandle` fields — reusing the
  [`CollectHandleDeps`](../../engine/src/Asset/Loaders/PrefabLoader.cpp)-style reflection walk over
  the *live* components — and collects the not-yet-resident handles into `Pending`. The walk is
  uniform over every `AssetHandle` field, but in practice the cooked handles are already resident: the
  populate pass asserts each embedded cooked dependency is resident at spawn
  ([`Prefab.cpp`](../../engine/src/Asset/Prefab.cpp), the `is not resident` assert), so a not-yet-resident
  cooked handle aborts before this collection runs. `Pending` therefore holds the **recipe-built**
  handles a spawn introduced; the uniform walk is what makes that fall out without special-casing the
  recipe source, not a claim that cooked handles are ever pending here.

- **Every caller updated.** `Roots` replaces the old return at each call site:
  [`Level::LoadInto`](../../engine/src/Asset/Level.cpp), the editor's
  [`PrefabEditorPanel::BuildScene`](../../editor/src/panels/PrefabEditorPanel.cpp), the sample's
  `SpawnPlayerRule` ([`main.cpp`](../../examples/hello-triangle/main.cpp)), and the prefab/spawn
  tests. "We do things the right way" — the clean return shape over a backward-compatible companion.

- **`LevelInstance` surfaces the batch.** `Level::LoadInto` threads `SpawnInto`'s `Pending` into
  `LevelInstance { Unique<Scene> World; Unique<SceneSimulation> Simulation; ResidencyBatch Pending; }`
  ([`Level.h`](../../engine/include/Veng/Asset/Level.h)), so the runtime and the editor both get the
  world prefab's residency batch from one call.

- **Sample migration.** Delete `WaitForPrimitiveResidency`. The smoke path waits on the level
  instance's batch before the capture: `instance.Pending.WaitResident(GetTaskSystem())`. The windowed
  path ignores it (content streams in over frames, as it does today).

## The residency boundary (documented, not closed)

The batch is scoped to **one spawn**. Content spawned *later* by a Sim system — the player, spawned by
`SpawnPlayerRule` at `OnStart` — carries its **own** batch (its own `SpawnInto` call returns one),
owned by the spawning system, not the level's. Today the sample player uses a cooked, already-resident
mesh, so nothing waits on it; with Plan 01, a *primitive* player would be a pending handle the spawn
rule could surface and wait on. Making the level block on simulation-spawned content would drag
residency into the tick loop — explicitly out of scope. The boundary is the correct scoping (each
spawn owns its batch), recorded so it is a conscious choice, not an oversight.

## Decisions

1. **Break the signature.** `SpawnInto` returns `SpawnResult`; no `SpawnTracked` companion. The
   spawned-roots return is widely used, so every caller is touched — the clean shape is worth it.

2. **No global drain.** `AssetManager` gains no "wait for all builds." Residency is always a batch
   scoped to a spawn; a loading screen aggregates the batches it cares about.

3. **The batch holds handles, not tasks.** It snapshots `AssetHandle`s and polls `IsLoaded()`,
   reusing the engine's existing residency notion. `WaitResident` pumps `TaskSystem::PumpMainThread`
   so off-thread upload continuations land — the same loop the sample owned, now engine-side.

## Verification

Clean build, full `ctest` (prefab/spawn tests updated for `SpawnResult`), `smoke_golden` green (the
smoke capture is deterministic because the level batch is waited before capture),
`validation_gate` green. The launcher smoke writes a correct-sized PPM with no `WaitForPrimitiveResidency`
in the sample.
