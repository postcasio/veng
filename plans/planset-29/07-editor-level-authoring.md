# Plan 07 — editor: Level authoring surface

**Goal:** make wiring a game something you do **in the editor** — the answer to "how do I wire up
gameplay in the editor." A **`LevelEditorPanel`** (registered for `AssetType::Level`) authors the
`Level`: it lists the **system catalog** and lets you enable + order the level's systems, edits
the game-mode/`Session` config and render settings through the existing reflection inspector, and
hosts the world prefab for layout editing. System *parameters* stay ordinary components edited in
the scene surface. Cook-on-demand + hot-reload like the other asset editors.

## Why it is its own plan

Plan 06 makes the `Level` an authorable artifact; this plan is the editor surface that authors it
— the part-b half of "wire up a game." It depends on 06 (the `Level` asset) and 05 (the system
catalog it lists), and reuses the editor framework (the `AssetEditorPanel` dockspace, the reflection
`FieldWidget`, the prefab editing surface), so it is a focused editor increment, separate from the
engine-side keystone.

## What lands

- **`LevelEditorPanel`** — a top-level asset editor registered in the `EditorRegistry` for
  `AssetType::Level` (double-clicking a `.level` opens it), built on `AssetEditorPanel`'s
  per-instance dockspace like the prefab and material editors. It hosts:

  - **A systems panel** — lists the `SystemRegistry` **catalog** (Plan 05: `[{ SystemId, Name }]`)
    with an enable toggle per system and drag-reorder for the active set, writing the level's
    ordered `systems` list. The Sim/View phase of each system (Plan 03) is shown so the author
    sees the two-phase grouping. This is the system-wiring surface.

  - **A level-settings panel** — the **game-mode/`Session` config** and the **render settings**
    drawn through the shared reflection inspector (`DrawFieldWidget` over the reflected config
    structs), so authoring them needs no bespoke widgets — the same machinery the entity inspector
    uses.

  - **The world** — the level's `world` prefab opened for layout editing via the existing
    `PrefabEditorPanel` / scene-editing surface (the Level editor links to or embeds it). System
    **parameters** are authored here as ordinary **components** on settings entities, edited by the
    existing inspector — the selection/order lives on the level, the params live in the world.

- **Cook-on-demand + hot-reload.** Editing the level recooks the `.level.json` off the render
  thread (`RequestCook` → `MountMemory` → reload behind the stable `AssetHandle`), the texture/
  material editor pattern, round-tripping the JSON (patch known keys, preserve unknown).

- **Play wires the level's system set.** The Play-in-viewport machinery (`Play()` / `m_Simulation`
  in `PrefabEditorPanel`, commit 47e4962) builds its `SceneSimulation` from the **level's** ordered
  system set (Plan 06, via Plan 05's id-list construction) when Play is launched from the Level
  editor, so Play runs exactly what the level authored — and the Plan 01 "view through the scene
  viewer" toggle shows it through the game
  camera. **Two Play run-sets coexist deliberately:** prefab-editor Play has no level, so it keeps
  the "all registered" set (a debugging convenience for editing a prefab in isolation); Level-editor
  Play runs the level's named set (the authoritative one). A system that runs in one but not the
  other is expected, not a bug — the panel surfaces which set is active so the author is not
  surprised.

## Decisions

1. **Reuse the reflection inspector for config; add no new widgets.** The game-mode/`Session` and
   render configs are reflected structs drawn by `DrawFieldWidget`, exactly like component fields
   — consistent with the editor's "reflection-driven inspector" principle.

2. **System selection/order on the level; params as components.** The panel authors *which*
   systems and *in what order* (the level's list); a system's *tuning* is a component edited in the
   world surface. Two clean surfaces, no overlap, both reusing existing machinery.

3. **Compose with the prefab editor, don't reimplement scene editing.** The world is a prefab;
   the Level editor reuses the prefab editing surface for layout rather than duplicating the
   scene-graph/inspector panels (the `AssetEditorPanel` class-restricted dockspace makes this
   composition clean).

4. **The catalog drives the systems panel.** The available systems come from the registry (Plan
   05), so a game's own registered systems appear automatically with no editor change — the editor
   reflects the catalog the way the inspector reflects the type registry.

## Files

| File | Change |
|---|---|
| `editor/src/panels/LevelEditorPanel.{h,cpp}` *(new)* | The Level editor: systems-catalog panel (enable/order), level-settings panel (reflected game-mode/render config), world-prefab editing composition, cook-on-demand + hot-reload. |
| `editor/src/…` (EditorRegistry wiring) | Register `LevelEditorPanel` for `AssetType::Level`. |
| `editor/src/FieldWidget.*` (reuse) | No new widget expected; reuse `DrawFieldWidget` for the config structs. |
| `editor/src/panels/PrefabEditorPanel.*` (Play) | Build the Play `SceneSimulation` from the level's ordered system set when launched from the Level editor (the `Play()` / `m_Simulation` path); prefab-editor Play keeps the "all registered" set. |

## Verification

- Clean build; `ctest` green across the bands (editor framework changes are exercised by the
  existing editor tests; the new panel is UI).
- Manual editor check: open the sample `.level`, toggle/reorder systems (the active set changes
  what Play runs), edit the game-mode/render config through the inspector, edit the world layout
  via the prefab surface, recook live, and confirm Play runs the authored system set and (with the
  toggle) views through the scene camera.
- `include_hygiene` unaffected (editor-only change); `smoke_golden` does **not** move (editor path,
  not the runtime capture).

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
