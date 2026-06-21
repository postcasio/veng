# The render graph

The `RenderGraph` lets you author a frame's passes without hand-writing a single
layout transition or barrier. You declare a pass with the resources it **writes**
and **reads**, and the graph derives the synchronization for you.

## Barriers fall out of declared use

Don't write `vkCmdPipelineBarrier`. Declare a pass with its color writes
(`.Color(...)`) and its samples (`.Sample(...)`), and the graph derives the
layout transitions and drives `BeginRendering`/`EndRendering`.

Passes refer to resources by a `ResourceId`, not a concrete image:

- **`CreateTransient({.Format, .Extent, .Usage})`** declares a graph-owned
  transient. The graph allocates its `Image`/`ImageView` at compile time,
  resolves it per frame, and may **alias** non-overlapping transients onto shared
  backing memory.
- **`Import(name)`** declares an external resource — the swapchain image, an
  app-owned target. The graph never allocates or aliases it; its concrete view is
  supplied per frame as an `ImportBinding`.

## Recording: the `PassContext`

A pass's `Execute` callback receives a **`PassContext`**:

- `Cmd()` — the command buffer to record into, and
- `Resolved(ResourceId)` — the concrete view of a declared transient *this frame*.

```cpp
graph.AddPass("blur")
    .Sample(sourceId)
    .Color(blurredId)
    .Execute([=](PassContext& ctx) {
        ctx.Cmd().BindPipeline(blurPipeline);
        // ... draw fullscreen ...
    });
```

!!! warning "Don't capture a transient's view"
    A callback may **not** capture a transient's `ImageView` — an aliased
    transient has no fixed backing. Resolve it through the context at record time
    with `Resolved(id)`.

## Compile once, replay every frame

`RenderGraph` is a **builder**: declaring passes records nothing.

- **`Compile()`** derives the barrier/transition schedule, allocates transients,
  builds each graphics pass's `RenderingInfo`, runs one-time validation, and
  returns a `Unique<CompiledGraph>`.
- **`CompiledGraph::Execute(cmd, imports)`** replays that baked schedule each
  frame — only your per-pass callbacks run.

You re-`Compile()` **only on a structural change** — a pass added or removed, a
transient's extent or format changed. Per-frame data never recompiles: hold the
compiled graph across frames, bind imports per frame, and re-compile on resize.

```
Compile()  ──▶  CompiledGraph  ──Execute(cmd, imports)──▶  frame
   ▲                                                          │
   └────────── only on a structural change ◀──────────────────┘
```

The sample's `main.cpp` shows the pattern directly: a member compiled graph held
across frames (`BuildCompositeGraph`), imports bound per frame
(`CompositeToSwapChain`), re-compiled on resize.

## When to use it

The [scene renderer](scene-renderer.md) is itself built on a render graph, so most
apps never touch one directly. Reach for it when you need passes the scene renderer
doesn't provide — the sample uses one to composite the rendered scene and the ImGui
overlay onto the swapchain.
