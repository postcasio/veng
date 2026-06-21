# How an application is structured

A veng application is built as two pieces: a **game module** and a **launcher**.

The game module is a shared library holding your code — your `Application`
subclass, your components, and your [systems](../scene/systems.md). The launcher is
a small executable that loads the module and runs it. `veng_add_game` builds both
from one declaration, so you don't write the launcher yourself.

The launcher loads your module and calls one function in it, `VengModuleRegister`,
where you register your types, your systems, and your application:

```cpp
extern "C" void VengModuleRegister(VengModuleHost* host)
{
    host->Types.Register<MyComponent>();
    host->Systems.Register<MySystem>();

    host->App.RegisterApplication(
        [](TypeRegistry& types, SystemRegistry& systems) {
            return Unique<Application>(new MyApp(
                ApplicationInfo{ .Name = "My App", .WindowInfo = { .Title = "My App" } },
                types, systems));
        });
}

VE_EXPORT_MODULE_ABI()
```

The launcher then constructs your application and runs it. `VE_EXPORT_MODULE_ABI()`
records the engine version the module was built against; a mismatched module is
rejected at load.

## What `Application` provides

You subclass `Application` and override its lifecycle methods (see
[Your first application](../getting-started/your-first-app.md)). The subclass is
thin — it sets up the scene and renderer and drives the frame, while gameplay logic
lives in [scene systems](../scene/systems.md). `Application` owns the services you
build on:

- the render **context** (`GetRenderContext()`), which you pass when creating GPU
  resources;
- the **asset manager** (`GetAssetManager()`), for loading and building assets;
- the **task system** (`GetTaskSystem()`), for running work off the render thread.

## A frame

Each frame, the engine calls your `OnUpdate(delta)` and then `OnRender()`, with
`delta` being the seconds elapsed since the last frame. Anything you started on a
worker thread and that has since finished is applied at the start of the frame,
before `OnUpdate` runs, so its result is ready by the time your code sees it.

Everything in a frame runs on one thread — see [The threading model](threading-model.md).
