# planset-34 — grab bag: resolution, meshes, residency, editor play, gizmos

**Phase goal:** a coherent grab bag of six fixes and one real architectural change, drawn from
friction found while playing the sample and driving the editor. Three threads:

1. **Render-allocation honesty** — lift the hardcoded half-resolution cap so the managed viewport
   can render at native HiDPI and the allocation-tier outer loop discovers the operating point on
   its own (planset-32's intent, undone by a too-conservative sample default).
2. **Meshes are meshes** — retire the special-cased two-pass `Primitive` resolve. A procedural
   primitive becomes a mesh whose *source* is an inline recipe, resolved through the ordinary async
   load path (the Godot `PrimitiveMesh : Mesh` model). Residency-on-spawn then falls out as a
   uniform, scoped `ResidencyBatch` the engine owns — no whole-scene poll, no game-side wait loop.
3. **Editor + engine ergonomics** — pull the sample's debug UI into reusable `Veng::UI` panels,
   make editor Play actually drop into a playable state, and give the editor a way to see non-mesh
   objects (lights, cameras) through a new engine debug-draw API.

## The architectural pivot — primitives load as meshes

Today a `Primitive` is a component carrying a shape recipe; a `SpawnResolve` thunk
([`Resolve.cpp`](../../engine/src/Scene/Resolve.cpp)) builds the mesh through
`AssetManager::Build<Mesh>` (already an async pending handle) and writes it into a **separate**
`MeshRenderer.Mesh`. That thunk is fired by a distinct post-populate pass in
[`Prefab::SpawnInto`](../../engine/src/Asset/Prefab.cpp) (pass 2b) and re-fired by
`ResolveComponents` from every editor mutation path — a standing "remember to resolve" hazard, and
a residency model that does not compose (the built handle is produced in a side pass; a primitive
spawned by a Sim system at `OnStart` escapes any pre-spawn residency wait).

The three major engines converged differently: Unity's standard primitives are **baked shared mesh
assets**; Unreal splits **baked StaticMesh** from a **component-owned DynamicMesh**; **Godot** makes
the parametric primitive a **subclass of `Mesh`** sitting in the one `MeshInstance.mesh` slot, recipe
on the resource, regenerated in place — no separate component, no two-pass, fully editable. The
Godot model is the literal realization of "primitives load in as meshes," so veng adopts it:
`MeshRenderer.Mesh` stays the one `AssetHandle<Mesh>`, but a mesh handle's **source** is
`cooked AssetId | inline recipe`, both resolved to a pending handle through the load path. The
`Primitive` component and the `SpawnResolve` pass are retired (Primitive is the seam's only rider).

A **mutable** mesh (runtime sculpting, voxel/marching-cubes terrain, CSG/destruction,
gameplay-generated geometry) is a genuinely different capability — the vertex buffer is the source
of truth, not a recipe — and requires a mutable `Mesh` the engine does not have. It is **deferred**
to a new future area (16, [`future/dynamic-meshes.md`](../future/dynamic-meshes.md)) to design
against a real consumer; the single-mesh-slot model adopted here is exactly what keeps that door
open.

## Plans

| # | Plan | Summary | Status |
|---|---|---|---|
| 00 | Lift the resolution cap | Drop the sample's `0.5` override of the default `1.0` `MaxAllocationScale` (the engine default is already `1.0`); let the allocation-tier outer loop discover the operating point. Re-frame the engine docs/comments that justified `0.5`. Smoke golden unaffected (already `1.0`). | done |
| 01 | Mesh-source unification | A mesh reference's source is `cooked AssetId \| inline recipe`, both resolved to a pending `AssetHandle<Mesh>` through the load path during the populate pass. Retire the `Primitive` component and the `SpawnResolve`/`VE_RESOLVE` pass (2b). Migrate the sample prefabs and the `*_resolve` test suites; migrate both module guides. Finalize the already-drafted `future/dynamic-meshes.md`. | done |
| 02 | Residency on spawn | Break `Prefab::SpawnInto` → `SpawnResult { Roots, Pending }`; a `ResidencyBatch` with `IsResident()` / progress / blocking `WaitResident(TaskSystem&)`. `LevelInstance` surfaces it. Delete the sample's `WaitForPrimitiveResidency`. Depends on 01. | done |
| 03 | À-la-carte debug UI | Extract `UI::RendererStatsPanel` / `UI::FrameTimeGraph` / `UI::RenderSettingsEditor` into `Veng::UI`; migrate the sample to consume them. Sequences after 00–02 (all rewrite `main.cpp`). | proposed |
| 04 | Editor Play seeds the session | Factor session-seeding out of `Level::LoadInto` into a shared helper; add a `SeedPlayScene(Scene&)` hook the base `Play()` calls post-clone; `LevelEditorPanel` seeds `Session`+`GameModeConfig` and makes the player prefab resident. The game's `SpawnPlayerRule` then fires. Reads atop 02. | done |
| 05 | Engine debug-draw + gizmos | An immediate-mode `DebugDraw` accumulator (lines + textured billboards) flushed by a `ScenePass` inside `SceneRenderer`, depth-tested with a dim occluded fallback, gated by a `SceneRendererSettings` toggle. A new editor icon pack; `SceneViewportPanel` pushes a billboard per `Light`/`Camera`. | done |

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = implemented, migrated, verified, committed.

## Dependencies

- **00** is independent and small.
- **01** is independent and foundational; **02** depends on it (so a procedural mesh is an ordinary
  pending handle the residency walk captures, and `SpawnInto` is rewritten once).
- **03** touches `main.cpp` heavily, as do 00–02; sequence it last in that chain.
- **04** is **independent**: its core (seeding the session) stands alone, and its only Plan 02
  touchpoint — player-prefab residency — uses a `LoadSync` that needs nothing from 02 (it prefers 02's
  `WaitResident` if that has landed, but does not require it).
- **05** is independent and the heaviest; it lands last.

Dependent plans must build on the *prior plan's integration commit*, not `origin/main`. Per
[[project_megaexec_worktree_base]], `isolation: "worktree"` branches from `origin/main`, so it will
**not** see a locally-committed-but-unpushed base: dispatch **02** non-isolated against a manual
worktree cut from **01**'s integration commit, and **03** non-isolated against the 00→01→02 chain.
The independent plans (**00**, **01**, **04**, **05**) can use `isolation: "worktree"` directly. The
main.cpp-touching chain (00→01→02→03) merges in number order; 04 and 05 are engine+editor work that
merges cleanly beside it.

## The decisions this planset settles

- **The render allocation is honest about HiDPI.** The sample no longer clamps the allocation to
  half the backing extent. Native resolution is the default ceiling; the tier outer loop reclaims
  footprint under sustained load, which is its whole job. "Discover an appropriate max res" is the
  outer loop's, not a hand-picked cap's.
- **A mesh reference is one thing.** Cooked or procedural, it is an `AssetHandle<Mesh>` resolved
  through the load path. Primitives stop being special: no second spawn pass, no `SpawnResolve`
  machinery, no scattered `ResolveComponents`. Editing a recipe re-loads its mesh, the same shape as
  repointing a cooked asset field.
- **Residency is scoped and engine-owned.** A spawn yields a `ResidencyBatch` of exactly what it
  introduced — the request-returns-a-token model every mature engine uses — not a global
  "is the asset system idle" drain. The blocking wait (smoke/tooling) and the progress poll (a real
  loading screen) are the same handle.
- **Editor Play is playable.** Pressing Play drops you into the same initialized state
  `Level::LoadInto` produces at runtime, through one shared seed path.
- **Non-mesh objects are visible.** A general engine debug-draw API (lines + billboards) renders
  inside the deferred pipeline, depth-aware; the editor ships icons and drives light/camera gizmos
  on top of it. It is runtime-usable, not an editor-only overlay.
- **Mutable meshes are a named future, not built on spec.** The dynamic-mesh substrate waits for a
  concrete consumer (area 16); the model chosen here does not foreclose it.
