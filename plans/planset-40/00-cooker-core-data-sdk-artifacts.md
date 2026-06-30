# Plan 00 — cooker + core data as SDK artifacts

**Goal:** make the cook *runnable from an installed veng* — install `vengc`, export it as an
**imported executable** in `vengTargets`, install the engine **core pack source** and the core
shader directory, retire `VENG_BUILD_TOOLS` so the tools are always built from source, and resolve
the core-data path variables mode-aware so a downstream cook finds them whether veng was discovered
in-tree or as a package. The foundation the helper modules (Plan 01) cook through. **Foundational —
nothing depends on it being merged first except the rest of the chain.**

## Why it is its own plan

`veng_add_project` / `veng_add_asset_pack` invoke `$<TARGET_FILE:vengc>` and thread
`--shader-include ${VENG_CORE_SHADER_DIR}` plus core-pack `--reference`s. Before any helper
module can run downstream, the *tool* and the *data* it needs must exist outside the source
tree. That is a self-contained install/export concern with no helper-body or editor change, so
it lands first and alone.

## What lands

- **Install `vengc`.** `install(TARGETS vengc EXPORT vengTargets RUNTIME DESTINATION
  ${CMAKE_INSTALL_BINDIR})` so the cooker exe ships with the SDK. It joins the **existing**
  `vengTargets` export set (the one `libveng`/`assetpack` already use), so it appears in the
  generated `veng-targets.cmake` as an imported executable target `veng::vengc`.

- **`vengc` is callable as `$<TARGET_FILE:vengc>` downstream.** The export gives `veng::vengc`;
  `veng-config` recreates the **unqualified** `vengc` target name the helpers use with
  `if (NOT TARGET vengc) add_executable(vengc ALIAS veng::vengc) endif()` — the same
  `if (NOT TARGET veng-editor)` guard [`Editor.cmake`](../../cmake/Editor.cmake) already uses.
  `add_executable(<name> ALIAS <imported-exe>)` is legal for an imported executable (verified on the
  CMake ≥ 4.1 this project requires) and `$<TARGET_FILE:vengc>` resolves to the imported location,
  so the helper bodies keep `$<TARGET_FILE:vengc>` **verbatim** in both worlds — no variable
  indirection, no `$<TARGET_FILE:...>` rewrite in Plan 01.

- **The installed `vengc` carries an `INSTALL_RPATH`.** `vengc` resolves `libveng` / `libveng_graph`
  through the build-tree auto-rpath today; an install strips that, so the install sets
  `INSTALL_RPATH` to `@loader_path/../lib` (macOS) / `$ORIGIN/../lib` (Linux) so the installed exe
  finds the co-installed libraries, **and** appends the directory `find_library(Slang …)` resolved
  to, so the installed `vengc` finds the host's Slang. Slang is **not** vendored: an installed veng
  requires the Vulkan SDK (with its Slang component) present to *run* the cooker — the same
  prerequisite the in-tree build already carries. Documented in Plan 06.

- **`VENG_BUILD_TOOLS` is retired.** The cooker (`vengc` + `libveng_cook`) and `veng::graph` now
  build **unconditionally whenever veng is built from source** (top-level or `add_subdirectory`) —
  veng *is* the tools now, so there is no "library without tools" from-source build. The
  `VENG_BUILD_TOOLS` option and its `if (NOT VENG_BUILD_TOOLS) return()` early-out in
  [`cooker/CMakeLists.txt`](../../cooker/CMakeLists.txt) are removed; the ~9 `VENG_BUILD_TOOLS` test
  gates in [`CMakeLists.txt`](../../CMakeLists.txt) fold into `VENG_BUILD_TESTS`. Consequence,
  accepted: a from-source `add_subdirectory(veng)` now requires the Vulkan SDK / Slang at configure
  time (`find_library(Slang)` runs unconditionally) and always builds assimp + Slang — a
  library-only consumer should `find_package(veng)` and get the prebuilt imported tools instead of
  building the cooker. (The editor's build/install stays gated — Plan 02's `VENG_INSTALL_SDK` ∪
  top-level — since it is the heavy GUI surface, not the cooker beneath it.)

- **Install the engine core data.** `install(DIRECTORY engine/assets/core/ DESTINATION
  ${CMAKE_INSTALL_DATADIR}/veng/core)` — the core pack manifest (`core.vengpack.json`), the
  vertex-layout JSONs, and `engine/assets/core/shaders/` (the `Veng/material.slang` header a
  consumer shader `#include`s and the standard per-domain vertex shaders a material references by
  core-pack id). A downstream cook needs the **source** manifest to resolve core-pack ids via
  `--reference`, and the shader dir on its Slang search path.

- **Mode-aware core-data variables, split by audience.** The **internal** build variables stay
  `VENG_CORE_SHADER_DIR` / `VENG_CORE_PACK_JSON` (uppercase) — the names the helper bodies read and
  the top-level [`CMakeLists.txt`](../../CMakeLists.txt) sets to source-tree paths today, unchanged
  in-tree. The **consumer-facing** names a downstream
  `veng_add_project(... REFERENCE ${veng_CORE_PACK_JSON})` reads are lowercase `veng_CORE_*`,
  matching the `veng_FOUND` / `veng_VERSION` result variables `find_package(veng)` auto-generates
  (CMake tracks the case of the `find_package` name; veng is lowercase). The generated `veng-config`
  (body written in Plan 01) sets the lowercase aliases — to
  `${PACKAGE_PREFIX_DIR}/${CMAKE_INSTALL_DATADIR}/veng/core/...` in install mode — **and** the
  uppercase internal names the installed helper bodies consume. The casing rule is **audience, not
  consumption mode**: uppercase `VENG_*` for engine-internal scaffolding a consumer never sees,
  lowercase `veng_*` for the find_package result API. This is an *added alias*, not a rename — the
  in-tree uppercase names are untouched. This plan hoists both into a single place the top-level
  build and the future `veng-config` set.

- **The embedded core pack is unaffected.** `libveng` still embeds its cooked core pack via the
  veng-free bootstrap cooker at engine-build time ([`CMakeLists.txt`](../../CMakeLists.txt)'s
  `veng_embed_binary(${VENG_CORE_PACK_BIN} g_CoreLayoutPack)`). The installed core-data *source*
  is for a **downstream cook**, not the engine's own embed — they are separate consumers of the
  same tree.

## Decisions

1. **The cooker ships with the SDK.** A downstream consumer cannot cook without `vengc`; it joins
   `vengTargets` as an imported executable rather than being re-built downstream (it carries heavy
   cooker-only deps — assimp, Slang, the texture encoders — a consumer must not inherit).
2. **The unqualified tool name is recreated as an alias, not a variable.** The helpers name `vengc`
   (and later `veng-editor`) without the `veng::` namespace; `veng-config` does
   `add_executable(vengc ALIAS veng::vengc)` behind a `NOT TARGET` guard, so the helper bodies stay
   byte-identical in-tree and downstream with no `$<TARGET_FILE:vengc>` rewrite.
3. **Installed tools carry an explicit `INSTALL_RPATH`.** The build-tree auto-rpath does not survive
   an install; the installed `vengc` gets `@loader_path/../lib` + the Slang dir so it runs from the
   prefix. Slang stays a host prerequisite, not a vendored artifact.
4. **`VENG_BUILD_TOOLS` is retired — veng is the tools.** A from-source build always builds the
   cooker + graph; the only library-only consumption path is `find_package(veng)` with prebuilt
   imported tools.
5. **Core data is installed as source, not cooked.** A downstream cook resolves core-pack ids from
   the source manifest and `#include`s the engine shader header; installing the source tree
   (manifest + layout JSONs + shaders) serves both, with no cooked artifact to keep in sync.

## Files

| File | Change |
|---|---|
| `CMakeLists.txt` | `install(TARGETS vengc EXPORT vengTargets RUNTIME …)` with `INSTALL_RPATH` (`@loader_path/../lib` + the Slang dir); `install(DIRECTORY engine/assets/core/ …)`; build the cooker + graph unconditionally (drop the `VENG_BUILD_TOOLS` gate, fold its test gates into `VENG_BUILD_TESTS`); hoist the internal `VENG_CORE_SHADER_DIR` / `VENG_CORE_PACK_JSON` for both the build and the future `veng-config`. |
| `cooker/CMakeLists.txt` | Remove the `VENG_BUILD_TOOLS` option + `if (NOT VENG_BUILD_TOOLS) return()` early-out so `vengc` / `libveng_cook` always build; set `vengc`'s `INSTALL_RPATH`; ensure `vengc` is exportable (visibility / target name). |

## Verification

- Clean in-tree build + `ctest` green — the in-tree path is unchanged for a top-level build (tools
  built as before, now unconditionally; the helpers still see source-tree `VENG_CORE_*` values;
  `vengc` is the same built target).
- `cmake --install build --prefix /tmp/veng-sdk` lays down `bin/vengc`, `share/veng/core/…` (the
  manifest + shaders), and a `veng-targets.cmake` naming `veng::vengc` as an imported executable.
  **Run the installed binary, not just inspect the layout:** `/tmp/veng-sdk/bin/vengc --help` (or a
  trivial cook) exits 0 from the prefix, proving the `INSTALL_RPATH` resolves `libveng` and Slang
  (full downstream consumption is Plan 05).
- `include_hygiene` / `validation_gate` unaffected — no public-header or runtime change.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
