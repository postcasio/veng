# veng

**veng** is a modern Vulkan rendering engine written in C++26. It gives you a
clean, high-level API for building real-time 3D applications and games, while
keeping all the Vulkan complexity — synchronization, memory, descriptors,
pipeline state — hidden behind it.

It is developed primarily on **macOS** (via MoltenVK) and written to be portable.

> **Status:** early and actively developed (`v0.1.0`). The API is still evolving.

---

## Requirements

- A **C++26**-capable compiler
- **CMake 4.1** or newer
- A **Vulkan SDK** (MoltenVK on macOS)

Everything else — GLFW, glm, zlib, fmt, Vulkan Memory Allocator, Dear ImGui, stb,
and the cooker's toolchain (assimp, Slang) — is downloaded and version-pinned
automatically by CMake during configuration. Nothing else to install.

---

## Installing & building

```sh
git clone <your-fork-url> veng
cd veng

cmake -B build -S .
cmake --build build
```

This produces the engine library (`libveng`), the asset cooker (`vengc`), and the
sample application.

To enable Vulkan validation layers while developing, configure a separate build
directory with `-DVE_DEBUG=ON`:

```sh
cmake -B build-debug -S . -DVE_DEBUG=ON
cmake --build build-debug
```

---

## Running the sample

The `hello-triangle` example is the quickest way to see veng working. After
building, run its launcher:

```sh
build/examples/hello-triangle/hello_triangle-launcher
```

A window opens showing a lit, textured sphere rendered through the deferred
pipeline, with an ImGui overlay. The launcher, the game library, and the cooked
asset pack form a self-contained directory you can copy and run anywhere.

You can also render a single frame off-screen to an image file, with no window —
handy for automated checks:

```sh
HT_SMOKE=/tmp/frame.ppm build/examples/hello-triangle/hello_triangle-launcher
```

---

## Using veng

A veng application is a shared library (your "game module") loaded by a small
launcher. You subclass `Application`, override a few lifecycle hooks, and register
it. The engine owns the window, the render context, the asset manager, and the
task system for you.

```cpp
#include <Veng/Application.h>
#include <Veng/Module/Module.h>

using namespace Veng;

class MyApp final : public Application
{
public:
    using Application::Application;

protected:
    void OnInitialize() override { /* load assets, build your scene */ }
    void OnUpdate(f32 delta) override { /* advance game state */ }
    void OnRender() override { /* render your scene */ }

    // A shutdown operation that must run while the app is still alive (not a
    // resource release) goes here; Run calls it before teardown. Most apps
    // need none — resources are members, freed by ~MyApp.
    void OnShutdown() override { /* optional: flush state, checkpoint, ... */ }

private:
    // Your engine resources are members here; the destructor frees them, in
    // reverse declaration order, before the engine's own members tear down.
};

// The launcher calls this once to discover your application.
extern "C" void VengModuleRegister(VengModuleHost* host)
{
    host->App.RegisterApplication([](TypeRegistry& types) {
        return Unique<Application>(new MyApp(
            ApplicationInfo{ .Name = "My App", .WindowInfo = { .Title = "My App" } },
            types));
    });
}

VE_EXPORT_MODULE_ABI()
```

The build system wires the two pieces together for you:

```cmake
veng_add_game(my_app
    SOURCES    main.cpp
    ASSET_PACK my_app_assets)
```

This builds your game library plus a launcher executable and copies your cooked
asset pack beside it.

### Assets

Assets are authored as JSON and cooked into a binary pack ahead of time. Each
asset has a small source file (`*.tex.json`, `*.mesh.json`, `*.shader.json`,
`*.vmat.json`, `*.prefab.json`) plus a manifest that lists them. The cooker turns
the manifest into a single `.vengpack` archive:

```sh
vengc cook my_pack.json -o my_pack.vengpack
vengc verify my_pack.vengpack   # check archive integrity
```

At runtime your app mounts the pack and loads assets by id. Geometry can also be
generated at runtime (cubes, planes, spheres) with no cooker involved.

The `examples/hello-triangle` directory is a complete, working reference for all
of this — application, scene setup, assets, and build wiring.

---

## Testing

```sh
ctest --test-dir build --output-on-failure
```

The test suite covers engine logic, GPU rendering, the asset cooker, and a
golden-image comparison of the sample's output. Tests that need a GPU skip
cleanly on machines without a Vulkan driver.

---

## Code style & linting

Formatting is enforced by `clang-format` (repo-root `.clang-format`). Static
analysis is configured in the repo-root `.clang-tidy` — a small, focused allowlist
of mechanical-convention checks (always-braced control flow, const-correctness,
designated initializers, and a few modernizations) that the whole tree is kept clean
against. clang-tidy is opt-in; turn it on during the build with:

```sh
cmake -B build -S . -DVENG_ENABLE_CLANG_TIDY=ON
```

A checked-in pre-commit hook format-checks and tidies **only the lines a commit
touches**, and skips cleanly when the tools are absent. Enable it once per clone:

```sh
git config core.hooksPath .githooks
```

---

## Code coverage

`VENG_ENABLE_COVERAGE` (default `OFF`) instruments veng's own sources for gcov, so
running the test suite leaves the counters `gcovr` turns into a report. `scripts/coverage.sh`
is the one-command path:

```sh
scripts/coverage.sh
```

It configures a dedicated `build-coverage/` tree with the option on, builds it, runs
the `unit` test selection, and writes `build-coverage/coverage.json` (machine-readable,
line- and branch-level) plus `build-coverage/coverage-html/index.html`. The selection is
one variable, so widening it needs no other change:

```sh
COVERAGE_CTEST_ARGS="-L unit -L gpu" scripts/coverage.sh
```

Coverage lives in its own build tree: the gcov flags (`--coverage`, `-O0`) suppress the
precompiled headers and fight the primary build's object caching, and the counter files a
run leaves behind should not accumulate in a tree used for ordinary work. `build-debug`
is unaffected — the option is off unless you ask for it.

The prerequisites are **gcovr** (`brew install gcovr` / `pip install gcovr`) and a
gcov-compatible tool. gcovr drives either through `--gcov-executable`, which is what makes
the same report reproducible on both toolchains: GNU builds use `gcov`, and Clang builds
use `xcrun llvm-cov gcov`, which the script selects on macOS. Third-party sources under
`_deps/` and the tests themselves are excluded, so the percentage describes first-party
code the tests are meant to cover.

---

## API documentation

veng's public headers are documented with Doxygen comments. If **Doxygen** is
installed, the build adds a `docs` target that generates a browsable HTML
reference of the public API:

```sh
cmake --build build --target docs
```

The output lands in `build/docs/html/index.html`. The target is optional —
without Doxygen installed it is simply absent and a normal build is unaffected.
