# How an application is structured

A veng application is built as two pieces: a **game module** and a **launcher**.

The game module is a shared library holding your code — your `Application`
subclass, your components, and your custom types. The launcher is a small
executable that loads the module and runs it. `veng_add_game` builds both from one
declaration, so you don't write the launcher yourself.

The launcher loads your module and calls one function in it,
`VengModuleRegister`, where you register your `Application`:

```cpp
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

The launcher then constructs your application and runs it. `VE_EXPORT_MODULE_ABI()`
records the engine version the module was built against; a mismatched module is
rejected at load.

## What `Application` provides

You subclass `Application` and override its lifecycle methods (see
[Your first application](../getting-started/your-first-app.md)). It owns the
systems you use to build everything else:

- the render **context**, which you pass when creating GPU resources;
- the **asset manager** (`GetAssetManager()`), for loading and building assets;
- the **task system** (`GetTaskSystem()`), for running work off the render thread.

## A frame

Each frame, the engine calls your `OnUpdate(delta)` and then `OnRender()`, with
`delta` being the seconds elapsed since the last frame. Anything you started on a
worker thread and that has since finished is applied at the start of the frame,
before `OnUpdate` runs, so its result is ready by the time your code sees it.

Everything in a frame runs on one thread — see [The threading model](threading-model.md).
