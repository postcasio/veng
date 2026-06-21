# veng

**veng** is a Vulkan rendering engine written in C++26. It provides a high-level
API for building real-time 3D applications and games, and handles the Vulkan
details — synchronization, memory, descriptors, pipeline state — for you.

It is developed primarily on macOS via MoltenVK, and written to be portable.

!!! warning "Early and actively developed (`v0.1.0`)"
    The API is still evolving. This guide tracks the engine as it stands today.

It includes:

- a high-level deferred renderer with shadows, SSAO, and bloom, and a
  [render graph](rendering/render-graph.md) for building your own passes;
- a [scene and ECS layer](scene/index.md) whose components are plain structs;
- an [asset pipeline](assets/index.md) that cooks assets offline and loads them at
  runtime;
- a [debug UI](ui.md) over Dear ImGui, and a separate [editor](tools/editor.md).

## Where to start

<div class="grid cards" markdown>

-   :material-rocket-launch: **[Getting started](getting-started/index.md)**

    Install the prerequisites, build the engine, run the sample, and write your
    first application.

-   :material-sitemap: **[Concepts](concepts/index.md)**

    The things to understand first: how an application is structured, the single
    render thread, and error handling.

-   :material-cube-outline: **[Rendering](rendering/index.md)**

    The render graph, the deferred scene renderer, bindless resources, and the
    shader and material model.

-   :material-book-open-variant: **[API reference](api/index.md)**

    The full public surface, generated from the headers.

</div>

## A minimal application

A veng application is a shared library loaded by a small launcher. You subclass
`Application` and override its lifecycle methods:

```cpp
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
```

[Your first application](getting-started/your-first-app.md) covers the full
example, including registering the module and wiring up the build.
