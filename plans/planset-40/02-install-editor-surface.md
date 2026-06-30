# Plan 02 — install the editor surface

**Goal:** make `veng_add_editor` work for a `find_package(veng)` consumer — install `veng_graph`
+ `veng_editor` (with its public headers) and export the **`veng-editor`** exe as an imported
executable, behind a `VENG_INSTALL_SDK` option that builds + installs the editor in an SDK build
(today it is `PROJECT_IS_TOP_LEVEL`-only). **Depends on Plan 01.**

## Why it is its own plan

The editor is the heaviest new install surface: a shared framework library (`veng_editor`, aliased
`veng_editor::veng_editor`) with its own public header tree (`VengEditor/`), a public dependency on
`veng_graph` (aliased `veng::graph`), and a standalone exe (`veng-editor`) that statically links the
cooker. It is currently gated to top-level builds and uninstalled
([`CMakeLists.txt`](../../CMakeLists.txt) `if (PROJECT_IS_TOP_LEVEL) add_subdirectory(editor)`).
Lifting that gate, installing the right pieces, and exporting `veng-editor` as an imported tool is a
coherent slice separable from the library/cooker install below it (Plan 00 already made the cooker +
graph build unconditionally; this plan is purely the editor surface on top).

## Target-name note

The plan names targets by their **real** CMake target name, not their `lib…` prose name:

| Prose name | CMake target | In-tree alias | Exported as |
|---|---|---|---|
| `libveng_graph` | `veng_graph` | `veng::graph` | `veng::veng_graph` |
| `libveng_editor` | `veng_editor` | `veng_editor::veng_editor` | `veng::veng_editor` |
| `veng-editor` (exe) | `veng-editor` | — | `veng::veng-editor` |

`install(EXPORT vengTargets NAMESPACE veng::)` produces `veng::<target>`, so downstream the targets
arrive as `veng::veng_graph` / `veng::veng_editor` / `veng::veng-editor`. The in-tree spellings
(`veng::graph`, `veng_editor::veng_editor`, the bare `veng-editor`) come from `ALIAS`es defined in
the subdirectory `CMakeLists.txt`s, which a downstream `find_package` consumer does **not** load — so
`veng-config` must **recreate those aliases** from the exported names (see below), exactly as it
recreates the `vengc` alias in Plan 00.

## What lands

- **A `VENG_INSTALL_SDK` option.** Default `${PROJECT_IS_TOP_LEVEL}`. When ON, the editor builds
  *and* installs as part of the SDK. This decouples "build the editor" from "veng is the top-level
  project," so an SDK install (which may be driven from a packaging superbuild) gets the full
  authoring surface. The cooker + graph already build unconditionally (Plan 00), so this option now
  gates **only** the editor build/install — no `VENG_BUILD_TOOLS` interaction remains to reconcile.

- **Install `veng_graph`.** [`graph/`](../../graph) (`veng_graph`, aliased `veng::graph`) is linked
  PUBLIC by `veng_editor`, so it must join the export set: `install(TARGETS veng_graph EXPORT
  vengTargets …)` + its public headers, with an `INSTALL_RPATH` (`@loader_path/../lib` /
  `$ORIGIN/../lib`) so the installed shared lib finds `libveng` beside it. Its private
  `nlohmann/json` stays private (not exposed in a header).

- **Install `veng_editor` + headers.** `install(TARGETS veng_editor EXPORT vengTargets …)` (with the
  same `INSTALL_RPATH`) and `install(DIRECTORY editor/include/VengEditor …)` so a downstream
  `libgame_editor` (`EDITOR_MODULE`) can link `veng_editor::veng_editor`. imnodes/nlohmann remain
  PRIVATE (the imnodes header is vendored, never a `VengEditor/` public header — the
  `include_hygiene` sweep already guards this).

- **Export `veng-editor` as an imported executable, with `INSTALL_RPATH`.** `install(TARGETS
  veng-editor EXPORT vengTargets RUNTIME …)` so `$<TARGET_FILE:veng-editor>` in
  [`Editor.cmake`](../../cmake/Editor.cmake) resolves to the imported exe downstream, exactly as
  `vengc` does (Plan 00). `veng-editor` links the cooker statically/privately and resolves
  `libveng` / `libveng_editor` / `libveng_graph` + Slang at runtime, so its `INSTALL_RPATH` is
  `@loader_path/../lib` (macOS) / `$ORIGIN/../lib` (Linux) **plus** the `find_library(Slang …)` dir —
  the same Slang-is-a-host-prerequisite rule as `vengc` (Plan 00). Slang is not vendored.

- **`veng-config` recreates the editor/graph aliases.** After `include(veng-targets.cmake)`,
  `veng-config` recreates the in-tree alias spellings the helpers and a downstream `EDITOR_MODULE`
  use, each behind a `NOT TARGET` guard:
  `add_executable(veng-editor ALIAS veng::veng-editor)`,
  `add_library(veng::graph ALIAS veng::veng_graph)`,
  `add_library(veng_editor::veng_editor ALIAS veng::veng_editor)`.
  `libveng_cook` is statically linked into the exes and is **not** in the export set, so no `cook`
  alias is needed downstream — a consumer never links the cooker.

- **Install `Editor.cmake`; `veng_add_editor` resolves the imported editor.** Add
  `cmake/Editor.cmake` to the installed helper set (Plan 01 installed the rest) and `include()` it
  from `veng-config`. Its `if (NOT TARGET veng-editor)` guard now finds the recreated alias
  downstream; the `.veng/build.json` sidecar write and the `<name>-editor` run target are unchanged.

- **The build-output sidecar's `corePackManifest` is mode-aware.** `Editor.cmake` writes
  `VENG_CORE_PACK_JSON` into `.veng/build.json` for the editor's cook-on-demand to resolve core-pack
  ids; that variable is now the mode-resolved internal one from Plan 00, so the sidecar names the
  installed core manifest in a package build.

## Decisions

1. **`VENG_INSTALL_SDK` gates the editor only.** With `VENG_BUILD_TOOLS` retired (Plan 00), the
   cooker + graph are always present; `VENG_INSTALL_SDK` decides only whether the heavy GUI editor
   builds + installs, so a packaging superbuild produces a complete authoring SDK regardless of how
   the build was invoked, and the "editor links a cooker that wasn't built" inconsistency cannot
   arise.
2. **`veng_graph` and `veng_editor` join the existing export set, and `veng-config` recreates their
   aliases.** They are PUBLIC link deps of a downstream `EDITOR_MODULE`; the imnodes/nlohmann privacy
   boundary the in-tree build enforces carries through the install untouched, and the in-tree alias
   spellings (`veng::graph`, `veng_editor::veng_editor`) are reconstructed downstream so helper and
   consumer code is identical in both worlds.
3. **`veng-editor` is an imported exe with an explicit `INSTALL_RPATH`, like `vengc`.** One uniform
   mechanism for both bundled tools; `veng_add_editor` / `veng_add_project` bodies stay
   mode-agnostic, and the installed exe runs from the prefix because its rpath covers `lib/` + the
   host Slang.

## Files

| File | Change |
|---|---|
| `CMakeLists.txt` | `VENG_INSTALL_SDK` option; gate `add_subdirectory(editor)` on it (∪ top-level); install `veng_graph`, `veng_editor` + headers, and `veng-editor` (imported exe) into `vengTargets`, each with `INSTALL_RPATH`. |
| `cmake/veng-config.cmake.in` | Recreate the `veng-editor` / `veng::graph` / `veng_editor::veng_editor` aliases (each `NOT TARGET`-guarded); `include(Editor.cmake)`; `find_dependency` editor needs (none beyond veng's PUBLIC set). |
| `cmake/Editor.cmake` | Joined to the installed helper set; `veng-editor` resolved as the imported alias downstream; mode-aware `corePackManifest` in the sidecar. |
| `editor/CMakeLists.txt` | Export/install wiring for `veng_editor` + headers + `veng-editor`; `INSTALL_RPATH` (`lib/` + Slang dir) on the exe. |
| `graph/CMakeLists.txt` | Export/install wiring for `veng_graph` + headers; `INSTALL_RPATH` on the shared lib. |

## Verification

- Clean in-tree build + `ctest` green — the editor builds under the default
  (`VENG_INSTALL_SDK` = `PROJECT_IS_TOP_LEVEL`) gate as before; `veng_test_material_preview` /
  `veng_test_editor_unit` unaffected.
- `cmake --install build --prefix /tmp/veng-sdk` lays down `bin/veng-editor`,
  `lib/libveng_editor.*`, `lib/libveng_graph.*`, `include/VengEditor/…`, and a
  `veng-targets.cmake` naming `veng::veng-editor` imported. **Run it:** `/tmp/veng-sdk/bin/veng-editor
  --version` (a no-op flag) exits 0 from the prefix, proving the exe's `INSTALL_RPATH` resolves
  `libveng_editor` / `libveng_graph` / Slang. The `find_package` probe from Plan 01 now also sees
  `veng_add_editor` defined and the `veng_editor::veng_editor` / `veng::graph` aliases present.
- `include_hygiene` green (it links `veng_editor::veng_editor` and sweeps the `VengEditor/` public
  headers — the install must not change which headers are public).

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
