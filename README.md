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
- **GLFW**, **glm**, and **zlib**

Everything else — fmt, Vulkan Memory Allocator, Dear ImGui, stb, and the cooker's
toolchain (assimp, Slang) — is downloaded and version-pinned automatically by
CMake during configuration. Nothing else to install.

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
    void OnDispose() override { /* release your resources */ }
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
