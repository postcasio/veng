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

This produces the engine library (`libveng`), the asset cooker (`vengc`), the
trace converter (`vengtrace`), and the sample application.

To enable Vulkan validation layers while developing, configure a separate build
directory with `-DVE_DEBUG=ON`:

```sh
cmake -B build-debug -S . -DVE_DEBUG=ON
cmake --build build-debug
```

---

## Build options

The load-bearing configure-time options (pass `-D<NAME>=ON|OFF`):

| Option | Default | Effect |
|--------|---------|--------|
| `VE_DEBUG` | `OFF` | Vulkan validation layers, asserts, `-Werror`; also selects a Debug build type. |
| `VE_PROFILE` | ON under `VE_DEBUG`, else OFF | Compiles in the diagnostics profiler (scope timing, per-thread trace buffers). It is a `PUBLIC` compile definition on the `veng` target — owned by the engine and propagated to every consumer, **never set by a consumer**, since a consumer whose macro expansion disagrees with the engine it links is an ABI split. With it off, every `VE_PROFILE_*` macro expands to nothing and no event-recording or buffer code is built. |
| `VENG_ENABLE_CLANG_TIDY` | `OFF` | Runs clang-tidy per-TU during the build. |
| `VENG_ENABLE_COVERAGE` | `OFF` | Instruments veng's own sources for gcov. |
| `VENG_TIME_TRACE` | `OFF` | Emits a clang `-ftime-trace` compile profile beside every object file, so the build's own cost is measurable. Turns the configure into a full cold build (the compiler cache is disabled) and lands gigabytes of JSON — see [Build-time tracing](#build-time-tracing). |

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

### Profiling

When built with the diagnostics profiler (`VE_PROFILE`, on by default under
`VE_DEBUG`), the engine can write a compact binary capture of a run's CPU and GPU
timing. The `vengtrace` tool converts a capture to Chrome Trace Event JSON, which
opens in [ui.perfetto.dev](https://ui.perfetto.dev) or
[speedscope.app](https://speedscope.app):

```sh
vengtrace convert build-debug/captures/run.vtrace --out run.json
```

See [docs/guides/profiling-captures.md](docs/guides/profiling-captures.md) for the
full workflow and what each track means.

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
COVERAGE_CTEST_ARGS="-L unit|gpu" scripts/coverage.sh
```

Widen with a single `-L` and a regex alternation. CTest requires a test to match
*every* `-L` it is given, so repeating the flag (`-L unit -L gpu`) selects only tests
carrying both labels — usually none — and the run then reports success over an empty
selection.

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

## Build-time tracing

`VENG_TIME_TRACE` (default `OFF`) adds clang's `-ftime-trace` to veng's own targets, so the
compiler writes a Chrome-Trace profile of each translation unit's own compilation — the
frontend/backend split, template instantiation, per-header parse cost — beside that TU's
object file. It is wired at the same boundary as the clang-tidy and coverage options, so
vendored and fetched sources are never traced and their compile time cannot dilute the
aggregate.

A tracing build goes in its own tree, `build-trace/`, a sibling of `build-coverage/`, so it
never disturbs `build-debug`:

```sh
cmake -B build-trace -S . -DVE_DEBUG=ON -DVENG_TIME_TRACE=ON
cmake --build build-trace
```

**Where the output lands.** One `.json` per translation unit, inside the build tree, under
that target's object directory, named after the object it sits beside — e.g.
`build-trace/engine/CMakeFiles/veng.dir/src/Renderer/Context.cpp.json`. Nothing moves it;
that layout is the contract anything reading the traces scans.

**What it costs, up front.** The option disables the compiler cache — `-ftime-trace` writes
a side output ccache does not reproduce, so a cache hit would yield an object with no trace
beside it, and a partial tree that reads as a complete one. Every `VENG_TIME_TRACE=ON`
configure is therefore a **full cold build**. Measured on the reference host (Apple M2, `-j 6`,
627 translation units): **3:53 of wall clock, ~1097 s of compile CPU, a 3.3 GB tree carrying
216 MB of JSON**. That is the reason the option defaults off.

**It is a measurement build, not a normal one.** The flag changes what the compiler does, not
merely what it emits: it costs frontend time and writes a file per TU. Compare a tracing
build's timings only against another tracing build's, never against a normal build's.

**Two properties of the format.** A trace's `ts` values **restart at zero in every document**, so
they order events within one TU and say nothing about where in the build that TU compiled — the
only build ordering the tree carries is the trace files' mtimes. And **a namespace-scope
instantiation placed in a precompiled header is serialised into the PCH and is not re-paid per
TU**: a PCH caches parsed declarations *and* whatever was instantiated while it was built, so
such an instantiation appears once and nowhere else.

### Guarding the build cost

`docs/build-cost-baseline.md` records this tree's whole-tree compile cost — the totals, the
frontend/backend split, and the template-instantiation breakdown by origin — together with the
provenance that makes the figure meaningful (compiler, build type, option set, host, TU count,
build span, git SHA). `scripts/check_build_cost.py` both produces that file and checks a tree
against it, parsing the `-ftime-trace` JSON directly with no external tooling:

```sh
python3 scripts/check_build_cost.py --tree build-trace                    # check
python3 scripts/check_build_cost.py --tree build-trace --write-baseline   # refresh
```

Exit codes are `0` pass, `1` regression, `2` skipped, `3` inconclusive. Run it after any change
to the reflection headers, the include graph, or the precompiled-header set; whoever lands such a
change either passes the check or refreshes the baseline in the same commit **and says in the
commit body why the number moved**. It fails when total compile CPU exceeds the baseline by more
than 5 %, or when a single origin's instantiation total exceeds its baseline by more than 10 %
*and* more than 5 s. A provenance mismatch **skips** rather than fails — compile CPU varies by a
factor of ~2 across CPU models. It is not a `ctest` test: it needs a tracing tree no ordinary
build produces.

**How to read a delta.** `-ftime-trace` durations are wall time *inside* the compile, so the
machine's own behaviour lands in them as compile cost. Three consequences, all measured: a
cluster of translation units an order of magnitude above the next is a **stall artifact**, and
the check reports it as **inconclusive** rather than failed; the recorded **build span** is
provenance, and a large mismatch against the baseline's means the totals are not comparable; and
whole-tree totals vary by **≈11 % run to run** on the reference host, concentrated in the last
build deciles, which puts the 5 % total threshold below the noise and makes the per-origin rule
the load-bearing half. Trust the figures in this order: the number of TUs paying an origin, then
the median per-TU ratio, then the per-origin totals, then the whole-tree total.

**Cleaning up.** Delete `build-trace/` outright — the objects and the JSON are only ever
regenerated as a pair, so removing the tree reclaims both. Nothing refreshes it on a schedule
and nothing depends on it being current; it is a deliberately-made artifact you refresh when
you want to profile the build and remove when you are done. `build-trace/` is gitignored.

**Mirroring it downstream.** A project that consumes veng and wants the same view of *its own*
build defines the same shape of option over its own targets: a cache option defaulting `OFF`;
declared ahead of whatever assigns `CMAKE_CXX_COMPILER_LAUNCHER`, so the launcher branch can
read it; opting out of the compiler cache through that single existing assignment rather than
adding a second one (CMake bakes the first launcher value into the compile rule and *appends*
later ones, which is silently wrong rather than an error); disabling any *other* cache
injection its dependency tree performs, since a third-party `CMakeLists` that sets a **global
`RULE_LAUNCH_COMPILE`** prefixes every target's compile rule without going near the launcher
variable at all; applied at the boundary that
already scopes warnings and precompiled headers to first-party targets; and leaving the
per-object output location where clang puts it. `-ftime-trace` is clang-specific, so the
compiler test is `if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")` — `MATCHES`, not `STREQUAL`,
since `AppleClang` must match — with a `message(WARNING …)` on the else branch rather than a
silent no-op.

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
