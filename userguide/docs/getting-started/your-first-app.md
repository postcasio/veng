# Your first application

A veng application is a shared library — your game module — loaded by a small
launcher. You subclass `Application`, register it, and build the two together. The
engine owns the window, render context, asset manager, and task system.

The `Application` subclass stays thin: it sets up the scene and renderer and drives
the frame. Gameplay logic lives in [scene systems](../scene/systems.md), not here.

## 1. Subclass `Application`

```cpp
#include <Veng/Application.h>

using namespace Veng;

class MyApp final : public Application
{
public:
    MyApp(const ApplicationInfo& info, TypeRegistry& types, SystemRegistry& systems)
        : Application(info, types, systems) {}

protected:
    void OnInitialize() override { /* create your scene, set up the renderer */ }
    void OnUpdate(f32 delta) override { /* advance the frame */ }
    void OnRender() override { /* record this frame's rendering */ }
    void OnDispose() override { }
};
```

The lifecycle methods:

| Method | Called |
| --- | --- |
| `OnInitialize()` | once, after the engine starts |
| `OnUpdate(delta)` | once per frame, before rendering (`delta` in seconds) |
| `OnRender()` | once per frame |
| `OnDispose()` | once, at shutdown |

`Application` hands you the systems you build on: `GetAssetManager()`,
`GetTaskSystem()`, `GetRenderContext()`, and `GetSystemRegistry()`.

## 2. Register the module

The launcher finds your application through one function the module exports. There
you register your component types, your gameplay systems, and a factory that
constructs your application:

```cpp
#include <Veng/Module/Module.h>

extern "C" void VengModuleRegister(VengModuleHost* host)
{
    host->Types.Register<MyComponent>();   // your component types
    host->Systems.Register<MySystem>();    // your scene systems

    host->App.RegisterApplication(
        [](TypeRegistry& types, SystemRegistry& systems) {
            return Unique<Application>(new MyApp(
                ApplicationInfo{ .Name = "My App", .WindowInfo = { .Title = "My App" } },
                types, systems));
        });
}

VE_EXPORT_MODULE_ABI()
```

`VE_EXPORT_MODULE_ABI()` records the engine version the module was built against; a
mismatched module is rejected at load.

See [Reflection & type registration](../scene/reflection.md) for component types
and [Game systems](../scene/systems.md) for systems.

## 3. Wire up the build

`veng_add_game` builds the game library and its launcher from one declaration, and
copies your cooked asset pack beside the launcher:

```cmake
veng_add_game(my_app
    SOURCES    main.cpp
    ASSET_PACK my_app_assets)
```

If your pack contains prefabs that reference your own component types, name your
module so the cooker can read them:

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

## Next

- Write gameplay: [Game systems](../scene/systems.md).
- Put something on screen: [The scene renderer](../rendering/scene-renderer.md)
  and [Entities & components](../scene/ecs.md).
- Give it assets: [Cooking asset packs](../assets/cooking.md) and
  [Loading at runtime](../assets/loading.md).
- `examples/hello-triangle` in the repo is a complete, working version of all of
  this.
