# Compiled RenderGraph — design overview (future)

> **Vision / design sketch, not scheduled.** Direction for moving `RenderGraph`
> from its current **immediate-mode** form (a per-frame vector of pass structs) to
> a **compiled** graph that caches topology, barriers, and transient allocation
> once and replays them per frame. The enabling prerequisite for the
> [scene renderer](scene-renderer.md) (area 8), and a natural home for parallel
> pass recording from the [threading/task system](threading-task-system.md)
> (area 2). Builds directly on the shipped `RenderGraph` (planset-5 era).

## Where it is now

`RenderGraph` is **immediate-mode and linear** (`RenderGraph.h`): each frame the
app declares passes with the image views they read and write, the engine derives
every barrier from those declarations, and `Execute(cmd)` emits the barriers and
runs each pass's lambda in declaration order. Its own header is explicit — *"The
graph is rebuilt per frame (it is just a vector of pass structs)."* There is **no
culling, reordering, or aliasing**, all accesses take a concrete app-owned
`Ref<ImageView>`, and graphics passes get `BeginRendering`/`EndRendering` driven
from their `Color`/`Depth` declarations.

That is the right v1: simple, correct, no hidden lifetime analysis. What it spends
every frame is barrier derivation and the rebuild of the pass list — cheap at
hello-triangle scale, not free once a scene renderer fans out many passes across N
editor viewports.

## What "compiled" caches — and what it does not

Compilation splits the work into two phases. The line is **topology vs. draws**:

- **Compile** (once, on a structural change): the pass set + order, the **derived
  barrier/layout schedule**, **transient resource allocation/aliasing**, and the
  attachment descriptions. Validated once.
- **Record** (every frame): each pass's callback still runs — *which objects you
  draw, camera matrices, push-constant indices, cull results* cannot be baked. The
  compiled graph replays the fixed barrier schedule and invokes the callbacks.

So the cache is **structure, not commands.** The draw recording stays per-frame;
what stops being redone is barrier derivation, transient sizing, and validation.

## What it buys

- **No per-frame barrier derivation.** The schedule is computed once and replayed.
- **Transient aliasing / pooling.** With a fixed pass order, each transient's live
  range is a trivial pass-index interval (first write … last read); non-overlapping
  transients share backing memory. The linear, no-reorder model makes this analysis
  almost free — a real reason to do it here rather than in a general graph.
- **One-time validation.** Cycles, read-before-write, format/usage mismatches are
  caught at compile, not re-checked per frame.
- **Parallel record (area 2).** A compiled schedule with independent, side-effect-
  free pass callbacks is what lets passes record into **secondary command buffers**
  across task-system workers — the barrier schedule is already fixed, so workers
  only fill in draws.

## The resource-model change (the real API shift)

Today every `Access` names a concrete, app-owned `Ref<ImageView>`. Aliasing breaks
that: if two transients share memory, neither has a stable concrete view a callback
could capture at compile time. So the compiled graph splits resources in two:

- **Graph-owned transients** are declared as **logical handles** (a typed
  `ResourceId` — extent/format/usage, no backing yet). The graph allocates and
  aliases them at compile; the **concrete** view is resolved per frame and handed to
  the callback. The callback may **not** close over it.
- **Imported resources** — the swapchain image, an app/`SceneRenderer`-owned output
  target — stay concrete `Ref<ImageView>`s, registered with the graph as external.
  They are never aliased and pin the graph's outputs (and so drive any future dead-
  pass culling).

This is exactly the `PassContext::Resolved(ResourceId)` channel the
[scene renderer](scene-renderer.md) design assumes: the record callback changes
from `function<void(CommandBuffer&)>` to taking a context that carries the cmd plus
the per-frame resolved resources. A pass authored against logical handles is what
makes it a **reusable unit** — it does not name concrete app resources.

```cpp
// Sketch — authoring is unchanged; you Compile once, then Execute many.
RenderGraph builder;
auto hdr = builder.CreateTransient({ .Format = …, .Extent = … });   // logical handle
builder.Import(swapchainView);                                       // external, concrete
builder.AddPass("Scene")
       .Color({ .Handle = hdr })
       .Execute([this](PassContext& ctx) { /* resolve + draw */ });

CompiledGraph graph = builder.Compile();   // topology + barriers + aliasing, once
// per frame:
graph.Execute(cmd, frameInputs);           // replay schedule, run callbacks
```

The authoring surface (`AddPass`/`.Color`/`.Sample`/…) stays; what changes is the
**`Compile()` → replay** lifecycle replacing rebuild-every-frame, and transient
attachments referencing logical handles instead of concrete views.

## Invalidation

A compiled graph is rebuilt when its **structure** changes — passes added/removed
(topology), or a transient's extent/format changes (allocation). Per-frame data
never invalidates it. For the `SceneRenderer` consumer those triggers are precisely
`Configure` (topology) and `Resize` (sizing); a standalone consumer keys recompile
off the same two. Whether invalidation is an explicit dirty flag or a structural
hash of the declaration is an open detail — start explicit.

**Data-driven emptiness is not a recompile.** "No transparent objects this frame"
keeps the pass compiled-in and records **zero draws**; only settings-driven topology
recompiles. (Same rule the scene-renderer doc states.)

## Open forks & scope

- **Alias transients in v1, or barriers-only first?** The schedule cache and the
  resource-model split are the load-bearing change; transient **aliasing** is a
  memory optimization on top. A first cut could compile the barrier schedule and
  give each transient its own allocation, adding aliasing once the lifetime analysis
  is trusted. Recommended split.
- **Dead-pass culling.** With imported resources pinning the outputs, passes whose
  writes reach no output can be dropped. Cheap-ish on a linear graph; defer until a
  real pipeline produces dead passes.
- **Multi-queue / async compute.** The graph is single-queue today. A compiled
  schedule *could* place passes across graphics/compute/transfer queues with
  semaphore sync — large scope, MoltenVK-fiddly, and entangled with area 2's queue
  work. Out of scope for the first compiled graph; note the seam, don't build it.
- **Keep an immediate-mode path?** The per-frame-rebuild path is a simple fallback
  and a debugging aid (no stale compiled state). Decide whether `Compile()` is
  mandatory or whether immediate execution remains a supported mode.

## Relationship to the scene renderer

`SceneRenderer` (area 8) **owns** a compiled graph and is the design's first real
consumer: `Create`/`Resize`/`Configure` are its recompile seams, `Execute` replays.
The two were designed together — the scene renderer's public shape is unchanged
across the immediate→compiled migration precisely because those seams exist. This
doc can also land independently; any `RenderGraph` consumer gains the replay win.

## Status

Vision only. Becomes its own planset when taken up — most naturally **ahead of or
folded into** the [scene renderer](scene-renderer.md) (area 8), which is what
exercises it, and benefiting from area 2's parallel-record story when that lands.
