# Plan 06 — docs + roadmap pass

**Goal:** document the installable SDK, the tri-mode consumption story, the co-development
override, and the helper renames across the `CLAUDE.md` set and a new `docs/guides/` entry; mark
future [area 6](../future/README.md#6-editor-application)'s install-wiring item **delivered**
(its SDK-freeze remainder staying future); run the full verification band. The closer. **Depends
on Plans 00–05.**

## Why it is its own plan

This planset retires a lot of "in-tree only" language scattered across the docs
([`game-module.md`](../future/game-module.md)'s "CMake surface" note,
[`editor.md`](../future/editor.md), the per-module `CLAUDE.md` guides, the root `CLAUDE.md`) and
introduces a genuinely new capability — out-of-tree consumption with three discovery modes —
plus two renamed public helpers. Folding the documentation into the implementation plans would
scatter it and risk describing surface that shifted between plans; a single closer writes it once
against the landed code.

## What lands

- **Root [`CLAUDE.md`](../../CLAUDE.md).** Document the **three consumption modes** (in-tree
  `add_subdirectory`; build-tree `find_package` → `veng/build`, no install; install-prefix
  `find_package` → the shipped SDK) and that `find_package(veng)` brings `veng::veng`, `vengc`,
  `veng-editor`, and the `veng_add_project` / `veng_add_game` / `veng_add_editor` /
  `veng_add_asset_pack` vocabulary. Update the layout/build narrative: the helpers are an
  installed surface, the two examples are co-migrated **in opposite modes** (`template`
  out-of-tree, `hello-triangle` in-tree). Record the **renames** (`add_project` →
  `veng_add_project`, `add_asset_pack` → `veng_add_asset_pack`) wherever the old names appear.

- **[`engine/CLAUDE.md`](../../engine/CLAUDE.md).** The launcher source as an installed SDK
  artifact; the imported-`vengc` cook path for a downstream consumer; the mode-resolved core-data
  variables.

- **[`cooker/CLAUDE.md`](../../cooker/CLAUDE.md).** `vengc` ships as an imported executable in
  `vengTargets`; the installed core-data source (`share/veng/core/`) a downstream cook resolves
  `--reference` / `--shader-include` against. The cooker + graph now build **unconditionally** from
  source (`VENG_BUILD_TOOLS` retired — veng is the tools); the installed `vengc` carries an
  `INSTALL_RPATH` and **requires the host's Vulkan SDK / Slang present to run** (Slang is not
  vendored).

- **[`editor/CLAUDE.md`](../../editor/CLAUDE.md).** `libveng_editor` + `veng-editor` are an
  installed surface behind `VENG_INSTALL_SDK`; a downstream `EDITOR_MODULE` links
  `veng_editor::veng_editor`; `veng_add_editor` resolves the imported `veng-editor`.

- **A `docs/guides/` SDK-consumption entry (new).** How to consume veng out-of-tree: the minimal
  standalone `CMakeLists` (`find_package(veng)` + `veng_add_project`/`veng_add_game`), the three
  discovery overrides (`veng_ROOT` / `CMAKE_PREFIX_PATH` / `FETCHCONTENT_SOURCE_DIR_VENG`), and the
  **co-development recipe** (engine + game iterated together with no reinstall). Finalizes the
  recipe drafted in Plan 03.

- **The SDK runtime prerequisites, documented.** State plainly in the guide (and the `cooker` /
  `editor` `CLAUDE.md`) that the installed `vengc` / `veng-editor` require the host's Vulkan SDK
  (with its Slang component) present at runtime — Slang is not vendored — and that a consumer should
  build against a veng built with the **same toolchain** (the `-fno-exceptions` / one-STL convention
  the package does **not** ABI-enforce; `find_package(veng)` checks only `SameMajorVersion`, which at
  `0.x` is effectively no gate). This is the "convention, not freeze" stance made concrete so a
  mismatch is a documented prerequisite rather than a silent link/ODR failure.

- **[`future/README.md`](../future/README.md) + [`game-module.md`](../future/game-module.md) +
  [`editor.md`](../future/editor.md).** Mark area 6's "installing `veng_add_game` for downstream
  `find_package(veng)` consumers" / "consumed through `find_package(veng)`" **delivered by
  planset-40**: the installed library + tools + helpers, the imported-tool export, the tri-mode
  `veng-config`, and the renamed surface. Keep only the **still-future remainder** — the **frozen
  module ABI + shipped-SDK freeze** (pinned toolchain contract, additive-only semver'd boundary
  interfaces for *separately built* third-party modules) and the **project-picker launcher** (now
  built on the installed `veng-editor` + the `.veng/build.json` sidecar). Strike the
  game-module.md "in-tree only / installed-package wiring remains forward work" passage as
  delivered.

- **[`plans/README.md`](../README.md).** Add the planset-40 index entry (the installable SDK +
  out-of-tree consumption + the renames) and update the area-6 future recap.

## Decisions

1. **Documentation lands once, against the landed code.** A single closer writes the guides after
   the install/export/rename surface is real, rather than each plan re-describing a shifting
   system.
2. **Area 6's install-wiring item is delivered; the ABI-freeze is not.** The *packaging*
   (`find_package(veng)`, installed cooker + editor, tri-mode discovery) ships; the *interface
   freeze* for hosting separately-built third-party modules stays a named follow-on behind a
   stabilized engine surface.
3. **The two-examples-in-two-modes rule is a documented norm.** The co-migration obligation now
   also pins the *consumption mode* (template out-of-tree, hello-triangle in-tree) in the root
   `CLAUDE.md` "Working norms." The template is **no longer built by the default in-tree build** —
   the SDK conformance test is its standing check — so the norm states plainly that a template
   breakage surfaces in the conformance test (the `gpu` band), not in a plain `cmake --build`.

## Files

| File | Change |
|---|---|
| `CLAUDE.md` | The three consumption modes; the installed authoring vocabulary; the renames; the two-examples-in-two-modes norm. |
| `engine/CLAUDE.md` | Launcher source as an SDK artifact; the imported-`vengc` downstream cook; mode-resolved core-data vars. |
| `cooker/CLAUDE.md` | `vengc` as an imported exe; the installed core-data source for downstream cooks. |
| `editor/CLAUDE.md` | `libveng_editor` + `veng-editor` installed behind `VENG_INSTALL_SDK`; `EDITOR_MODULE` linkage. |
| `docs/guides/<sdk-consumption>.md` (new) | Out-of-tree consumption + the three discovery overrides + the co-dev recipe. |
| `plans/future/README.md` | Mark area 6's install-wiring delivered; keep the ABI-freeze + project-picker remainder. |
| `plans/future/game-module.md` | Strike the "in-tree only / installed-package wiring forward work" passage as delivered. |
| `plans/future/editor.md` | Update the `find_package(veng)` consumption note as delivered. |
| `plans/README.md` | The planset-40 index entry; the area-6 future recap. |
| `plans/planset-40/README.md` | Flip every plan's status to `done`. |

## Verification

- The **full band** green from a clean `build/`: `cmake -B build` + build; `ctest
  --output-on-failure` (including `sdk_conformance_install`, `sdk_conformance_buildtree`,
  `template_launcher_smoke`, `hello_triangle_launcher_smoke`); `smoke_golden` unmoved (no render
  change this planset); `ctest -L validation` on a `build-debug` (`validation_gate` green);
  `include_hygiene` green.
- A fresh `cmake --install build --prefix <tmp>` + a standalone out-of-tree template build against
  it succeeds end-to-end (the manual mirror of the conformance test).
- No grep of the old helper names (`add_project` / `add_asset_pack` as bare calls) remains in the
  tree outside historical plan prose.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
</content>
