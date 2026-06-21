# Your first application

A veng application is a **shared library** (your "game module") loaded by a small
**launcher** executable. You subclass `Application`, override a few lifecycle
hooks, and register your app through one C-ABI entry point. The engine owns the
window, the render context, the asset manager, and the task system for you.

This page walks through the smallest complete app. For *why* it is split this
way, see [Architecture overview](../concepts/architecture-overview.md).

## 1. Subclass `Application`

```cpp
#include <Veng/Application.h>

using namespace Veng;

class MyApp final : public Application
{
public:
    using Application::Application;

protected:
    void OnInitialize() override
    {
        // Load assets, build your scene, create render resources.
    }

    void OnUpdate(f32 delta) override
    {
        // Advance game state. `delta` is seconds since the last frame.
    }

    void OnRender() override
    {
        // Record your rendering for this frame.
    }

    void OnDispose() override
    {
        // Release every engine resource you created (reset all Refs/Uniques).
    }
};
```

These four hooks are the whole lifecycle:

| Hook | When | Use it for |
| --- | --- | --- |
| `OnInitialize()` | once, after the engine is up | Loading assets, building the scene, creating render resources. |
| `OnUpdate(delta)` | once per frame, before render | Advancing game state. |
| `OnRender()` | once per frame | Recording draw work for the frame. |
| `OnDispose()` | once, at shutdown | Releasing every resource you created. |

!!! warning "Release your resources in `OnDispose()`"
    Reset every `Ref` and `Unique` you created. A resource that outlives the
    engine fails on destruction.

`Application` owns the major subsystems and hands them to you:

- `GetAssetManager()` — load and build assets.
- `GetTaskSystem()` — run work off the render thread.
- the render `Context` — threaded into every resource you create.

## 2. Register the module

The launcher finds your application through one function the module exports. In it,
you register a factory that constructs your `Application`:

```cpp
#include <Veng/Module/Module.h>

extern "C" void VengModuleRegister(VengModuleHost* host)
{
    host->App.RegisterApplication([](TypeRegistry& types) {
        return Unique<Application>(new MyApp(
            ApplicationInfo{
                .Name = "My App",
                .WindowInfo = { .Title = "My App" },
            },
            types));
    });
}

VE_EXPORT_MODULE_ABI()
```

`VE_EXPORT_MODULE_ABI()` stamps the module with the ABI version the launcher
checks at load — a stale module is rejected loudly rather than crashing later.

If your app defines its own components, this is also where you register their
reflected descriptors into `host->Types` — see
[Reflection & type registration](../scene/reflection.md).

## 3. Wire up the build

`veng_add_game` emits the game library *and* its launcher from one declaration,
and copies your cooked asset pack beside the launcher:

```cmake
veng_add_game(my_app
    SOURCES    main.cpp
    ASSET_PACK my_app_assets)
```

This builds `libmy_app` (your module) plus `my_app-launcher` (the exe that
loads it), cooks your asset pack, and places all three next to each other so the
result runs from any directory.

If your pack contains prefabs that reference your own component types, name your
module so the cook can reflect them:

```cmake
add_asset_pack(my_app_assets
    MANIFEST  assets/pack.json
    MODULE    my_app)
```

## 4. Run it

```sh
cmake --build build -j 4
build/.../my_app-launcher
```

## Where to go next

- Put something on screen: [The scene renderer](../rendering/scene-renderer.md)
  and [Entities & components](../scene/ecs.md).
- Give it assets: [Cooking asset packs](../assets/cooking.md) and
  [Loading at runtime](../assets/loading.md).
- The `examples/hello-triangle` directory in the repo is a complete, working
  version of everything above.
