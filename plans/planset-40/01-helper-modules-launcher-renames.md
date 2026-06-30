# Plan 01 — helper modules, launcher source, and the renames

**Goal:** make the authoring vocabulary available to a `find_package(veng)` consumer — install
the `cmake/*.cmake` helper modules + the launcher source, `include()` the helpers from the
generated `veng-config`, resolve `VENG_LAUNCHER_MAIN` mode-aware, and rename the two
unprefixed helpers (`add_project` → **`veng_add_project`**, `add_asset_pack` →
**`veng_add_asset_pack`**) with every call site moved in the same pass. **Depends on Plan 00.**

## Why it is its own plan

Plan 00 installed the *tool* and *data*; this plan installs the *functions* that drive them and
makes them load through `veng-config`. The rename is bundled here because it edits the very files
this plan installs and `include()`s — doing it in the same pass means the installed surface is
correct on first ship, with no churn. It is a pure CMake-surface + mechanical-rename change, no
engine-runtime or editor work.

## What lands

- **Install the helper modules.** `install(FILES cmake/AssetPack.cmake cmake/Project.cmake
  cmake/Game.cmake cmake/BuildConfig.cmake cmake/PerConfigInvalidation.cmake DESTINATION
  ${CMAKE_INSTALL_LIBDIR}/cmake/veng)` (the editor helper joins in Plan 02). They sit beside the
  generated `veng-config.cmake` / `veng-targets.cmake` already installed there.

- **`veng-config.cmake.in` includes the helpers.** After the `find_dependency` block and the
  `include(veng-targets.cmake)`, the config recreates the `vengc` alias
  (`if (NOT TARGET vengc) add_executable(vengc ALIAS veng::vengc) endif()`, Plan 00), `include()`s
  each helper module, and `find_dependency`s anything they need beyond the library's own deps. The
  helpers' tool references (`vengc`) resolve through that alias — the helper bodies are unchanged.

- **Install the launcher source; resolve `VENG_LAUNCHER_MAIN` mode-aware.**
  [`Game.cmake`](../../cmake/Game.cmake) hardcodes `VENG_LAUNCHER_MAIN` to
  `${CMAKE_CURRENT_LIST_DIR}/../engine/src/Launcher/launcher_main.cpp` — a source-tree path. Install
  `launcher_main.cpp` to `${CMAKE_INSTALL_DATADIR}/veng/launcher/`, and make `Game.cmake` resolve
  `VENG_LAUNCHER_MAIN` from `VENG_PACKAGE_MODE` — a three-valued mode flag `veng-config` sets:
  `IN_TREE` (unset / the top-level build → source path), `INSTALL` (the installed-prefix path), and
  `BUILD_TREE` (the live source path again, wired in Plan 03). This plan handles `IN_TREE` +
  `INSTALL`. The `veng_add_game` body — which compiles `${VENG_LAUNCHER_MAIN}` into
  `<name>-launcher` — is otherwise unchanged.

- **`add_project` → `veng_add_project`.** Rename the function in
  [`Project.cmake`](../../cmake/Project.cmake) and its header-comment surface. Call sites:
  `examples/hello-triangle/CMakeLists.txt`, `examples/template/CMakeLists.txt`, and any test wiring.

- **`add_asset_pack` → `veng_add_asset_pack`.** Rename the function in
  [`AssetPack.cmake`](../../cmake/AssetPack.cmake). The rename is **atomic across the tree in this
  one commit** — every call site moves regardless of which later plan touches that file. Call sites:
  [`CMakeLists.txt`](../../CMakeLists.txt)'s `veng_test_shaders` pack,
  [`editor/CMakeLists.txt`](../../editor/CMakeLists.txt)'s `veng_editor_icons` pack (renamed here
  even though the editor *install* is Plan 02 — the function no longer exists under the old name the
  moment this lands), and `examples/hello-triangle`'s `hello_triangle_assets` pack. The returned
  `${TARGET_NAME}_TARGET` parent-scope variable name is unchanged.

- **No alias kept.** Neither name ever shipped as an installed public surface, so there is nothing
  downstream to break; a deprecation shim would only invite the old name into new code.

## Decisions

1. **The helpers load through `veng-config`.** A consumer gets `veng_add_project` /
   `veng_add_game` / `veng_add_asset_pack` for free on `find_package(veng)`, with no manual
   `include()` of an SDK path.
2. **`VENG_LAUNCHER_MAIN` is mode-resolved, the body is not.** The launcher source compiles into
   each game's launcher; resolving the *path* per mode keeps the single `veng_add_game` body valid
   in-tree and downstream.
3. **The renames land with the install, no alias.** Uniform `veng_`-prefixing of the exported
   surface, done once, before any downstream consumer exists to depend on the old names.

## Files

| File | Change |
|---|---|
| `CMakeLists.txt` | `install(FILES cmake/*.cmake …)` for the helpers; install `launcher_main.cpp`. |
| `cmake/veng-config.cmake.in` | Recreate the `vengc` alias (`add_executable(vengc ALIAS veng::vengc)` behind a `NOT TARGET` guard); `include()` the helper modules; `find_dependency` their needs; set `VENG_PACKAGE_MODE` (`INSTALL` here; `BUILD_TREE` added in Plan 03) + the installed `VENG_LAUNCHER_MAIN` and both the internal `VENG_CORE_*` and consumer-facing `veng_CORE_*` paths. |
| `cmake/Game.cmake` | Mode-aware `VENG_LAUNCHER_MAIN` keyed on `VENG_PACKAGE_MODE` (source path in-tree, installed path as a package). |
| `cmake/Project.cmake` | `add_project` → `veng_add_project` (function + header comment). |
| `cmake/AssetPack.cmake` | `add_asset_pack` → `veng_add_asset_pack` (function + header comment). |
| `CMakeLists.txt` (test wiring) | `add_asset_pack(veng_test_shaders …)` → `veng_add_asset_pack(...)`. |
| `editor/CMakeLists.txt` | `add_asset_pack(veng_editor_icons …)` → `veng_add_asset_pack(...)` (rename only; the editor install is Plan 02). |
| `examples/hello-triangle/CMakeLists.txt` | `add_project` / `add_asset_pack` call sites renamed. |
| `examples/template/CMakeLists.txt` | `add_project` call site renamed (still in-tree at this point; full out-of-tree migration is Plan 04). |

## Verification

- Clean in-tree build + full `ctest` green — the renamed helpers drive the same cooks; the
  examples and `veng_test_shaders` build and run unchanged (`smoke_golden`,
  `*_launcher_smoke`).
- `cmake --install build --prefix /tmp/veng-sdk` lays down the helper `.cmake` files,
  `launcher_main.cpp`, and a `veng-config.cmake` that `include()`s them; a smoke `find_package`
  probe (a throwaway `CMakeLists` that only `find_package(veng)` + calls
  `if(COMMAND veng_add_game)`) confirms the functions are defined (full build is Plan 05).
- `include_hygiene` / `validation_gate` unaffected.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
</content>
