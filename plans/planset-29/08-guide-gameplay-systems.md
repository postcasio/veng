# Plan 08 — guide: writing gameplay systems

**Goal:** ship **human-readable documentation** for writing a game in veng — a checked-in,
tutorial-style guide that takes an author from "empty `SceneSystem`" to "running in a level,"
covering the patterns this planset establishes: the Sim/View phases, Input → Intent → Movement,
config-via-components, registration into the catalog, and wiring into a `Level` in the editor.
Distinct from the generated Doxygen API reference — this is the prose a person reads to learn how
to build gameplay, not a symbol index.

## Why it is its own plan

The runtime primitives (00–04) and the authoring surface (05–07) are only *usable* by someone
who knows the intended patterns; a worked guide is the deliverable that makes the planset's design
teachable rather than archaeology. It depends on the model being settled (02/03 for the
phases/intent pattern, 05/06/07 for the catalog + level + editor wiring), so it lands near the end,
and it is doc-only so it carries no code risk.

## What lands

- **`docs/guides/writing-gameplay-systems.md`** — the primary guide. A task-oriented walkthrough,
  not a reference dump:

  1. **What a system is** — `SceneSystem`, the `OnStart`/`OnUpdate`/`OnStop` lifecycle, and the
     `SystemContext` (`Assets`, `Input` — and that `Input` is always present, reading all-zeros in
     headless, so a system reads it without any null-guard and stays deterministic there).
  2. **Choosing a phase** — `Phase::Sim` (deterministic, replicable gameplay) vs `Phase::View`
     (client-local view derivation: camera rigs, anything reading finalized state to present it),
     and *why* the line matters (it is the seam the future net layer and a paused-sim editor rely
     on). **The Sim determinism contract:** a `Phase::Sim` system must read only entity state and
     `Intent` — never `context.Input` directly, wall-clock time, or async asset-load state — because
     those make it non-replayable and silently break lockstep. Determinism is a discipline the phase
     *enables*, not a guarantee the engine enforces; this is the rule that upholds it.
  3. **The Input → Intent → Movement pattern** — read `PlayerInput` → write `Intent` in a control
     system; consume `Intent` → mutate state in a movement/gameplay system; and the payoff: an AI
     system is just another `Intent` producer, and the same shape is what a net layer predicts and
     rolls back. Show the anti-pattern (reading the device or moving the pawn directly in one
     system) and why it is avoided.
  4. **Configuring a system** — the config-via-components convention: a system reads a settings
     component (defaulting if absent), authored and tuned in the inspector, rather than carrying
     hardcoded constants.
  5. **Registering it** — `VE_SYSTEM(Type, 0x…ULL, "Name")` + `Register<T>()` in
     `VengModuleRegister`, minting the `SystemId` with `vengc generate-id`, so the system appears in
     the editor's system catalog.
  6. **Wiring it into a level** — open the `Level` in the editor, enable + order the system, set
     its config component in the world, hit Play. The `Level` is where a game is assembled.
  7. **A worked example, end to end** — a small but complete gameplay system (e.g. a simple
     pickup/score or a patrol/AI mover) written from scratch through to running in the sample
     level, cross-referencing the **real hello-triangle systems** as the live, compiling reference.

- **`docs/guides/wiring-a-level.md`** (or a section of the above) — the `Level` concept from the
  author's side: world prefab vs. level-scoped data (game mode, systems, render settings), why a
  level is not a prefab, and the load-to-play flow.

- **A docs index entry** — a `docs/README.md` (or guides index) listing the guides, and a pointer
  to it from the engine `CLAUDE.md` (Plan 09 folds the cross-reference into the doc re-cut).

## Decisions

1. **Hand-written, task-oriented markdown — separate from Doxygen.** The generated API reference
   (`build/docs/html`) documents *symbols*; this guide documents *how to build gameplay*. They are
   different artifacts with different audiences; the guide lives in source under `docs/guides/`.

2. **The example references real shipped code.** The worked system cross-references the actual
   hello-triangle systems so the guide cannot silently rot away from the code — a reader can open
   the live system next to the prose. (Plan 09's read-through checks they still agree.)

3. **It teaches the *why*, not just the steps.** The phase choice and the intent pattern are
   explained with their reasons (determinism, net-readiness, AI uniformity), so an author
   internalizes the model rather than copying boilerplate — matching the project's comment
   philosophy applied to docs.

## Files

| File | Change |
|---|---|
| `docs/guides/writing-gameplay-systems.md` *(new)* | The primary tutorial guide (sections 1–7 above). |
| `docs/guides/wiring-a-level.md` *(new, or a section)* | The `Level` concept from the author's side; the load-to-play flow. |
| `docs/README.md` *(new)* | A guides index. |

## Verification

- The guide's described API matches the shipped code — every named type/macro
  (`SceneSystem`, `Phase`, `PlayerInput`, `Intent`, `VE_SYSTEM`, `Level`) exists as written, and
  the worked example's pattern compiles against the real engine (spot-checked against the
  hello-triangle systems it references).
- Links resolve (the guides index, the `CLAUDE.md` pointer added in Plan 09, the cross-references
  to source).
- No code change; `ctest` stays green.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
