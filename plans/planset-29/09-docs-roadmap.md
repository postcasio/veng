# Plan 09 — docs & roadmap re-cut

**Goal:** bring the live project docs to the delivered end state — the `Viewer`/resolve camera
seam, the Input→Intent→Movement pipeline, the Sim/View tick split + `Authority`, the Session/
rule-system game mode, and the **`Level` asset + system catalog + editor wiring** — across the
engine/assetpack/cooker/editor `CLAUDE.md`s, the `future/README.md` roadmap, the `plans/README.md`
index, and this planset's status table. Doc-only; the leaf after Plans 00–08.

## Why it is its own plan

The per-plan commits update the docs they touch, but the **cross-cutting** picture — how the
camera / control / game-mode / level-authoring stack sits in the architecture, why it is built
from components + systems + a thin asset rather than objects, and what it leaves for the
networking layer (area 4) — belongs in one coherent pass once the end state is settled. It also
folds in the `docs/guides/` cross-reference from Plan 08.

## What lands

- **Engine `CLAUDE.md` — Scene & ECS section.** Document the full stack:
  - the **`Viewer`/resolve** camera seam (selected per seat, resolved by
    `ResolveCameraView`/`ResolvePrimaryCameraView`, aspect-from-target), distinct from the
    editor's external `EditorCamera`;
  - **Input → Intent → Movement** (`PlayerInput` → `Intent` → movement; AI/remote as drop-in
    intent producers; possession independent of view);
  - the **Sim / View tick split** on `SceneSystem`/`SceneSimulation` and the **`Authority`**
    annotation;
  - the **game mode** as a `Session` state component + rule systems;
  - the **system catalog** (`SystemId`/`VE_SYSTEM`, the registry as a catalog) and the **`Level`**
    asset (wrapper-by-reference over a world prefab + level-scoped game-mode/system/render data,
    loaded into play) — the authored wiring artifact, with system *params* as components.
  - a pointer to the new **`docs/guides/`**.

- **`CLAUDE.md` accuracy fix.** The module ABI prose is stale (states 2 while the code is at 3);
  correct it to **3** and state this planset adds **no** ABI surface — the contrast that records
  why the ECS-native design (asset + catalog + systems) is lighter than the discarded
  actor-derived `GameMode`-object draft.

- **`assetpack/CLAUDE.md` + `cooker/CLAUDE.md`.** Add `AssetType::Level` + `CookedLevelHeader` to
  the format doc, and the `LevelImporter` (validating a level's system ids + world prefab + reflected
  game-mode config against the `dlopen`ed module) to the cook doc, beside `PrefabImporter`.

- **`editor/CLAUDE.md`.** Document `LevelEditorPanel` — the system-catalog wiring panel, the
  reflected level-settings panel, and the composed world-prefab editing — beside the prefab/material
  editors.

- **`future/README.md`.** Update **area 7 (scene/entity model)**: the gameplay + authoring layer is
  now delivered (systems framework, Viewer/resolve, Input→Intent→Movement, Sim/View phases, Session/
  rule game modes, the `Level` asset + catalog + editor wiring), leaving the **richer system
  scheduler** (inter-system dependencies/parallelism), archetype storage, and dirty-flag propagation
  as the remaining area-7 items. Re-frame **area 4 (event & input)** as the **next gate**:
  single-player runs on one `Veng::Input` → one `PlayerInput`; **multi-seat input routing** and the
  **networking layer** consuming `Intent`/`Authority`/the Sim-View split are the work this planset
  motivates and shapes. Note **camera blend/shake** (View-phase) and **richer `Level` data**
  (streaming, sublevels, spawn/nav) as named refinements.

- **`plans/README.md`.** Update the planset-29 index entry to the delivered (done-state) summary.

- **This planset's status table + "On completion".** Flip rows to `done` as they land; confirm the
  completion paragraph matches what shipped.

## Decisions

1. **One doc pass at the end** — the architectural narrative (the whole stack + what it leaves
   area 4/the net layer) is only coherent once Plans 05–08 land.

2. **Correct the stale ABI prose and state "no change here"** — recording why this design is
   lighter than the actor-derived draft.

3. **Doc policy holds** — present-tense facts; describe the delivered seams, never the discarded
   `GameMode`-object draft or the old hardcoded camera/hand-spawn.

## Files

| File | Change |
|---|---|
| `engine/CLAUDE.md` | The full Scene/ECS stack (camera/control/phases/Authority/game-mode/catalog/`Level`); ABI prose → 3, "no change"; `docs/guides/` pointer. |
| `assetpack/CLAUDE.md` | `AssetType::Level` + `CookedLevelHeader`. |
| `cooker/CLAUDE.md` | `LevelImporter` beside `PrefabImporter`. |
| `editor/CLAUDE.md` | `LevelEditorPanel`. |
| `plans/future/README.md` | Area 7 delivered status + scheduler/blend/level refinements; area 4 as the multi-seat-input + networking gate. |
| `plans/README.md` | The planset-29 entry → delivered summary. |
| `plans/planset-29/README.md` | Status table → `done`; completion paragraph confirmed. |

## Verification

- Docs/links resolve (the `CLAUDE.md` cross-references, the `docs/guides/` pointer, the `plans/`
  relative links).
- No code change beyond doc comments; `ctest` stays green.
- A read-through confirms the engine/assetpack/cooker/editor `CLAUDE.md`s, `future/README.md`, and
  `plans/README.md` agree on the delivered end state, the (unchanged) ABI version, and the scoping
  of multi-seat input + networking to future area 4.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
