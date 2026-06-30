# Plan 03 — build-tree export + the co-development override

**Goal:** let a game discover veng **without an install step** while both are under active
development — `export(EXPORT vengTargets …)` into the engine build tree plus a generated
build-tree `veng-config.cmake`, so `find_package(veng)` resolves against `veng/build`; document
the `veng_ROOT` / `CMAKE_PREFIX_PATH` / `FETCHCONTENT_SOURCE_DIR_VENG` overrides and the
"develop engine and game together" recipe. **Depends on Plan 02.**

## Why it is its own plan

Plans 00–02 produce a correct *installed* package. This plan adds the **build-tree** resolution
of the same target set — the workflow the request named ("override discovery so I don't have to
keep reinstalling the SDK while developing both"). It is the point at which the package config
must be proven *mode-resolved* (Plan 01's `VENG_PACKAGE_MODE`, the imported tools, the path
variables) against a build tree as well as a prefix. Self-contained: it adds export wiring and
docs, changes no helper body.

## What lands

- **`export(EXPORT vengTargets …)`.** Alongside the existing `install(EXPORT vengTargets)`, the
  top-level build `export()`s the same target set into the build dir as
  `${CMAKE_BINARY_DIR}/veng-targets.cmake`, with the imported `veng::vengc` / `veng::veng-editor` /
  `veng::veng_editor` / `veng::veng_graph` pointing at their **build-tree** locations.

- **A *separate* generated build-tree `veng-config.cmake`.** The install config from
  `configure_package_config_file` **cannot** be reused as-is for the build tree: its `@PACKAGE_INIT@`
  computes `PACKAGE_PREFIX_DIR` by walking up the fixed nesting depth implied by `INSTALL_DESTINATION`
  (`lib/cmake/veng`, three levels), but the build-tree config sits at the build-dir root (one level),
  so the same expansion resolves `PACKAGE_PREFIX_DIR` to an unrelated ancestor (the existing build-dir
  `veng-config.cmake` is only an install *staging* artifact today, so this is latent until now).
  Generate the build-tree config as its **own** file via a plain `configure_file` (not
  `configure_package_config_file`), substituting build-tree absolute paths directly and skipping the
  prefix-relative math. In build-tree mode (`VENG_PACKAGE_MODE=BUILD_TREE`) the path variables resolve
  to **source-tree** paths (`VENG_LAUNCHER_MAIN`, `VENG_CORE_SHADER_DIR`, `VENG_CORE_PACK_JSON` → the
  live engine tree; the lowercase `veng_CORE_*` aliases likewise), the alias recreation and helper
  `include()`s point at the live `cmake/*.cmake` (not copies), and the two configs share a common body
  fragment but differ in their prefix/path preamble. A consumer pointing `veng_ROOT` at the build dir
  finds this config.

- **The three discovery overrides, documented and verified to work:**
  - `find_package(veng)` with **`-Dveng_ROOT=/path/to/veng/build`** (or on `CMAKE_PREFIX_PATH`) —
    resolve against the build tree, no install. The engine's `cmake --build` refreshes the
    exported targets in place; the game's next build picks them up.
  - the same with an **install prefix** — the shipped-SDK path (Plans 00–02).
  - **`FETCHCONTENT_SOURCE_DIR_VENG=/path/to/veng`** — for a game repo that declares veng as a
    pinned `FetchContent` dependency: redirect the pinned tag to a live checkout, transparently an
    `add_subdirectory` of the live engine source.

- **The co-dev recipe.** A short documented flow: configure the engine once
  (`cmake -B build -S veng`), configure the game with `-Dveng_ROOT=$PWD/veng/build`, then iterate
  — edit engine → `cmake --build build` → edit game → `cmake --build game-build`, no reinstall in
  the loop. (Docs land fully in Plan 06; the recipe is drafted here against the working export.)

## Decisions

1. **The build tree is a first-class package source.** `export(EXPORT)` + a build-tree-valid
   `veng-config` make `find_package(veng)` resolve against `veng/build`, so co-development never
   pays an install round-trip.
2. **Build-tree mode resolves paths to the live source tree.** Unlike install mode (copied data
   under a prefix), build-tree mode points `VENG_LAUNCHER_MAIN` / `VENG_CORE_*` and the helper
   `include()`s at the live engine checkout, so an engine source edit is visible to the game with
   only a rebuild.
3. **`FetchContent` redirect is supported, not required.** A game may consume veng via
   `find_package` (engine built separately) **or** via `FetchContent` with a local-checkout
   redirect; both are documented, neither is privileged. The template (Plan 04) demonstrates
   `find_package`.

## Files

| File | Change |
|---|---|
| `CMakeLists.txt` | `export(EXPORT vengTargets NAMESPACE veng:: FILE ${CMAKE_BINARY_DIR}/veng-targets.cmake)`; generate a **separate** build-tree `veng-config.cmake` via `configure_file` (not `configure_package_config_file`) with build-tree absolute paths. |
| `cmake/veng-config-buildtree.cmake.in` (new) | The build-tree preamble: `VENG_PACKAGE_MODE=BUILD_TREE`, source-tree path resolution, live `cmake/*.cmake` `include()`s, the alias recreation — sharing the common body fragment with the install `veng-config.cmake.in`. |
| `docs/guides/` (drafted, finalized in 06) | The co-dev recipe + the three discovery overrides. |

## Verification

- Clean in-tree build + `ctest` green — adding `export(EXPORT)` does not change the in-tree or
  install behavior; the build still produces `build/veng-targets.cmake` + `build/veng-config.cmake`.
- A throwaway consumer `CMakeLists` configured with `-Dveng_ROOT=$PWD/build` resolves
  `find_package(veng)`, sees `veng_add_game` / `veng_add_project` / `veng_add_editor` defined, and
  `$<TARGET_FILE:vengc>` points into the build tree (full build is Plan 05).
- A second probe with `FETCHCONTENT_SOURCE_DIR_VENG` pointed at the source tree resolves veng as a
  subdirectory build.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
</content>
