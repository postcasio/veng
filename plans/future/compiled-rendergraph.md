# Compiled RenderGraph — design overview

> **The compiled core shipped in [planset-8](../planset-8/README.md).**
> `RenderGraph` is a builder; `Compile()` bakes the barrier/transition schedule,
> transient allocation/aliasing, per-graphics-pass `RenderingInfo`, and one-time
> validation into a `CompiledGraph` that replays per frame, and the resource model
> splits into graph-owned transients (resolved through `PassContext::Resolved`) and
> late-bound imports. The current behaviour lives in `RenderGraph.h` and the
> planset-8 README; this doc keeps the **enduring seams** that planset left for a
> later phase — dead-pass culling, multi-queue / async-compute scheduling, and the
> parallel-record story from the [threading/task system](threading-task-system.md)
> (area 2).

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

## The resource-model split (the real API shift)

A concrete-view-per-`Access` model cannot support aliasing: if two transients share
memory, neither has a stable concrete view a callback could capture at compile time.
So the compiled graph splits resources in two:

- **Graph-owned transients** are declared as **logical handles** (a typed
  `ResourceId` — extent/format/usage, no backing yet). The graph allocates and
  aliases them at compile; the **concrete** view is resolved per frame and handed to
  the callback. The callback may **not** close over it.
- **Imported resources** — the swapchain image, an app/`SceneRenderer`-owned output
  target — are external: declared by name, never allocated or aliased, their
  concrete view supplied per frame as a binding to `Execute`. They pin the graph's
  outputs (and so drive any future dead-pass culling).

This is exactly the `PassContext::Resolved(ResourceId)` channel the
[scene renderer](scene-renderer.md) design assumes: the record callback takes a
context that carries the cmd plus the per-frame resolved resources rather than a
bare `CommandBuffer&`. A pass authored against logical handles is what makes it a
**reusable unit** — it does not name concrete app resources.

```cpp
// Authoring records nothing; Compile once, then Execute many.
RenderGraph builder(context);
auto hdr  = builder.CreateTransient({ .Format = …, .Extent = …, .Usage = … }); // logical handle
auto swap = builder.Import("SwapChain");                                        // external, late-bound
builder.AddPass("Scene")
       .Color({ .Resource = hdr })
       .Execute([this](PassContext& ctx) { /* ctx.Resolved(...) + draw */ });

Unique<CompiledGraph> graph = builder.Compile();   // topology + barriers + aliasing, once
// per frame — bind the import's view for this frame, replay schedule, run callbacks:
graph->Execute(cmd, {{ swap, swapchainViewThisFrame }});
```

## Invalidation

A compiled graph is rebuilt when its **structure** changes — passes added/removed
(topology), or a transient's extent/format changes (allocation). Per-frame data
never invalidates it. The consumer drives the rebuild explicitly: with a separate
`RenderGraph` builder and `CompiledGraph`, there is no internal dirty flag to
maintain — the consumer knows when it changed topology or sizing and re-`Compile()`s
then. For the `SceneRenderer` consumer those triggers are precisely `Configure`
(topology) and `Resize` (sizing).

**Data-driven emptiness is not a recompile.** "No transparent objects this frame"
keeps the pass compiled-in and records **zero draws**; only settings-driven topology
recompiles. (Same rule the scene-renderer doc states.)

## Enduring seams

The compiled core, the resource-model split, and transient aliasing shipped. These
remain for a later phase:

- **Single compiled path.** There is no immediate-mode fallback: `RenderGraph` is a
  builder and `Compile()` → replay is the only execution path. The explicit
  `RenderGraph`/`CompiledGraph` split makes "compile once, replay many" impossible
  to misuse, which is why a separate per-frame-rebuild debugging mode was not kept.
- **Dead-pass culling.** With imported resources pinning the outputs, passes whose
  writes reach no output can be dropped. Cheap-ish on a linear graph; defer until a
  real pipeline produces dead passes.
- **Multi-queue / async compute.** The graph is single-queue. A compiled schedule
  *could* place passes across graphics/compute/transfer queues with semaphore sync —
  large scope, MoltenVK-fiddly, and entangled with area 2's queue work. The seam is
  noted, not built.

## Relationship to the scene renderer

`SceneRenderer` (area 8) **owns** a compiled graph and is the design's first real
consumer: `Create`/`Resize`/`Configure` are its recompile seams, `Execute` replays.
The compiled graph's `PassContext::Resolved` / `Compile` / recompile seams are
exactly what the scene renderer's public shape assumes — any `RenderGraph` consumer
gains the replay win.

## Status

The compiled core, transient/import resource model, `PassContext`, and transient
aliasing shipped in [planset-8](../planset-8/README.md). What remains is the seam
work above — dead-pass culling, multi-queue / async-compute scheduling, and area 2's
parallel-record story — each a candidate for a later phase, most naturally exercised
by the [scene renderer](scene-renderer.md) (area 8).
