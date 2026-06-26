# Plan 05 — the minimal game template

**Goal:** add `examples/template/` — the **absolute minimum** code to open a window and render a
rotating cube. It is the canonical **starting point a new developer copies**, and — like
`hello-triangle` — it is **migrated in the same pass as every breaking change** going forward. Where
`hello-triangle` is the engine's *maximal* sample, the template is its *minimal* conformance surface.
**Independent** of the build-configuration plans (it relies on the zero-config cook default).

## Why it is its own plan

`hello-triangle` has become a kitchen-sink demo — ~700 lines of `main.cpp` driving every battery, a
frame-time graph, dynamic-resolution panels, a control pipeline, a game mode, a skinned character.
That is the right *maximal* exercise, but it is the wrong *first thing a new developer reads*. The
template is the opposite end: the smallest app that proves the engine is wired and renders something.
It is its own plan because it is a new build target with its own asset pack and its own smoke test,
independent of the build-config thread, and because establishing the **two-examples, co-migrated**
discipline is a deliberate project decision, not a side effect.

## What lands

- **`examples/template/` — a `veng_add_game` member.** A `libtemplate` + `template-launcher` trio
  (the same relocatable shape as hello-triangle), built under the existing `VENG_BUILD_EXAMPLES`
  toggle, so it needs no new test or CMake infrastructure. A short `README.md` states its purpose
  (*"copy this directory to start a new veng game"*) and points at `hello-triangle` + the Project
  Settings panel for the richer surface (batteries, debug UI, build configurations).

- **A minimal `main.cpp`.** An `Application` subclass that:
  - mounts the template's pack (`ExecutableDirectory()`-relative, the relocatable idiom);
  - builds its world **in code** — three entities: a `Camera`, a directional `Light`, and the cube
    (`Transform` + `MeshRenderer` whose mesh is a **cube primitive recipe**, the planset-34 inline
    `cooked AssetId | recipe` source — so the cube needs no cooked mesh asset);
  - **rotates the cube inline in `OnUpdate`** by mutating its `Transform.Rotation` — *no custom
    component, no `SceneSystem`, no `SceneSimulation`, no prefab, no `--module` reflection*. The
    rotation is the minimal "something is alive on screen," not a gameplay lesson;
  - opts into the engine-owned **managed primary viewport** (`ApplicationInfo::ManagedViewport`) and
    pushes the resolved camera each frame via `GetPrimaryViewport()->SetViewState(...)` — the
    plug-and-play render path, and nothing else: no `SceneRenderer` wiring, no composite, no ImGui.

  The file is the deliberate floor: a newcomer reads it top to bottom and understands every line.

- **A tiny asset pack.** One **trivial surface material** (`*.vmat.json` — a flat/simple PBR material,
  referencing the engine core `surface.vert` by id like hello-triangle's `brick`), its one fragment
  shader, and nothing else. The cube is a recipe (no mesh asset), the world is built in code (no
  prefab/level asset), so the pack is one material + one shader. The pack's `add_asset_pack` must
  `REFERENCE` the engine core pack (as hello-triangle's does) so the material's core `surface.vert` id
  resolves at cook time. New `AssetId`s are minted with `vengc generate-id` after the build is verified
  (clearly-marked placeholders while implementing, per the working norms).

- **A build-only launcher smoke — no pixel golden.** A `template_launcher_smoke` ctest runs the
  launcher under a `TEMPLATE_SMOKE`-style headless env and asserts **exit 0 + a correct-sized
  capture** (the relocatable-trio check, mirroring `hello_triangle_launcher_smoke`), plus a **cheap
  non-blank check** — at least some pixels differ from the clear colour, so a black-screen regression
  (a material/shader that failed to resolve, e.g. the [[project_material_param_vec_layout]] trap) fails
  the test rather than passing on size alone. It is deliberately **not** golden-image-checked: a second
  golden doubles the regen burden on every deliberate render change for little extra coverage
  (hello-triangle's golden already pins the exact pixels). Labelled `gpu`, `SKIP_RETURN_CODE 77`,
  skipping with no ICD.

- **The co-migration rule, recorded.** A line in the root `CLAUDE.md` "Working norms" (and the
  examples norms) states that a breaking change migrates **both** `examples/hello-triangle` *and*
  `examples/template` in the same pass — the template is the minimal conformance check that the
  smallest correct app still compiles and runs after an API change. (Plan 06 writes the prose; this
  plan establishes the obligation.)

## Decisions

1. **The template is genuinely minimal — world built in code, cube rotated inline.** No custom
   component, system, simulation, prefab, level, or module reflection. The smallest path to "a window
   with a rotating cube" is an `Application` that authors three entities and mutates one `Transform`
   per frame. The ECS/systems/prefab/level story is what `hello-triangle` and the `docs/guides/`
   tutorial teach; the template teaches *the engine opens and renders*.
2. **The cube is a primitive recipe, not a cooked mesh.** planset-34 made a primitive a mesh whose
   source is an inline recipe in the one `MeshRenderer.Mesh` slot, so the template needs no `.obj` and
   no mesh asset — the leanest possible geometry.
3. **The template relies on the zero-config cook default.** It ships **no** `project.veng` — its pack
   cooks via the planset-33/Plan-01 zero-config ASTC default. The template demonstrates the *smallest
   app*, not the build-config surface; `hello-triangle` is the multi-configuration demonstration, and
   the template `README` points there. This keeps the template minimal and decouples Plan 05 from
   Plans 00–04.
4. **No pixel golden for the template, but a non-blank guard.** A build-only launcher smoke (exit 0 +
   correct-sized capture + at least some non-clear pixels) is the guard; the image is not
   golden-checked, so a deliberate render change regenerates only hello-triangle's golden. The
   template's job is to prove *it builds, runs, and draws something*, cheaply.
5. **Two co-migrated examples, by rule.** The engine ships a maximal example and a minimal one; both
   move in lockstep with breaking changes. The template is the standing check that the newcomer's
   starting point never rots.

## Files

| File | Change |
|---|---|
| `examples/template/main.cpp` (new) | The minimal `Application`: mount pack, author camera/light/cube in code, rotate the cube in `OnUpdate`, push the managed-viewport `ViewState`. |
| `examples/template/CMakeLists.txt` (new) | `add_asset_pack` (zero-config, `REFERENCE` the core pack) + `veng_add_game` for the trio. |
| `examples/template/assets/` (new) | One `*.vmat.json` + its fragment shader + the pack manifest; the cube is a recipe (no mesh asset). |
| `examples/template/README.md` (new) | Purpose (copy-to-start), a pointer to `hello-triangle` + the Project Settings panel. |
| `examples/CMakeLists.txt` (or the examples aggregation) | Add the `template` subdirectory under `VENG_BUILD_EXAMPLES`. |
| `tests/…` | `template_launcher_smoke` — run the launcher headless, assert exit 0 + correct-sized capture + non-blank pixels (`gpu`, `SKIP_RETURN_CODE 77`). No golden. |

## Verification

- Clean build; `ctest` green — `template_launcher_smoke` runs the relocatable trio headless and exits
  0 with a correct-sized, non-blank capture; it skips cleanly with no ICD.
- The template builds under `VENG_BUILD_EXAMPLES` with no new toggle; `hello_triangle_*` tests
  unaffected.
- `smoke_golden` does **not** move — the template carries no golden and does not touch
  hello-triangle's.
- `include_hygiene` / `validation_gate` unaffected — the template is an app over the public API only.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
