# Plan 04 — editor Play seeds the session

**Goal:** pressing Play in the editor should drop into the same initialized, playable state the
runtime reaches through `Level::LoadInto` — a seeded `Session`, the game-mode config applied, the
player spawned. Today Play runs the simulation over a clone with no session, so the spawn rule bails
and the scene is inert. Reads atop **Plan 02** (it reuses the residency wait for the play scene).

## The bug

[`Level::LoadInto`](../../engine/src/Asset/Level.cpp) is what *starts the game* at runtime: after
spawning the world prefab it creates a `Session{Phase = Playing}` + `GameModeConfig` entity, and the
game's `SpawnPlayerRule::OnStart` reads that `Playing` phase to instantiate the player
([`main.cpp`](../../examples/hello-triangle/main.cpp)).

The editor's [`PrefabEditorPanel::Play`](../../editor/src/panels/PrefabEditorPanel.cpp) only clones
the edit scene and `Start`s the simulation — it never seeds the session. So `SpawnPlayerRule`'s
`FindSession` returns null, `OnStart` bails, no player spawns, and the "playing" scene does nothing.
The [`LevelEditorPanel`](../../editor/src/panels/LevelEditorPanel.cpp) already edits the
`GameModeConfig` (`m_GameMode`) and the ordered system set, but applies neither on Play.

## What lands

- **A shared session-seed helper.** Factor the session creation out of `Level::LoadInto` into a
  reusable engine function — `SeedSession(Scene&, const GameModeConfig&)` (or a small
  `Level`-adjacent helper) — that creates the well-known session entity with `Session{Phase =
  Playing}` + the config. `LoadInto` calls it; the editor calls it too, so the runtime and editor
  seed identically from one code path.

- **A `SeedPlayScene` hook on the base.** `PrefabEditorPanel::Play` gains a virtual
  `SeedPlayScene(Scene&)` called after the clone and before `Start`. The base is a no-op (a bare
  prefab has no session). `LevelEditorPanel` overrides it to `SeedSession(scene, m_GameMode)` — so the
  level's edited game-mode config drives the play session, and the game's `SpawnPlayerRule` (already
  in the level's ordered system set) fires exactly as at runtime.

- **Player-prefab residency before Start.** `GameModeConfig::PlayerPrefab` is an `AssetHandle<Prefab>`
  edited through the inspector picker; on Play it must be resident or `SpawnPlayerRule::OnStart` bails
  the same way the runtime relies on the level loader eager-resolving it. `LevelEditorPanel::Play`
  ensures the handle is resident (a `LoadSync`, or a `WaitResident` on the player's spawn batch from
  Plan 02) before `Start`.

- **No change to player ownership.** Per the planset decision, the player stays in `GameModeConfig`
  spawned by the game's `SpawnPlayerRule`; this plan only makes the editor seed the session that rule
  reads. The rule is unchanged and unmoved.

## Decisions

1. **One seed path, runtime and editor.** The session is seeded by a shared helper, not duplicated in
   the editor — so "what Play does" cannot drift from "what `LoadInto` does."

2. **A `SeedPlayScene` hook, not session logic in the base.** The base `PrefabEditorPanel` knows
   nothing about sessions (a bare prefab has none); only `LevelEditorPanel` seeds. The hook keeps the
   level-specific play setup in the level panel, beside `GetPlaySystems`.

3. **Editor Play does not block on streaming.** Unlike the smoke capture, the editor play scene
   streams content in over frames (it is interactive, not a deterministic capture). The only residency
   it forces is the **player prefab** itself, so the spawn rule has something to spawn; recipe meshes
   inside it pop in a few frames late, as in the running game.

## Verification

Clean build, `ctest` green. In the editor, opening the sample level and pressing Play spawns the
player, captures the cursor, and is controllable (move/look) — the same state the launcher drops into
— and Stop returns cleanly to the authored edit scene.
