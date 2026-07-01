# Plan 04 — docs, template co-migration, roadmap pass

**Goal:** the closer. Document the action-mapping layer for someone writing a game against it,
co-migrate the minimal `examples/template` (the out-of-tree conformance sample), update the module
guides, record the roadmap state (naming multi-seat routing + networking as the next planset built on
this seat seam), and run the full verification band. Depends on Plans 00–03.

## The starting point

- Both samples are co-migrated on every breaking change: `examples/hello-triangle` (maximal, in-tree)
  and `examples/template` (minimal, out-of-tree via `find_package(veng)`, exercised only by
  `sdk_conformance_*`). A breaking change that misses the template surfaces in the conformance tests,
  not a plain build.
- The template's `main.cpp` is a **bare `Application`** with a managed viewport + world; its startup
  level spins a cube via the engine `ConstantMotionSystem` and takes **no player input** — it
  registers no control system today.
- Gameplay authoring has a `docs/guides/` tutorial set (writing gameplay systems, wiring a level).
- The per-module architecture lives in `engine/CLAUDE.md` (runtime), `editor/CLAUDE.md` (editor),
  `cooker/CLAUDE.md` (cook); the roadmap in `plans/` and `plans/future/README.md`.

## What lands

### 1. `examples/template` co-migration

The template takes **no** player input — its cube is driven by `ConstantMotion`, not an `Intent`.
So the co-migration is a **verification, not a feature add**: confirm the reshaped `PlayerInput` and
the new builtin `InputMappingSystem` do not force the minimal app to carry input it does not need
(the system iterates seats; a world with no `Viewer`/`InputContextStack` seat resolves nothing, a
clean no-op). The template stays input-free — the point is proving the minimal app is *unaffected*.
If the reshape leaks any required wiring into it, that is a design leak to fix here, not to paper over
in the template. (No `.inputmap` asset is added to the template; the maximal sample carries the
end-to-end input path.)

### 2. Docs

- **A `docs/guides/` entry — "authoring input actions"**: declare action-id constants, write a
  `*.inputmap.json` (actions + bindings), reference it from a seat's `InputContextStack` in the world
  prefab, and read actions by name in a control system (`input.WasTriggered(Actions::Jump)`). The key
  teaching points: gameplay reads `Intent` (not actions), only the control system reads actions,
  `InputMappingSystem` must run before the control system, and context-stack push/pop switches
  schemes.
- **`engine/CLAUDE.md`** — extend the *Gameplay* section: `PlayerInput` is now the resolved
  `ActionState`; `InputContextStack` + the builtin `InputMappingSystem` (sole raw-input reader, Sim,
  first, locally-owned seats); the `ResolveActions` pure core in `Veng/Input/`; the `AssetType::InputMap`
  asset in the *Assets* section beside the other CPU-only assets. State the layering invariant
  (actions → `PlayerInput` → control system → `Intent` → gameplay; AI/net produce `Intent` directly).
- **`editor/CLAUDE.md`** — the `InputMappingEditorPanel` beside the texture/material editors: reflected
  binding-table inspector, live recook, resolved-state preview, basic-by-design.
- **`cooker/CLAUDE.md`** — the `InputMapImporter` + its binding→action validation beside the material
  and prefab importers.
- **Root `CLAUDE.md`** — the `AssetType::InputMap` mention if the asset-type list is enumerated; no
  layout change (no new library).

### 3. Roadmap pass

- **`plans/future/README.md` area 4** — mark the **input half delivered** (event routing was already
  delivered by planset-30; this planset delivers the action-mapping layer and `PlayerInput` =
  `ActionState`). Re-cut the *remaining* to: **multi-seat input routing** (pointer-by-region +
  device-by-id into per-seat `InputContextStack` → `PlayerInput`) and **the networking layer**
  (replicate `PlayerInput`, re-derive `Intent` server-side), naming them the **next planset** built
  directly on this planset's seat seam — `InputMappingSystem` already resolves per seat, so multi-seat
  is additive routing. Note the *runtime remapping UI* and *richer triggers/modifiers* + a *global
  `ActionRegistry`/bespoke editor* as the smaller named follow-ons.
- **`plans/README.md`** — add the planset-42 entry (the recap paragraph, house style: what it delivers,
  what it supersedes — the `PlayerInput` reshape supersedes planset-29's fixed-struct `PlayerInput` —
  and what it holds back).
- **The planset-42 README status column** — flip to `done` per plan as they land.

### 4. Verification band (the whole planset)

- **`ctest -R action_resolve`** (Plan 00 pure tests), the `cooker` `.inputmap` validation tests (Plan
  02), and the `PlayerInput`/`InputContextStack` prefab round-trip (Plan 01) all green.
- **`hello_triangle-launcher` under `HT_SMOKE`** writes the correct-sized PPM; **`smoke_golden`
  unmoved** (input is neutral in smoke, so the render is byte-identical — a green golden is expected,
  not evidence the feature works; the resolve/cook/round-trip tests are that evidence).
- **`sdk_conformance_install` / `sdk_conformance_buildtree`** green (the template co-migration).
- **The validation gate** in `build-debug` (`ctest -L validation`) — the reshaped component + new
  system + new asset type must not introduce a non-exhaustive switch under `-Werror`.
- Clean build, full `ctest` green, pre-commit format/lint clean.

## Files (sketch)

- `docs/guides/authoring-input-actions.md` (+ `docs/README.md` index entry).
- `engine/CLAUDE.md`, `editor/CLAUDE.md`, `cooker/CLAUDE.md`, root `CLAUDE.md`.
- `plans/future/README.md`, `plans/README.md`, `plans/planset-42/README.md`.
- `examples/template/…` — verified unaffected (no feature edit expected).

## Verification

The band above is the planset's exit gate. The closer commits the docs + roadmap in one pass after
Plans 00–03 are green and migrated.

## Dependencies

Plans 00–03.
