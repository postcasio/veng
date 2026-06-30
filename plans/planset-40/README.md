# planset-40 — the installable veng SDK & out-of-tree consumption

**Phase goal:** let a game live **outside the engine tree** and consume veng as a normal
CMake-discovered library — `find_package(veng)` brings `veng::veng`, a working `vengc`, a
shared `veng-editor`, and the full authoring vocabulary (`veng_add_project` / `veng_add_game`
/ `veng_add_editor` / `veng_add_asset_pack`) with **no veng source tree present**. The
**in-tree path stays a first-class option** (`hello-triangle` and the tests keep building veng
as the top-level project and consuming its targets directly); `examples/template` becomes the
**out-of-tree exemplar** consuming an installed *or* a build-tree veng. This takes up future
[area 6](../future/README.md#6-editor-application)'s named-but-undrafted item — "installing
`veng_add_game` for downstream `find_package(veng)` consumers" (and the editor's "consumed
through `find_package(veng)`") — and the **CMake-surface forward work** in
[game-module.md](../future/game-module.md).

## Why now, and what already exists

The **library** install is already most of the way there and this planset builds on it rather
than redoing it: [`CMakeLists.txt`](../../CMakeLists.txt) already
`install(TARGETS veng EXPORT vengTargets)`, installs `engine/include/Veng` + the reachable
imgui/imnodes headers, and generates `veng-config.cmake` + `veng-targets.cmake`;
[`assetpack/CMakeLists.txt`](../../assetpack/CMakeLists.txt) joins the export set; and every
FetchContent dependency is configured `*_INSTALL ON` (glm/fmt/glfw/VMA/nfd/zstd) precisely so
`find_package(veng)` is self-contained. So `target_link_libraries(app veng::veng)` from an
installed veng is essentially already wired.

The gap is everything **above the library** — the authoring machinery a game actually needs,
all of it currently in-tree-only:

1. The **helper modules** ([`cmake/AssetPack.cmake`](../../cmake/AssetPack.cmake),
   [`Project.cmake`](../../cmake/Project.cmake), [`Game.cmake`](../../cmake/Game.cmake),
   [`Editor.cmake`](../../cmake/Editor.cmake), [`BuildConfig.cmake`](../../cmake/BuildConfig.cmake),
   [`PerConfigInvalidation.cmake`](../../cmake/PerConfigInvalidation.cmake)) are `include()`d
   only from the top-level tree and are neither installed nor referenced from
   [`veng-config.cmake.in`](../../cmake/veng-config.cmake.in).
2. **`vengc`** is uninstalled, yet `veng_add_project` invokes `$<TARGET_FILE:vengc>`.
3. **`VENG_LAUNCHER_MAIN`** ([`Game.cmake`](../../cmake/Game.cmake)) and **`VENG_CORE_SHADER_DIR`**
   / the embedded **core pack** all resolve to source-tree paths.
4. **`libveng_editor` + `veng-editor`** (and `veng::graph`) are `PROJECT_IS_TOP_LEVEL`-gated and
   uninstalled, so `veng_add_editor` cannot resolve a shared editor downstream.

## The unifying design — imported tools + tri-mode `veng-config`

Two moves make the helper modules work **identically** in every consumption mode without
forking their bodies:

- **Export `vengc` and `veng-editor` as `IMPORTED` executables** in `vengTargets`, and have
  `veng-config` recreate the unqualified target names with a `NOT TARGET`-guarded
  `add_executable(vengc ALIAS veng::vengc)` (legal for imported exes; verified on the CMake ≥ 4.1
  this project requires). Then `$<TARGET_FILE:vengc>` / `$<TARGET_FILE:veng-editor>` resolve to a
  *built* target in-tree and an *aliased imported* one downstream — `veng_add_project` /
  `veng_add_asset_pack` / `veng_add_editor` keep their **exact current bodies**, with no variable
  indirection.
- **Path variables become mode-resolved.** `VENG_LAUNCHER_MAIN`, `VENG_CORE_SHADER_DIR`, and the
  core-pack manifest are set to source-tree paths when the helpers load **in-tree**, and to
  **installed** paths (or **build-tree** paths) when the helpers load from a generated
  `veng-config`.

This yields **three consumption modes through one `veng-config`**:

```
in-tree         add_subdirectory(veng)            hello-triangle, the tests        — unchanged
build tree      find_package(veng) → veng/build   co-develop engine + game, NO install
install prefix  find_package(veng) → <prefix>     the shipped SDK
```

The **build-tree** mode is the co-development answer: alongside `install(EXPORT)`, the engine
also `export(EXPORT vengTargets …)`s into its build dir and writes a `veng-config.cmake` there,
so a game configured with `-Dveng_ROOT=/path/to/veng/build` discovers veng with **no install
step** — `cmake --build` on the engine refreshes the exported targets in place and the game's
next build picks them up. A game repo that *declares* veng as a pinned `FetchContent`
dependency can additionally redirect to a live checkout with
`FETCHCONTENT_SOURCE_DIR_VENG=/path/to/veng`.

## The renames

Two helpers lack the `veng_` namespace prefix the rest of the exported surface carries
(`veng_add_game`, `veng_add_editor`). Since both become part of the **installed public
vocabulary** this planset, they are renamed uniformly:

- `add_project` → **`veng_add_project`**
- `add_asset_pack` → **`veng_add_asset_pack`**

All call sites (the examples, the tests, the docs) move in the same pass. No alias is kept —
the old names never shipped as a public, installable surface, so there is nothing downstream to
break.

## Plans

| # | Plan | Summary | Status |
|---|---|---|---|
| 00 | Cooker + core data as SDK artifacts | Install `vengc` (with `INSTALL_RPATH`); export it as an **imported executable** in `vengTargets`; install the engine **core pack source** (`engine/assets/core/`) + the core shader dir; **retire `VENG_BUILD_TOOLS`** (tools always built from source); resolve the core-data vars mode-aware (internal `VENG_CORE_*`, consumer-facing lowercase `veng_CORE_*`). No helper-module or editor change yet. | proposed |
| 01 | Helper modules + launcher + the renames | Install the `cmake/*.cmake` helpers + `launcher_main.cpp`; `include()` the helpers from `veng-config` and `find_dependency` what they need; resolve `VENG_LAUNCHER_MAIN` mode-aware. **`add_project` → `veng_add_project`**, **`add_asset_pack` → `veng_add_asset_pack`**, all call sites updated. Depends on 00. | proposed |
| 02 | Install the editor surface | Install `veng_graph` + `veng_editor` (+ public headers, with `INSTALL_RPATH`) and export `veng-editor` as an **imported executable**; `veng-config` recreates the `veng::graph` / `veng_editor::veng_editor` / `veng-editor` aliases; flip the `PROJECT_IS_TOP_LEVEL`-only gate behind a `VENG_INSTALL_SDK` option (which now gates the editor only). Depends on 01. | proposed |
| 03 | Build-tree export + co-dev override | `export(EXPORT vengTargets …)` + a generated build-tree `veng-config.cmake`; the `veng_ROOT` / `CMAKE_PREFIX_PATH` / `FETCHCONTENT_SOURCE_DIR_VENG` discovery overrides; a documented "develop engine and game together, no reinstall" recipe. Depends on 02. | proposed |
| 04 | Make `examples/template` a `find_package` consumer | `examples/template` becomes a **standalone** project consuming veng via `find_package` and is **removed from the engine build** (`add_subdirectory(template)` dropped); only the conformance test builds it. `hello-triangle` stays in-tree. The two halves of the dual-mode conformance matrix. Depends on 03. | proposed |
| 05 | SDK conformance test | A staged install **and** a build-tree export, each consumed by a throwaway configure+build of the template, then the launcher smoke — the build-time analogue of the relocatable-trio test, wired into ctest. Depends on 04. | proposed |
| 06 | Docs + roadmap pass | Document the SDK surface, the tri-mode consumption story, and the renames across the `CLAUDE.md` set; mark area 6's install-wiring item delivered in `future/README.md` + `game-module.md` + `editor.md`; add a `docs/guides/` SDK-consumption entry; run the full verification band. The closer. Depends on 00–05. | proposed |

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = implemented, migrated, verified, committed.

## Dependencies

- **00** is foundational (the install of the cooker + core data + the imported-`vengc` export);
  everything downstream cooks through it.
- **01** depends on 00 (the helpers it installs invoke the imported `vengc` and reference the
  installed core data). **02** depends on 01 (the editor helper joins the same installed-helper
  set). **00 → 01 → 02** is the install chain — they share
  [`CMakeLists.txt`](../../CMakeLists.txt)'s install block and
  [`veng-config.cmake.in`](../../cmake/veng-config.cmake.in), so merge in number order.
- **03** depends on 02 (it adds the build-tree export of the *complete* target set 00–02
  install).
- **04** depends on 03 (the template consumes whichever discovery 03 enables; the conformance
  matrix wants both). **05** depends on 04 (it drives the migrated template).
- **06** is the closer, depending on 00–05.

Dependent plans must build on the **prior plan's integration commit**, not `origin/main`. Per
[[project_megaexec_worktree_base]], `isolation: "worktree"` branches from `origin/main` and will
not see a locally-committed-but-unpushed base: dispatch each plan against a manual worktree cut
from the prior plan's integration commit. This planset is a **linear chain** (00 → 01 → 02 → 03
→ 04 → 05 → 06) — there are no independent plans to parallelize, since each layer installs/exports
on top of the last.

## The decisions this planset settles

- **The library install is reused, not rebuilt.** `install(TARGETS veng EXPORT vengTargets)` and
  the dependency `*_INSTALL ON` wiring already exist; this planset completes the surface *above*
  the library (tools, helpers, core data, editor) on the same export set.
- **Tools are exported as imported executables, with aliases recreated downstream.** `vengc` and
  `veng-editor` join `vengTargets` as `IMPORTED` exes, and `veng-config` recreates their unqualified
  names (and the `veng::graph` / `veng_editor::veng_editor` library aliases) with `NOT TARGET`-guarded
  `ALIAS`es, so `$<TARGET_FILE:…>` and the in-tree alias spellings keep resolving downstream — the
  helper-module bodies are mode-agnostic.

- **Veng is the tools — `VENG_BUILD_TOOLS` is retired.** A from-source build always builds the
  cooker + graph (and, behind `VENG_INSTALL_SDK`, the editor); a library-only consumer uses
  `find_package(veng)` and gets the prebuilt imported tools. The installed tools require the host's
  Vulkan SDK / Slang at runtime (not vendored) and carry an `INSTALL_RPATH` so they run from the
  prefix.
- **Path variables are mode-resolved through one `veng-config`.** `VENG_LAUNCHER_MAIN`,
  `VENG_CORE_SHADER_DIR`, and the core-pack manifest resolve to source paths in-tree and
  installed/build-tree paths when found as a package — one config, three modes.
- **Co-development needs no reinstall.** A build-tree `export(EXPORT)` + a build-tree
  `veng-config` lets `find_package(veng)` resolve against `veng/build`; the engine's
  `cmake --build` refreshes targets in place. `FETCHCONTENT_SOURCE_DIR_VENG` is the ergonomic
  redirect for a game repo that vendors the dependency declaration.
- **The exported authoring surface is uniformly `veng_`-prefixed.** `add_project` and
  `add_asset_pack` are renamed to `veng_add_project` / `veng_add_asset_pack`; no alias is kept.
- **Both examples co-migrate, in opposite modes.** `examples/template` is the out-of-tree
  `find_package` exemplar — **removed from the engine build** and exercised only by the SDK
  conformance test; `examples/hello-triangle` stays the in-tree exemplar, covered by the default
  build's smokes. Together they are the dual-mode conformance check.
- **Packaging, not ABI freeze.** This planset ships the *build-time* SDK; the
  `-fno-exceptions` / one-STL / one-toolchain contract stays **convention** (consume veng built
  from the same tree or the same toolchain). A semver'd, frozen boundary ABI for hosting
  *separately built* third-party modules in a *shipped* editor remains future
  ([area 6](../future/README.md#6-editor-application)'s SDK-freeze note).

## What remains future

- **The frozen module ABI + shipped-SDK freeze** — a pinned, enforced toolchain contract and
  additive-only, semver'd boundary interfaces so a *separately built* third-party module loads
  into a *shipped* editor. This planset delivers the packaging (`find_package(veng)`, an
  installed `veng-editor` + cooker); the interface-freezing discipline is deferred until the
  engine surface stops churning each planset.
- **A project-picker launcher** — the standalone front-end that lists projects and spawns
  `veng-editor --project <path>`. It builds on the installed `veng-editor` this planset ships
  and the `.veng/build.json` sidecar already in place; it is its own planset.
- **Windows packaging specifics** — the install lays down `INSTALL_RPATH` and the
  `$<TARGET_RUNTIME_DLLS>` copy the in-tree `veng_add_game` already does; a fully Windows-shipped
  SDK (the dllimport/dllexport visibility flip, an installer) rides the anticipated Windows port.
</content>
</invoke>
