# Plan 01 — Project reorganization into per-lib subdirectories

**Goal:** reshape the repository so each library has its own root subdirectory,
making room for the cooker and the shared format lib without tangling them into the
engine tree. Move the engine (`include/Veng` + `src`) under `engine/`, scaffold
empty `assetformat/` and `cooker/` subdirectories, and split the monolithic
top-level `CMakeLists.txt` into a thin top-level that `add_subdirectory`s each lib.
**Behaviour-preserving** — same binaries, same tests, same `#include <Veng/…>`
spellings.

## Why this is its own plan, and first

Every later plan adds files to one of these subdirectories; doing the move once,
alone, behind a green build keeps the rest of the planset from fighting path
churn. It is pure mechanical relocation + CMake surgery — exactly the kind of
risky-only-if-rushed change that deserves its own verified commit. Nothing in this
plan adds a feature.

## The target tree

```
/CMakeLists.txt          thin: project(), find_package, FetchContent of shared deps,
                         then add_subdirectory(engine|assetformat|cooker|examples|tests)
/engine/CMakeLists.txt   the libveng target (was the bulk of the old root CMake)
/engine/include/Veng/…   moved verbatim from /include/Veng
/engine/src/…            moved verbatim from /src
/assetformat/            empty scaffold this plan (CMakeLists declaring an INTERFACE
                         or empty STATIC target; real content in plan 02)
/cooker/                 empty scaffold this plan (CMakeLists; real content in plan 03)
/examples/  /tests/  /cmake/  /docs/  /plans/   unchanged location
```

## Work

1. **Move the engine.** `git mv include/Veng engine/include/Veng` and
   `git mv src engine/src`. The public include dir becomes `engine/include`, so
   `target_include_directories(veng PUBLIC engine/include)` keeps every
   `#include <Veng/…>` working unchanged. No source file edits.

2. **Split CMake.** Carve the `veng` target definition, its sources glob, its
   `target_link_libraries`/`target_include_directories`, the `add_shaders`
   plumbing it owns, and `VE_DEBUG`/validation options into
   `engine/CMakeLists.txt`. Keep `find_package` (Vulkan/GLFW/glm/zlib) and the
   shared `FetchContent` blocks (fmt, VMA, nfd, tinyexr, stb, ImGui, imnodes,
   doctest) in the **top-level** so all subdirs can use them. Top-level ends with
   `add_subdirectory(engine)`, then `assetformat`, `cooker`, and — guarded by
   `PROJECT_IS_TOP_LEVEL` + the existing toggles — `examples`/`tests`.

3. **Scaffold the two new libs.** `assetformat/CMakeLists.txt` and
   `cooker/CMakeLists.txt` declaring empty targets (`libveng_assetformat`,
   `libveng_cook`) so the tree builds. No sources yet.

4. **Fix relative paths.** Anything that referenced `${CMAKE_SOURCE_DIR}/src` or
   `include/` (the vendor TU include dirs for tinyexr/stb, the `include_hygiene`
   test's header sweep, shader output dirs) updates to the new locations. Grep for
   `src/` and `include/` in all `CMakeLists.txt` and `cmake/`.

## Dependencies

None — foundation of the planset. Must land before everything else.

## Acceptance

- `cmake -B build -S . && cmake --build build -j` clean from scratch (delete
  `build/` first — a moved tree invalidates the old cache).
- `ctest --test-dir build --output-on-failure` green (all existing tests).
- Smoke binary writes a correct-sized PPM
  (`HT_SMOKE=/tmp/ht.ppm build/examples/hello-triangle/hello_triangle`,
  1280×720 RGB ≈ 2,764,816 bytes).
- `VE_DEBUG` build (`build-debug/`) still configures and builds; the GPU binaries
  run from `build-debug/` with no *new* validation ERRORs.
- `git mv` preserves history (verify `git log --follow engine/src/Renderer/Backend/Context.cpp`).

## Notes

- **Behaviour-preserving** — if a single byte of generated SPIR-V or a single
  Vulkan call changes, something was edited that shouldn't have been. Only file
  locations and CMake move.
- `engine/include` PUBLIC vs `engine/src` PRIVATE is the same `include_hygiene`
  load-bearing split as before — preserve PUBLIC/PRIVATE linkage exactly.
- Keep the two new scaffold targets buildable-but-empty so CI/`ctest` stays green;
  plans 02/03 fill them. Don't link them into anything yet.
- This is the most delegatable plan in the set (pure mechanical churn) but the
  CMake split is fiddly — review the PUBLIC/PRIVATE and FetchContent placement on
  the main thread.
