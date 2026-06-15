# Plan 02 — Launcher + `veng_add_game` + sample migration

> **Stream A (game-module build model), plan 2 of 2.** Depends on plan 01. It migrates
> `examples/hello-triangle` to the launcher model — the sample file stream B's plan 04
> also edits, so **land this before plan 04** (see the README's *Dependencies &
> dispatching* section).

**Goal:** land the launcher and the build function, then reshape the example into the
two-artifact model in the same pass. Add a **generic veng-provided launcher** and
`veng_add_game(...)` emitting `lib<name>` (shared) + `<name>-launcher` (exe), both
consuming the `ApplicationRegistry` slot **plan 01 already defined**. Migrate
`examples/hello-triangle` to `libhello_triangle` + a launcher, with the `HT_SMOKE`
capture running **through the launcher**. This is the planset's single breaking change —
the API/build shift and the sample migration ship together, per the planset cadence.

## Why this is its own plan

The ABI/loader + `ApplicationRegistry` (plan 01) are verifiable in isolation; turning a
game into a shared library a launcher loads is the change that actually moves the example
and so must be done as one atomic pass — you cannot half-migrate an exe into a shared lib
+ launcher. Both new pieces (the launcher, the CMake function) exist only to serve that
migration, so they land with it.

## Consuming `ApplicationRegistry` (defined in plan 01)

A module fills the slot with `RegisterApplication(factory)`, passing an explicit
`function<Unique<Application>()>` it writes itself (plan 01's signature). The factory
captures the `ApplicationInfo` the game wants, so the `HT_SMOKE`/headless decision lives in
the module beside the app (below), not the launcher. The launcher reads it back with
`ApplicationRegistry::Create()`.

## The launcher — `engine/src/Launcher/launcher_main.cpp`

One generic `main`, shipped by veng, compiled per-game by `veng_add_game` with the
module filename baked in (`VENG_GAME_MODULE`, a compile definition):

```cpp
int main(int argc, char** argv)
{
    auto module = Veng::ModuleLoader::Load(VENG_GAME_MODULE);
    if (!module) { Veng::Log::Error("module load failed: {}", module.error()); return 1; }

    Veng::ApplicationRegistry apps;
    Veng::VengModuleHost      host{ .App = apps, .Editor = nullptr };
    module->Register(host);

    auto app = apps.Create();
    VE_ASSERT(app, "module registered no Application");
    app->Run(Veng::vector<Veng::string>(argv, argv + argc));
    return 0;
}
```

The launcher owns the `ApplicationRegistry` for the run, passes `Editor = nullptr`
(decision 4 — no editor host here), constructs the app from the factory, and runs it.
`Application` is **unchanged**: `Run()` still builds and owns
`Context`/`AssetManager`/`TaskSystem`. `VENG_GAME_MODULE` is the module's **file name**
(e.g. `libhello_triangle.dylib`), not an absolute path: the launcher `dlopen`s it by
name and the dynamic loader resolves it **beside the launcher binary** via an
`$ORIGIN`/`@loader_path` rpath the CMake function sets (below).

For the launcher + lib to be a genuinely relocatable pair, the **assets must resolve the
same way** — beside the launcher binary, not at an absolute build-tree path. veng has no
way today to find its own executable's directory, so this plan adds one:

```cpp
// engine/include/Veng/Application.h (declaration) + libveng impl, platform-split:
//   macOS:   _NSGetExecutablePath           Linux: readlink("/proc/self/exe")
//   Windows: GetModuleFileNameW
// Returns the directory containing the running executable (the launcher). path-typed,
// so it adds no backend include — include_hygiene stays green.
[[nodiscard]] VE_API path ExecutableDirectory();
```

The game mounts its pack relative to that directory (below), and `veng_add_game` copies
the cooked pack beside the launcher (below), so launcher + lib + pack travel together. A
copied directory runs unchanged — that is the property the verification actually
exercises (it copies the trio to a fresh directory and runs from there, not from the
build tree where an absolute path would resolve regardless).

## CMake — `cmake/Game.cmake` (`veng_add_game`)

Mirrors `add_asset_pack`'s shape; emits the shared lib + launcher from one call:

```cmake
# veng_add_game(<name>
#     SOURCES    <game sources...>       # the libgame translation units
#     [ASSET_PACK <pack target>]         # optional: an add_asset_pack target to depend on
# )
# Produces lib<name> (SHARED, links veng::veng) and <name>-launcher (exe, the veng
# launcher compiled with VENG_GAME_MODULE pointing at lib<name>). The EDITOR arm
# (lib<name>_editor) is reserved for the editor planset — libveng_editor does not
# exist yet — and is intentionally not emitted here.
# VENG_LAUNCHER_MAIN is the launcher source. In-tree (the only consumer this planset
# has — the example and the tests) it is the source-tree path, set right here:
set(VENG_LAUNCHER_MAIN "${CMAKE_CURRENT_LIST_DIR}/../engine/src/Launcher/launcher_main.cpp"
    CACHE INTERNAL "veng launcher source")

function(veng_add_game NAME)
    cmake_parse_arguments(ARG "" "ASSET_PACK" "SOURCES" ${ARGN})
    add_library(${NAME} SHARED ${ARG_SOURCES})
    target_link_libraries(${NAME} PRIVATE veng::veng)

    add_executable(${NAME}-launcher ${VENG_LAUNCHER_MAIN})   # the shipped veng launcher source
    target_link_libraries(${NAME}-launcher PRIVATE veng::veng)
    # The launcher dlopens the module by file name; resolve it beside the launcher
    # binary so the pair is relocatable (build tree and a shipped directory both work).
    target_compile_definitions(${NAME}-launcher PRIVATE
        VENG_GAME_MODULE="$<TARGET_FILE_NAME:${NAME}>")
    # Resolve the dlopen'd module beside the launcher binary. The BUILD_RPATH property is
    # APPENDED to CMake's auto-computed build rpath (it does not replace it), so the
    # launcher still finds libveng via the auto rpath while gaining @loader_path/$ORIGIN
    # for the game module. INSTALL_RPATH is set too (it DOES replace) so the same relative
    # resolution survives an install — though installed-package wiring is out of scope.
    if (APPLE)
        set(GAME_RPATH "@loader_path")
    else ()
        set(GAME_RPATH "$ORIGIN")
    endif ()
    set_target_properties(${NAME}-launcher PROPERTIES
        BUILD_RPATH   "${GAME_RPATH}"
        INSTALL_RPATH "${GAME_RPATH}")
    # Place lib<name> beside the launcher so @loader_path/$ORIGIN finds it.
    set_target_properties(${NAME} PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:${NAME}-launcher>)
    add_dependencies(${NAME}-launcher ${NAME})
    # Copy the cooked pack beside the launcher so ExecutableDirectory()-relative mounting
    # finds it; the trio (launcher + lib + pack) is then a self-contained, movable
    # directory. The pack target records its cooked-file path as a property (added to
    # add_asset_pack, below) so this copy reads it without the caller restating the path.
    if (ARG_ASSET_PACK)
        add_dependencies(${NAME}-launcher ${ARG_ASSET_PACK})
        add_custom_command(TARGET ${NAME}-launcher POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_PROPERTY:${ARG_ASSET_PACK},VENG_ASSET_PACK_OUTPUT>
                $<TARGET_FILE_DIR:${NAME}-launcher>)
    endif ()
endfunction()
```

`veng_add_game` is **in-tree only** this planset — it serves the example and the tests,
which build veng as the top-level project. `Game.cmake` is `include()`d from the top-level
`CMakeLists.txt` next to `AssetPack.cmake`. Making `veng_add_game` available to a
**downstream consumer of an installed veng** (installing `Game.cmake` +
`launcher_main.cpp`, wiring them into `veng-config.cmake.in`, resolving `VENG_LAUNCHER_MAIN`
to the installed path) is a separate packaging concern — `AssetPack.cmake` is itself not
installed today and `veng-config.cmake.in` references neither helper — and is **out of
scope here**, left to a future packaging plan rather than folded into this migration.

`add_asset_pack` gains one line so `veng_add_game` can find the cooked file:
`set_target_properties(${TARGET_NAME} PROPERTIES VENG_ASSET_PACK_OUTPUT ${ARG_OUTPUT})`
(it already has `ARG_OUTPUT`). This is the only edit to `AssetPack.cmake`.

## Sample migration — `examples/hello-triangle/`

- **The app moves into `libhello_triangle`.** `HelloTriangleApp` (its class +
  `OnInitialize`/`OnUpdate`/`OnRender`/`OnDispose`) stays as-is; what changes is the
  bottom of `main.cpp`. Delete `int main()`. Add `VengModuleRegister`:

  ```cpp
  extern "C" void VengModuleRegister(Veng::VengModuleHost* host)
  {
      const bool smoke = std::getenv("HT_SMOKE") != nullptr;
      host->App.RegisterApplication([smoke] {
          return Veng::Unique<Veng::Application>(new HelloTriangleApp(Veng::ApplicationInfo{
              .Name = "Hello Triangle",
              .InternalRenderExtent = {1280, 720},
              .WindowInfo = { .Extent = {1280, 720}, .Resizable = false,
                              .EventCallback = [](Veng::Event&) {}, .Title = "veng — Hello Triangle",
                              .CaptureMouse = false },
              .Headless = smoke,
          }));
      });
  }
  ```

  The `HT_SMOKE`/headless decision and the `ApplicationInfo` move here verbatim from
  the old `main()` — the launcher stays game-agnostic. The module owns the factory
  lambda and captures exactly `smoke`.

- **The pack mount becomes executable-relative.** The old
  `path(HT_ASSET_DIR) / "sample.vengpack"` (an absolute build-tree path) becomes
  `Veng::ExecutableDirectory() / "sample.vengpack"`, resolving the pack copied beside the
  launcher by `veng_add_game`. This is what makes the migrated sample actually
  relocatable, not just build-tree-runnable.

- **`examples/hello-triangle/CMakeLists.txt`** swaps `add_executable(hello_triangle …)`
  for `veng_add_game(hello_triangle SOURCES main.cpp ASSET_PACK hello_triangle_assets)`.
  The `add_asset_pack(hello_triangle_assets …)` call stays (it still cooks into the build
  tree, and `veng_add_game` copies the result beside the launcher). The **`HT_ASSET_DIR`
  compile definition is dropped** — the app no longer reads an absolute path at runtime;
  `HT_ASSET_DIR` survives only as the CMake-local cook `OUTPUT` directory. The runnable
  becomes `hello_triangle-launcher`.

## Verification

- Clean build produces `libhello_triangle` (shared) + `hello_triangle-launcher` (exe),
  with `sample.vengpack` copied beside the launcher; `ctest` green (the registered
  `headless_smoke` test links `veng` directly and is unaffected; `loader_test` from plan
  01 still passes).
- **Automated launcher smoke (CI coverage of the new shipping path).** Register a
  `hello_triangle_launcher_smoke` `add_test` that runs `hello_triangle-launcher` with
  `HT_SMOKE` pointed at a temp PPM and asserts exit 0 (label `gpu`, `SKIP_RETURN_CODE 77`
  like `headless_smoke`, so it skips where no device is present). This is the only test
  that exercises the real `dlopen` → `VengModuleRegister` → registry → `Run()` binary path
  end-to-end — without it the migration's headline change has zero automated coverage.
  Add `$<TARGET_FILE:hello_triangle-launcher>` to the validation gate
  (`cmake/ValidationGate.cmake`) so the launcher path runs under `VE_DEBUG` too.
- **Relocatability (the property the plan claims).** Copy the trio to a fresh directory
  and run from there — *not* from the build tree, where any absolute path would resolve
  regardless:
  ```sh
  D=$(mktemp -d)
  cp build/examples/hello-triangle/hello_triangle-launcher \
     build/examples/hello-triangle/libhello_triangle.* \
     build/examples/hello-triangle/sample.vengpack "$D"/
  ( cd /tmp && HT_SMOKE=/tmp/ht-reloc.ppm "$D"/hello_triangle-launcher )   # cwd is NOT $D
  ```
  Exits 0 and writes a correct-sized PPM (1280×720 RGB ≈ 2,764,816 bytes), proving the
  module (`@loader_path`/`$ORIGIN` rpath) and the pack (`ExecutableDirectory()`-relative
  mount) both resolve beside the launcher independent of the working directory. (`libveng`
  itself still resolves via the launcher's auto build rpath — `veng_add_game` *appends*
  `@loader_path` to it rather than replacing it — so the copy need not include `libveng`;
  shipping `libveng` beside the launcher is the out-of-scope installed-package concern.)
- A windowed run (`build/examples/hello-triangle/hello_triangle-launcher`, no env)
  shows the rotating textured primitive — same scene as before the migration.
- `ctest --test-dir build-debug -L validation` green (now including the launcher path via
  the gate addition above); the render/load path is byte-for-byte the same (only
  construction + linkage changed), so the allowlist stays empty.

## Acceptance

- The example is `libhello_triangle` + `hello_triangle-launcher`; no `int main()`
  survives in the example; the app is constructed via the module factory.
- `veng_add_game` produces both artifacts from one declaration and copies the pack beside
  the launcher; `add_asset_pack` integration is preserved.
- The launcher + lib + pack are a **relocatable trio**: copied to a fresh directory and
  run with an unrelated working directory, the launcher writes a correct-sized PPM and
  exits 0.
- `hello_triangle_launcher_smoke` runs the launcher binary automatically under `ctest` and
  the validation gate; windowed run renders; `ctest` and the validation gate are green.
