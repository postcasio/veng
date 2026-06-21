# Plan 04 — game mode: Session + rule systems

**Goal:** express "game mode" the ECS-native way — a **`Session`** state component plus a
selectable set of **rule systems** plus a per-scene config field — with **no object, no registry,
and no module-ABI bump**. The sample's player/camera spawn moves out of `OnInitialize` into a
Sim-phase **`SpawnPlayerRule`** system that reacts to `Session` entering play, the veng analogue
of a game mode spawning the player at match start — built from the seams Plans 00–03 already
established.

## Why it is its own plan

It is the keystone that *uses* the whole spine: a rule system spawns the player prefab (a pawn +
camera + `Viewer` seat from Plans 00–01), wires possession (`Possesses`, Plan 02), runs in the Sim
phase (Plan 03), and drives `Session` state. Isolating it keeps the session/rules model and the
sample's spawn migration in one reviewable commit, and demonstrates that "game mode" needs no new
engine infrastructure — the deliberate contrast with the discarded registered-`GameMode` draft.

## Why no object, no registry, no ABI bump

A game mode's jobs decompose cleanly (scope decision 6):

- **State** → a `Session` component (server-authoritative, the future net layer replicates it).
- **Rules** → ordinary `SceneSystem`s reacting to `Session` state and mutating it.
- **Selection** → a config field a scene/prefab names.
- **Lifecycle** → `SceneSimulation::Start`/`Stop` (already present) plus `Session` phase
  transitions the rule systems react to.

Every piece rides existing seams. The discarded draft's `GameModeRegistry` + `VengModuleHost`
change + ABI 3→4 bought nothing these do not, and froze an actor-network authority shape ahead of
any net decision. This plan touches **no** module surface.

## What lands

- **`Session` — the mode's state.** A reflected component on a well-known session entity holding
  `Phase { NotStarted, Playing, Ended }`, plus mode-relevant fields a first cut needs (an elapsed
  `Timer`, a placeholder score/objective field). It is the replicated authority state in the net
  future; today it is plain scene data the rule systems read and write. It carries
  `Authority::Server` (Plan 03).

- **Rule systems — the mode's behavior.** Sim-phase `SceneSystem`s that operate on `Session` +
  player/pawn entities. The first cut ships at least:
  - **`SpawnPlayerRule`** — spawns at the rule's `OnStart` (the authored `Session` begins in
    `Playing`, so the spawn is deterministic with no `Update` tick required — this is what keeps the
    smoke capture reproducible). It spawns the configured player prefab (`Prefab::SpawnInto`), then
    ensures the spawned seat possesses the spawned pawn and names the spawned camera (`Possesses`,
    `Viewer.Camera`) — or, more simply, spawns a player prefab that **already authors** those
    references, so the rule only picks + spawns. On `Session` ending / `OnStop`, it despawns the
    player.
  - room is left for a `WinConditionRule` / scoring rule as the obvious second system; the first
    cut may stub it.

- **Selection as config.** Which rule set + parameters a scene runs is **data**: a `GameModeConfig`
  field (a reflected struct: the player-prefab `AssetHandle<Prefab>`, mode params) on the session
  entity, authored in the prefab. Different modes = different config + different registered rule
  systems; no C++ singleton picks the mode. *Vocabulary:* the reflected component is
  **`GameModeConfig`**; its authored JSON key (in Plan 06's `.level.json`) is **`gameMode`**; prose
  calls it the **game-mode config**. These name the same data at three layers, not three things.

- **The sample migrates its spawn into a rule.** The hand-authored player/camera setup
  (Plan 01's authored bootstrap seat, and any `OnInitialize` spawn) moves into the
  `SpawnPlayerRule` driven by an authored `Session`. The static world (primitives, lights) stays
  authored in `scene.prefab.json`; the **player** is what the rule spawns — the level holds the
  world, the mode spawns the player.

## The golden and smoke determinism

**The smoke path does not run the simulation today** — the sample constructs the `SceneSimulation`
and calls `Start` only in the windowed branch (`if (!m_SmokeOutput)`,
[main.cpp:171](../../examples/hello-triangle/main.cpp:171)); smoke writes a fixed pose directly and
never starts the systems. For a spawn-at-start rule to produce the player, **this plan changes the
smoke path to construct + `Start` the simulation** (so `SpawnPlayerRule::OnStart` runs), while
keeping smoke's existing behavior of **not** ticking `Update`. This is the explicit migration, not a
pre-existing fact, and it is also the path Plan 06 commits to (load → spawn → build sim → render).
Three consequences:

1. **Spawn is deterministic.** The rule spawns at `OnStart` from a fixed prefab with authored
   transforms; smoke still does not tick `Update`, so nothing moves after spawn, and the
   always-present `Input` reads all-zeros (Plan 00) regardless. The capture is reproducible.
2. **The camera pose is the spawned/authored one, rig-independent.** Because `Update` is not ticked,
   the View-phase camera rig (Plan 03) does not run in smoke, so the camera sits at the transform
   the spawn/authoring gave it. The spawned camera's authored transform must therefore equal the
   intended capture pose (the Plan 03 smoke invariant) — verify this when the seat moves onto the
   spawned player.
3. **The golden moves iff the spawned player renders a visible mesh.** Spawning the *camera + seat*
   only (no visible mesh) leaves the capture as Plan 01 left it. A visible pawn moves it — re-bless
   `smoke_golden` here per the `CLAUDE.md` procedure and state the change. Prefer a non-rendering
   spawn in the smoke-relevant path; re-bless only if a visible pawn is the chosen demonstration.

## Decisions

1. **State + systems + config, never an object.** The ECS-native game mode (scope decision 6); no
   registry, no ABI bump.

2. **Lifecycle on the existing simulation seam.** Rule systems start/stop with the session via
   `SceneSimulation::Start`/`Stop` and react to `Session` phase; no new begin/end-play
   infrastructure.

3. **The player prefab carries its own wiring where possible.** Authoring `Viewer`/`Possesses`/
   `Camera` in the player prefab keeps the spawn rule to "pick + spawn," with imperative
   post-spawn wiring only where a reference must point at a sibling the prefab cannot name.

4. **Per-scene mode selection is data.** A scene names its `GameModeConfig`; one engine, many modes,
   no code path picks the mode. Selecting *which registered rule systems* run per mode (vs. all
   registered rules gating themselves on config) is an implementation choice — the first cut may run
   one rule set gated on `Session`/config.

## Files

| File | Change |
|---|---|
| `engine/include/Veng/Scene/Components.h` (or new headers) | `Session` (+ `Phase`) and `GameModeConfig` components + reflect blocks. |
| `engine/include/Veng/Scene/BuiltinTypes.h` / `.cpp` | Register `Session`, `GameModeConfig`. |
| `examples/hello-triangle/main.cpp` | Construct + `Start` the `SceneSimulation` in **both** paths (lift it out of the `!m_SmokeOutput` gate) so `SpawnPlayerRule::OnStart` runs headless; smoke still does not tick `Update`. |
| `examples/hello-triangle/…` | A `SpawnPlayerRule` (Sim-phase) + an authored `Session`/config; a `player.prefab.json` carrying the pawn + camera + `Viewer`/`Possesses`. Remove the `OnInitialize` hand-spawn of the player/camera. |
| `tests/…` | A session-lifecycle test (spawn on start, despawn on stop) + a rule-ordering check. |
| `tests/golden/hello_triangle_scene.png` | Re-blessed **only if** the spawned player renders. |

## Tests

- **Spawn/despawn (unit):** after `Start` with an authored `Session`/config, the scene contains the
  spawned player (pawn possessed, a `Viewer` naming the spawned camera); after `Stop`, the player is
  gone. A scene with no `Session`/rule runs unchanged.
- **Phase placement:** the spawn rule runs in the **Sim** phase (Plan 03) and its spawned camera is
  followed by the View-phase rig in the same/next tick.

## Verification

- Clean build; `ctest` green across the bands, including the session-lifecycle test.
- The no-device **cooker** path builds the new prefabs and components (all GPU-free); the
  prefab-cook module load is unaffected — **no ABI change**.
- `hello_triangle_launcher_smoke` exits 0 and writes a correct-sized PPM through the full
  `dlopen` → `Start` → `SpawnPlayerRule` → render chain.
- `smoke_golden`: unchanged if the start-spawn is non-rendering; otherwise re-blessed here with the
  change stated.
- The relocatable trio still resolves and runs.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
