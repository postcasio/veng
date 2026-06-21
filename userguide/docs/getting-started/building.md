# Building

veng is a standard CMake project. Configure once, then build.

```sh
git clone <your-fork-url> veng
cd veng

cmake -B build -S .
cmake --build build -j 4
```

This produces:

- **`libveng`** — the engine runtime (a shared library).
- **`vengc`** — the offline asset cooker CLI.
- **`hello-triangle`** — the sample application (a game module plus its launcher).

!!! warning "Cap parallelism at `-j 4`"
    If you parallelize the build, **do not go higher than `-j 4`**. The Slang and
    assimp builds are memory-hungry; more jobs can exhaust RAM.

## What builds when

Tests, examples, and the cooker build only when veng is the top-level project — if
you pull veng into a larger build as a subdirectory, you get just `libveng` and its
libraries. The toggles:

| Option | Default | Effect |
| --- | --- | --- |
| `VENG_BUILD_TESTS` | on (top-level) | Builds the test suites. |
| `VENG_BUILD_EXAMPLES` | on (top-level) | Builds `hello-triangle`. |
| `VENG_BUILD_TOOLS` | on (top-level) | Builds `vengc` and the cooker library. |
| `VENG_BUILD_DOCS` | on (if Doxygen found) | Adds the `docs` API-reference target. |

## The validation build

To debug Vulkan problems, build a separate directory with the Vulkan validation
layers enabled (`-DVE_DEBUG=ON`). Keep it apart from the default `build/` so you can
switch between a fast build and a checked one without reconfiguring:

```sh
cmake -B build-debug -S . -DVE_DEBUG=ON
cmake --build build-debug -j 4
```

Validation messages are written to the log. Both `build/` and `build-debug/` are
gitignored.

## The API reference

If `doxygen` is installed, the build adds a `docs` target that renders the
public headers' Doxygen comments into a browsable HTML reference:

```sh
cmake --build build --target docs
# output: build/docs/html/index.html
```

The same comments feed the [API reference](../api/index.md) section of this
guide, rendered natively into the site.

## Next

You now have a build. [Run the sample](running-the-sample.md) to see it working.
