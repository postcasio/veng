# planset-29 — gameplay control: cameras, input→intent, sim/view split, game modes, levels

**Phase goal:** build the gameplay layer — camera management, player control, game modes, and the
**authoring surface to wire an actual game** — **from ECS first principles**, shaped by veng's
data-oriented grain and by the **networking it anticipates**, not by another engine's actor
hierarchy. The runtime spine: a `Camera` entity selected per **`Viewer`** seat and resolved to a
`CameraView` by a pure function; an **Input → Intent → Movement** pipeline where input is captured
as data, translated to an abstract command, and consumed by gameplay; a **game mode** expressed as
**rule systems over a replicated `Session` state**, not an object; all held together by an explicit
**Sim / View tick split** and an **`Authority`** annotation (the two structural seams every
networking model needs, cheap now and brutal to retrofit). The authoring layer on top answers *how
you wire up a game*: gameplay systems are written in code and discoverable through a **catalog**, a
thin **`Level`** asset wraps a world prefab with the level-scoped wiring (game mode, the active
system set, render settings), an **editor surface** authors it, and a **human-readable guide** teaches
it. Takes up the gameplay + authoring half of
[future area 7 (systems)](../future/README.md#7-scene--entity-model--remaining-systems-perf-reflection-follow-ons)
and concretely shapes [future area 4 (event & input)](../future/README.md#4-event--input-systems).

**Two tiers:** Plans **00–04** are the runtime gameplay primitives (camera, control, phases,
game-mode mechanism); Plans **05–09** are the authoring/wiring layer (the system catalog, the
`Level` asset, the editor surface, the gameplay guide, the doc re-cut) that turns those primitives
into a game you assemble as data instead of hardcoding in `main.cpp`.

This planset was **re-cut from an Unreal-derived first draft** (a
`PlayerCameraManager`/`APlayerController`/`AGameMode` trinity, with a registered `GameMode`
type and a module-ABI bump). That draft was sound mechanically but imported actor-network
structure into an ECS, and froze the most net-model-specific layer (`GameMode`) ahead of any
net decision. The design below is **lighter** (no new registry, no ABI bump) and **more
net-ready** (the intent chokepoint, the sim/view split, the authority property), and it follows
the project's own rule for area 4: *let the requirements drive the shape*.

## The spine — four principles

1. **Roles are components + systems, never objects.** A player, a camera rig, a game mode is a
   *component marking an entity* plus a *system acting on it*. No controller/manager/mode class.

2. **Input → Intent → Movement.** Device/wire input is captured into a per-player **`PlayerInput`**
   component; a **control** system translates it into an abstract **`Intent`** command; a
   **movement** system (and gameplay systems generally) consume intent and mutate state. Intent is
   the serializable chokepoint a net layer predicts and rolls back, the uniform interface AI and
   remote players also write through, and what *enables* a deterministic sim — a pure function of
   `(state, intents)` *provided* Sim systems read only state and intents (the discipline Plan 08's
   guide spells out).

3. **Authority is a property, threaded early.** An entity carries who simulates/owns it
   (`Server` / `Local`). Model-agnostic, inert today, but the one annotation it is painful to add
   to a populated codebase later.

4. **Cameras are derived view, never authoritative.** A camera is computable from game state on
   any client, so it lives in the **View** phase, outside the deterministic/replicated **Sim**
   phase. Replicate the pawn; each client derives its own camera.

## The tick & authority map this produces

| Phase | System | Reads → Writes | Authority |
|---|---|---|---|
| Ingest | input capture | `Veng::Input` / wire → `PlayerInput` | local + remote |
| **Sim** | control | `PlayerInput` + `Possesses` → `Intent` | server (AI: server) |
| **Sim** | movement (gameplay) | `Intent` → pawn `Transform` | server + client-predict |
| **Sim** | rule systems (game mode) | pawns + `Session` → `Session` | server only |
| **View** | camera rig (follow/blend) | replicated pawn → camera `Transform` | client-local |
| **View** | camera resolve | `Viewer`'s camera → `CameraView` | client-local |

The **Authority** column and the predict annotations describe how the columns will *read* once a
net layer exists; today every row runs locally in two phases — the table is the design's
net-readiness thesis, not a claim that this planset implements replication or prediction.

The line between **Sim** (deterministic, replicated, a pure function of state + intents) and
**View** (client-local, derived, never on the wire) is the most important structure to set now;
single-threaded it is one extra partitioned pass over the system list, and it is agony to
introduce after the fact.

## Why this anticipates networking without committing to a model

The earlier `GameMode`-as-type failed three tests; the seams here pass all three — **cheap now,
model-agnostic, painful to retrofit:**

- **Intent** is true under lockstep, rollback, or snapshot replication alike — every model sends
  *something* per player and replays it.
- **The Sim/View split** is true under every model — all of them separate authoritative
  simulation from local view derivation.
- **`Authority`** is an annotation, not an institution — it commits to no replication strategy.

None of them reproduces an actor-network object graph, so an ECS-native net layer
(state/input replication, à la the ECS-netcode lineage) slots in without dismantling anything.

## Scope decisions

1. **The renderer stays untouched.** It consumes a resolved `CameraView` through `SceneView`
   ([SceneRenderer.h:285](../../engine/include/Veng/Renderer/SceneRenderer.h:285)); this planset
   changes only how that view is *sourced*. Resolution stays a step the caller owns — never a
   `SceneRenderer` lookup — so the editor's external camera and the runtime's in-scene camera
   coexist with zero renderer change.

2. **Selection is per-`Viewer`, not a global active-camera tag.** A **`Viewer { Entity Camera }`**
   component is a *seat* (a render target / local player / editor) that names the camera it
   renders through. Single-player is one viewer; split-screen is two; the editor viewport is a
   non-player viewer. This separates *seat* from *camera*, which is what multi-view and
   per-client net need — a one-field component over a bare tag, for a generalization a tag cannot
   express.

3. **Aspect is a render-target property, never a `Camera` field.** The resolve takes `aspect`
   from the caller (output extent in the runtime, panel extent in the editor).

4. **Control produces *intent*, not motion.** The control system writes an `Intent` component; a
   separate movement system integrates it. The intent layer is the net-critical seam (principle
   2) and is mandatory even though single-player makes it look like indirection — it is the
   cheap-now/essential-later investment, and it makes AI a drop-in intent producer.

5. **Possession and view are independent.** `Possesses` (a `Viewer`/player controls a pawn) and
   `Viewer.Camera` (the seat's camera) are separate references — a spectator views without
   possessing; a cutscene retargets the viewer's camera without un-possessing. An opt-in couple
   (a pawn's own camera becomes its viewer's) is offered, not the default.

6. **Game mode is rule systems over a replicated `Session`, with per-scene config — no object.**
   A `Session` component holds phase/scores/timer (server-authoritative, the future net layer
   replicates it); the "mode" is a *selectable set of rule systems* (spawn-on-start, scoring,
   win-condition) plus a config field a scene/prefab names. Begin/end-play is the systems'
   `Start`/`Stop` (already in `SceneSimulation`) and `Session` phase transitions. **No
   `GameModeRegistry`, no module-ABI bump.**

7. **`Authority` is a lean annotation, deliberately ahead of its consumer.** `Authority { Tier;
   Owner }` is threaded onto entities with sensible defaults and read by nothing in this planset
   — its consumer is the future net layer. It is included now *only* because retrofitting
   ownership across every spawn site later is the expensive path; it commits to no net model and
   can be deferred if reviewed as too speculative (it is the one purely-anticipatory piece).

8. **Single local player; no input routing.** One `Veng::Input` populates one `PlayerInput`.
   **Multiple seats** (split-screen, AI-vs-player) need input routed per `Viewer`/player — real
   work gated on [future area 4](../future/README.md#4-event--input-systems), out of scope. The
   components here are written so that layer is additive.

9. **No render features; the golden moves where the scene moves.** The deferred pipeline is
   unchanged. `smoke_golden` is re-blessed in the plan that first moves it (the resolved in-scene
   camera in Plan 01, and the rule-spawned player in Plan 04 if it renders), per the `CLAUDE.md`
   procedure, with the change stated.

10. **A `Level` is the home for level-scoped data — a thin wrapper by reference, not a new world
    format.** A `Level` (`AssetType::Level`) references a **world prefab** and adds the data that is
    *not* reusable-recipe data: the game-mode/`Session` config, the ordered active system set, and
    render settings. This resolves the prefab dual-purpose tension (a prefab is a recipe again; the
    `Level` is the once-loaded playable unit) by **reusing** prefab serialization, not embedding a
    second copy of it. Named `Level` (not `Scene`) to avoid colliding with the runtime `Scene`.
    Loading a level spawns its world, builds a `SceneSimulation` from its system set, and seeds the
    `Session` — *that* is starting the game, and it is authored data, not `main.cpp` code.

11. **Systems are a catalog; selection/order is level data, config is components.** A `SceneSystem`
    declares a stable **`SystemId`** + name via a trait (`VE_SYSTEM`), so `SystemRegistry` enumerates
    *available* systems and a `Level` names the *active* ordered subset — no registration-signature
    or host-ABI change (the id rides a trait like a component's `TypeId`). A system's **parameters
    are authored as components** (a settings entity the system reads), reusing the entire reflection
    inspector and keeping systems pure logic — no reflected-system-config infra.

12. **Editor wiring reuses existing surfaces.** The `LevelEditorPanel` authors the level (enable/
    order systems from the catalog; edit the game-mode/render config through the shared reflection
    `FieldWidget`; compose the prefab editing surface for the world). It adds no new inspector
    machinery — the catalog drives the systems panel and reflection draws the config.

13. **Gameplay documentation is a hand-written guide, separate from Doxygen.** `docs/guides/`
    carries a task-oriented tutorial for writing gameplay systems and wiring a level, referencing
    the real shipped systems so it cannot rot away from the code — distinct from the generated
    symbol reference.

## Plans

| # | Plan | Summary | Status |
|---|---|---|---|
| 00 | [Camera resolve + Viewer selection](00-camera-resolve-viewer.md) | The camera seam. A **`Viewer { Entity Camera }`** seat component (`VE_REFLECT`, registered in `BuiltinTypes`) and pure helpers **`ResolveCameraView(const Scene&, Entity viewer, f32 aspect)`** + **`ResolvePrimaryCameraView(const Scene&, f32 aspect) → optional<CameraView>`** beside `MakeCameraView` (read the viewer's `Camera` entity, `WorldMatrix`, project at the caller's aspect). Engine addition + unit test; no caller migration, no golden. | done |
| 01 | [Runtime + editor in-scene camera](01-runtime-in-scene-camera.md) | Migrate the sample: author a `Camera` + `Transform` entity and a `Viewer` seat in `scene.prefab.json`, drop the hardcoded `CameraView m_Camera`, resolve in `OnRender` (output-extent aspect, default-view fallback). Editor gains a Play-only "render through the scene's viewer camera" toggle over the existing clone. Re-bless `smoke_golden` if the resolved view moves. | done |
| 02 | [Input → Intent → Movement](02-input-intent-movement.md) | The control pipeline. **`PlayerInput`** (per-player snapshot), **`Intent`** (abstract command), **`Possesses`** (seat→pawn). A **control** system (`PlayerInput`→`Intent`, reading the always-present `Input`) and a **movement** system (`Intent`→`Transform`); AI/remote write the same `Intent`. Sample gains a player-driven pawn (windowed-only; smoke unaffected). | done |
| 03 | [Sim/View tick phases + Authority + camera rig](03-sim-view-phases-authority.md) | The net seam. `SceneSystem` declares a **`Phase { Sim, View }`**; `SceneSimulation` runs Sim then View each tick. Add the **`Authority { Tier, Owner }`** annotation (defaults, no consumer yet). Add a client-local **camera-rig** View system trailing the possessed pawn — the first View-phase system, justifying the split. | done |
| 04 | [Game mode: Session + rule systems](04-game-mode-session.md) | The mode role, object-free. A **`Session`** state component (phase/score/timer) + a selectable set of **rule systems** (spawn-on-start, win-condition) + a per-scene config field naming the rule set. The sample's player spawn becomes a Sim-phase `SpawnPlayerRule` reacting to `Session` start. No registry, no ABI bump. Re-bless the golden if the spawned player renders. | done |
| 05 | [System catalog](05-system-catalog.md) | The engine-side wiring seam. Systems gain a **`SystemId`**/name trait (`VE_SYSTEM`); `SystemRegistry` stores `{id, name, factory}`, enumerates the **catalog**, resolves an id, and fatally rejects a duplicate id. `SceneSimulation` gains construction from an **ordered `SystemId` set** (phase-honoring) beside the "all registered" convenience. Engine-only — no asset, no cooker; does **not** depend on Plan 04, so the registry rework lands de-risked before the `Level` keystone. | done |
| 06 | [The Level asset](06-level-asset.md) | The wiring keystone. A thin **`Level`** asset (`AssetType::Level`) references a world prefab + the game-mode/`Session` config + the ordered system set + render settings (tolerant reflection records); a **loader** spawns the world, builds the `SceneSimulation` from the level's system set (Plan 05), seeds the `Session`. A **`LevelImporter`** validates the level (system ids + world prefab + game-mode config). The sample app **loads a Level** instead of hand-spawning. | done |
| 07 | [Editor: Level authoring surface](07-editor-level-authoring.md) | Wiring in the editor. A **`LevelEditorPanel`** (for `AssetType::Level`): enable/order systems from the catalog, edit the game-mode/render config via the reflection inspector, compose the prefab surface for world layout; system params stay components. Play builds its sim from the level's system set. | done |
| 08 | [Guide: writing gameplay systems](08-guide-gameplay-systems.md) | Human-readable docs. `docs/guides/writing-gameplay-systems.md` (+ a level-wiring guide): `SceneSystem` lifecycle, choosing Sim/View phase, the Input→Intent→Movement pattern, config-via-components, `VE_SYSTEM` registration, and wiring into a `Level` in the editor — with a worked example referencing the real hello-triangle systems. Doc-only. | proposed |
| 09 | [Docs & roadmap re-cut](09-docs-roadmap.md) | `CLAUDE.md` ×4 (engine Scene/ECS stack + `Level`/catalog; assetpack `AssetType::Level`; cooker `LevelImporter`; editor `LevelEditorPanel`), `future/README.md` (area 7 delivered; area 4 the multi-seat/networking gate), the `plans/README.md` entry, this table. Correct the stale ABI prose to 3 (no change here). Doc-only. | proposed |

## Dependency analysis

```
   runtime primitives (00–04)                          authoring layer (05–09)

   00 (Viewer + resolve + always-present Input)
    ├─► 01 (runtime / editor camera) ──┐
    └─► 02 (Input → Intent → Movement) ─┼─► 05 (system catalog)
                                        └─► 03 (Sim/View + Authority + rig)
                                             └─► 04 (game mode) ──┐
                                          05 + 00–04 ─────────────┴─► 06 (Level) ─► 07 (editor) ─► 08 (guide) ─► 09 (docs)
```

Arrows show the build order. 00 feeds both 01 and 02; 05 (the catalog) depends only on 02/03 and
**not** on 04, so it lands de-risked in parallel; 04 additionally depends on 00–01 (it spawns the
camera / seat / pawn); and 06 — the keystone — converges 05 + all of 00–04, as the bullets below
detail. (The boxes are tiers, not strict columns: 05–09 are the authoring layer, 00–04 the runtime
primitives.)

- **Plan 00** is a pure additive engine seam (a component, two resolve helpers, a unit test);
  depends on nothing, moves no golden, migrates no caller.
- **Plan 01** depends on 00; it is the runtime migration and the first golden re-bless. The
  editor toggle also needs 00.
- **Plan 02** depends on the delivered scene-systems framework (control/movement are systems) and
  on Plan 00's always-present `Input` (the control system reads input with no null-guard); since
  Plan 00 lands first regardless, it still runs in parallel with 01.
- **Plan 03** depends on **01 and 02**: the camera-rig View system trails the pawn (02) and
  writes the camera entity (01), and the Sim/View split needs both a Sim system (movement) and a
  View system (rig) to be meaningful.
- **Plan 04** depends on **00–03**: a rule system spawns the player + camera + viewer and wires
  possession, runs in the Sim phase, and reacts to `Session` state.
- **Plan 05** (system catalog) depends only on **02/03** (systems exist; phases order the id-list
  construction) and **not** on 04 — it is engine-only (`SystemId` trait, registry catalog,
  `SceneSimulation`-from-id-list), so the registry rework lands and is verified de-risked, in
  parallel with 03/04.
- **Plan 06** (the Level asset) depends on **00–05**: the keystone the runtime primitives converge
  on — it references the camera/world (00–01), runs the systems (02–03) through the catalog (05),
  and seeds the game mode (04). The new on-disk format, the loader, the cooker importer, and the
  sample migration live here, separate from the engine-only catalog.
- **Plan 07** depends on **06** (it authors the `Level` asset) and **05** (it lists the catalog).
- **Plan 08** depends on **02/03/05/06/07** (it documents the system model + the level wiring it
  teaches).
- **Plan 09** is the doc leaf after 08.

## Process & conventions

Same cadence as every planset: implement → migrate the example in the same pass as any breaking
change → verify (clean build, `ctest` green across the unit/death/cooker bands and the `gpu` band
where a device is present, the smoke PPM correct size + exit 0, `smoke_golden` re-checked) →
update this table → one commit per plan (`Plan NN: <summary>`, `Co-Authored-By` trailer).

Common to all plans:

- **The renderer and every `ScenePass` stay untouched** — only *call sites* choose where their
  `CameraView` comes from. A diff editing `SceneRenderer.{h,cpp}` is out of scope.
- **New components are plain reflected builtins** (`Viewer`, `PlayerInput`, `Intent`,
  `Possesses`, `Authority`, `Session`, `GameModeConfig`) — registered through
  `RegisterBuiltinTypes`, authored with `VE_REFLECT`/`VE_TYPE`, persisted by `TypeId`, **GPU-free
  by contract** (the no-device cooker test stays green). New `TypeId`s (and the `SystemId`s minted
  in Plan 05) use a marked placeholder during implementation, minted with `vengc generate-id` in
  the final pass (hex in C++, decimal in JSON).
- **No module-ABI change.** Unlike the first draft, nothing here touches `VengModuleHost` or
  `VENG_MODULE_ABI_VERSION` — game modes are systems + components, the system catalog rides a trait
  like a component's `TypeId`, and the `Level` is an asset. Everything is registered through the
  existing `SystemRegistry`/`TypeRegistry` or authored as data.
- **The authoring tier reuses existing machinery.** The `Level` (Plan 06) wraps a prefab by
  reference and loads through the ordinary `AssetManager` path; the cooker validates it like
  `PrefabImporter`; the editor (Plan 07) draws its config through the existing reflection
  `FieldWidget` and composes the prefab editing surface. System *params* are components, not a new
  config mechanism.
- **Headless/smoke determinism is preserved.** `SystemContext.Input` is an always-present service
  ([SceneSystem.h:21](../../engine/include/Veng/Scene/SceneSystem.h:21)) that reports the neutral
  all-zeros state in headless (Plan 00) — never null — so input-reading systems need no guard and a
  headless tick is automatically a pure function of state. The smoke path pins a fixed pose and does
  not tick `Update`, so
  Sim-phase motion never perturbs the capture; **View-phase systems must also be safe to run (or
  be skipped) in the pinned smoke frame.**
- **The `smoke_golden` moves at most twice — Plan 01 and possibly Plan 04 — each regenerated per
  the `CLAUDE.md` procedure** with the expected change stated. Plan 06 must keep it put (the
  `Level` loads the same world the sample authored; only the load path changes); Plans 00, 02, 03,
  05, 07, 08, 09 must not move it either.
- **Comments are present-tense facts** — no "used to hold a `CameraView` member", no "was a
  GameMode object" narrative.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.

## On completion

Gameplay control is built from ECS primitives shaped for veng's grain and its networking
trajectory. A `Camera` entity is selected per **`Viewer`** seat and resolved to a `CameraView` by
a pure function the runtime feeds straight into the renderer (the editor keeps its external
camera and can preview the scene viewer during Play). Player control flows **Input → Intent →
Movement** — `PlayerInput` captured as data, a control system producing an abstract `Intent`, a
movement system consuming it — so AI and (later) remote players are drop-in intent producers and
the simulation is a pure function of state + intents. A **Sim / View tick split** separates the
deterministic, replicable simulation from client-local view derivation, and an **`Authority`**
annotation marks ownership ahead of the net layer that will read it. A **game mode** is a
**`Session` state component + a selectable set of rule systems + a per-scene config field** — no
object, no registry, no ABI bump.

On top of those primitives sits the answer to *how you wire up a game*: gameplay systems are
written in code and made discoverable through a **`SystemRegistry` catalog** (a `SystemId`/name
trait), a thin **`Level`** asset wraps a world prefab with the level-scoped wiring (game mode, the
ordered active system set, render settings) and **loads into play**, a **`LevelEditorPanel`**
authors it (enable/order systems from the catalog, edit config through the reflection inspector,
edit the world through the prefab surface), and a **`docs/guides/`** tutorial teaches writing
gameplay systems end to end. A game is assembled as **authored data**, not hardcoded in `main.cpp`.

**Multi-seat input routing** (split-screen, AI-vs-player), the **networking layer** that consumes
intent / authority / the sim-view split, a **richer system scheduler** (inter-system
dependencies/parallelism), and **richer `Level` data** (streaming, sublevels, nav) stay named
future increments behind the seams this planset establishes.
