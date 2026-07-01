# Consuming veng from a game project

A game project lives **outside** the engine tree and discovers veng as a normal
CMake package: `find_package(veng)` brings the library `veng::veng`, the cooker
`vengc`, the shared `veng-editor`, and the full authoring vocabulary
(`veng_add_project` / `veng_add_game` / `veng_add_editor` / `veng_add_asset_pack`).
The same `find_package(veng)` resolves against three sources — pick the one that
matches how you are working.

## The three consumption modes

```
in-tree         add_subdirectory(veng)            veng is the top-level project    — the engine's own build
build tree      find_package(veng) → veng/build   co-develop engine + game, NO install
install prefix  find_package(veng) → <prefix>     the shipped SDK
```

A game's `CMakeLists.txt` is identical across all three — only the discovery
incantation on the configure line differs. A minimal game:

```cmake
cmake_minimum_required(VERSION 4.1)
project(mygame LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 26)

find_package(veng REQUIRED)

veng_add_game(mygame SOURCES src/main.cpp PROJECT mygame_project)
```

### Build tree — co-develop the engine and the game, no install

Point the game at the engine's build directory. `find_package(veng)` resolves the
build-tree `veng-config.cmake` the engine writes there; the imported `vengc` /
`veng-editor` point at their build-tree binaries and the core-data / launcher paths
resolve to the **live engine source tree**.

```sh
# once: configure the engine
cmake -B veng/build -S veng

# once: configure the game against the engine build tree
cmake -B mygame/build -S mygame -Dveng_ROOT=$PWD/veng/build
```

`veng_ROOT` (or putting `veng/build` on `CMAKE_PREFIX_PATH`) is the whole of the
wiring. From then on the loop is **reinstall-free**:

```sh
# edit the engine  → rebuild it in place; the exported targets refresh
cmake --build veng/build

# edit the game     → its next build picks up the refreshed engine
cmake --build mygame/build
```

An engine *source* edit (a helper module, the core shaders, the launcher) is visible
to the game on its next configure/build because the build-tree config resolves those
paths to the live checkout, not to copied install artifacts.

### Install prefix — the shipped SDK

Install veng to a prefix, then point the game at it. `VENG_PACKAGE_MODE` resolves to
`INSTALL` and the core-data / launcher paths resolve under the prefix.

```sh
cmake -B veng/build -S veng
cmake --build veng/build
cmake --install veng/build --prefix /opt/veng

cmake -B mygame/build -S mygame -DCMAKE_PREFIX_PATH=/opt/veng
```

### FetchContent redirect — a game that vendors the dependency declaration

A game repo may instead *declare* veng as a pinned `FetchContent` dependency and,
for local development, redirect the pinned tag to a live checkout with
`FETCHCONTENT_SOURCE_DIR_VENG` — transparently an `add_subdirectory` of the live
engine source (the in-tree consumption mode).

```cmake
include(FetchContent)
FetchContent_Declare(veng
    GIT_REPOSITORY https://…/veng.git
    GIT_TAG v0.1.0)
FetchContent_MakeAvailable(veng)
```

```sh
cmake -B mygame/build -S mygame -DFETCHCONTENT_SOURCE_DIR_VENG=$PWD/veng
```

Both `find_package` (engine built separately) and `FetchContent` (with a
local-checkout redirect) are supported; neither is privileged.

## What `find_package(veng)` defines

- **Targets** — `veng::veng` (the library), the imported `vengc` and `veng-editor`
  executables (recreated under their unqualified names so `$<TARGET_FILE:vengc>`
  resolves), and the `veng::graph` / `veng_editor::veng_editor` library aliases.
- **The authoring vocabulary** — `veng_add_project`, `veng_add_asset_pack`,
  `veng_add_game`, `veng_add_editor`, and the build-configuration helpers.
- **Result variables** — `veng_CORE_PACK_JSON` / `veng_CORE_SHADER_DIR` (a
  downstream cook references them) and `VENG_PACKAGE_MODE` (`INSTALL` /
  `BUILD_TREE`, unset in-tree).
