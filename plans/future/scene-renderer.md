# Scene renderer — design overview (future)

> **Vision / design sketch, not scheduled.** Direction and recommendations for a
> long-lived, configurable scene-rendering object that sits **on top of**
> `RenderGraph`. Builds on the shipped renderer / bindless / material foundation
> (planset-5) and interacts with three other future pieces: the **compiled
> [`RenderGraph`](compiled-rendergraph.md)** (area 9 — the enabling prerequisite),
> the
> [threading/task system](threading-task-system.md) (area 2 — the frames-in-flight
> contract and parallel pass recording), and the [editor](editor.md) (area 6 — the
> prime consumer, whose viewport panels each drive one of these). The **scene** it
> renders is the [scene/entity model](README.md#7-scene--entity-model) (area 7).

## What it is

`SceneRenderer` is a **long-lived, opinionated render pipeline**: constructed once
with an output format and a settings block, it owns the GPU resources its passes
need and (eventually) a compiled graph, and renders a `Scene` from a camera into
an **offscreen target it owns**, handing back a sampleable result. The game's main
view and every editor preview panel are the same object wired two ways — the
editor renders **one `Scene` through N `SceneRenderer`s**, each with its own
camera, settings, and target ([editor.md](editor.md): each preview surface renders
into its own offscreen RT).

It is **batteries-included, not an extension mechanism.** The über-pipeline (the
fixed wiring of interdependent passes) is the product; authoring a bespoke pass
graph still means dropping to `RenderGraph` directly (the hello-triangle pattern).
Keeping that line sharp is what stops `SceneRenderer` becoming a god-object whose
settings block tries to express every possible pipeline.

## The lifetime split (the core shape)

The whole design falls out of **separating state by how often it changes**. Baking
target, settings, or the graph into construction forces a full rebuild on exactly
the things that change most in the editor (panel resizes and UI toggles fire
constantly).

| Phase | Rate | Job |
|---|---|---|
| `Create(info)` | once | Allocate persistent resources; build the graph. Output **format** + initial extent + settings are inputs. |
| `Resize(extent)` | often (panel drag) | Recreate extent-sized transients via the deferred-destruction retire path; recompile the graph's resource allocation. |
| `Configure(settings)` | often (UI toggle) | Mark dirty; lazily recreate only the resources whose passes the changed setting affects, and recompile topology. |
| `Execute(cmd, view)` | every frame | Build/replay the graph; run each pass's record callback against this frame's `SceneView`. **Never** reallocates or recompiles. |
| `GetOutput()` | per consumer | Return the `Ref<ImageView>` of the owned result for compositing / `ImGui::Image`. |

```cpp
struct SceneRendererSettings { bool Shadows = true; bool AO = true; u32 ShadowRes = 2048; /* … */ };

struct SceneRendererInfo {
    Context&               Context;
    Renderer::Format       OutputFormat;   // a format, not a caller-owned target
    uvec2                  Extent;
    SceneRendererSettings  Settings;
};

// Single owner — nothing holds a Ref to a SceneRenderer → Unique, per docs/ownership.md.
static Unique<SceneRenderer> Create(const SceneRendererInfo&);

void           Resize(uvec2 extent);
void           Configure(const SceneRendererSettings&);
void           Execute(CommandBuffer& cmd, const SceneView& view);
Ref<ImageView> GetOutput() const;
```

`SceneView` is the per-frame input and is **not owned** by the renderer:

```cpp
struct SceneView {
    const Scene&  Scene;     // shared across N renderers in the editor
    const Camera& Camera;
    f32           Delta;
    // debug-view selector, frozen-cull toggle, etc.
};
```

### Output & target ownership

`SceneRenderer` takes an **output format and owns the render target**, rather than
accepting a caller-owned target. The editor wants the renderer to own its RT —
sized to the panel, recreated on resize through the retire path, handed back as a
sampleable view to drop into an `ImGui::Image`. Passing in a caller-owned target
re-introduces the resize-ownership tangle the lifetime split exists to avoid. The
game's swapchain path then composites `GetOutput()` exactly as an editor panel
samples it — **one handoff model, both consumers** (it is `ImGuiLayer`'s
single-offscreen-image → swapchain compositing, one level down).

## Compile vs. record (designing for the compiled graph)

`RenderGraph` is immediate-mode today (rebuilt per frame — `RenderGraph.h`), but
the direction is a **compiled** graph for performance. The `SceneRenderer` API
above is **forward-compatible with that migration by construction**, and that is
deliberate. Compilation splits the work into two phases:

- **Compile** (once, at `Create` / `Resize` / `Configure`): pass set + order, the
  derived barrier/layout schedule, transient allocation/aliasing, attachment
  descriptions. The expensive part you stop redoing per frame.
- **Record** (every `Execute`): the per-pass callbacks still run — *which objects
  you draw, camera matrices, push-constant indices, cull results* cannot be baked.
  The compiled graph replays a fixed barrier schedule and invokes the callbacks.

So a compiled graph caches **topology, not draw calls**, and the things that
invalidate it — topology (a pass on/off) and transient sizing (extent/format) —
are **exactly `Configure` and `Resize`**. Those two methods are the recompile
seams; `Execute` never recompiles. A bare `Execute(scene)` that rebuilt everything
inside would have *no seam to hang compilation off* — putting `Configure`/`Resize`
in now, while the graph is still immediate-mode, is what buys the compiled
migration cheaply later. The public shape does not change across the migration;
only where the graph lives internally does.

This rests on **two invariants** — write them down, because everything else
follows:

1. **Settings drive recompile; per-frame data never does.** A knob that changes
   topology is a `Settings` field (→ `Configure` → recompile). A value that varies
   per frame is a `SceneView` field (→ never recompile). The corollary for *empty*
   work: settings-driven topology is compile-time; **data-driven emptiness** ("no
   transparent objects this frame") is runtime — keep the pass compiled-in and let
   it record **zero draws** rather than recompiling the graph.
2. **Compile-time callbacks capture only stable state.** A record callback,
   registered at compile time, may close over only `this` + config — never
   frame-varying data. Everything per-frame arrives through the record-time context
   (next section). This keeps callbacks recompile-safe (they don't go stale when
   the graph rebuilds) and parallel-safe (no reach into shared mutable state).

## Feeding the passes: a typed `PassContext`

In a compiled graph, **transient aliasing settles how callbacks get their data.**
Once the graph pools/aliases transients, a pass's attachment is not a fixed
`ImageView` — the concrete resource it resolves to can differ frame to frame, and
at compile time does not exist yet. So the callback **cannot capture its attachment
views**; the graph must resolve allocation per frame and hand the concrete view to
the callback **at record time**. That record-time channel already exists by
necessity — so the per-frame `SceneView` rides it too, rather than being delivered
some second way:

```cpp
class PassContext {
public:
    CommandBuffer&    Cmd();
    const SceneView&  View() const;          // this frame's camera / scene / lights — read-only
    ImageView&        Resolved(ResourceId);  // concrete view for a declared transient, this frame
};

// Registered once at compile time. Captures only `this` + config (invariant 2).
.Execute([this](PassContext& ctx) {
    const auto& cam    = ctx.View().Camera;
    auto&       albedo = ctx.Resolved(m_GBufferAlbedo);
    /* bind, draw … */
});
```

Two house-style guardrails:

- **No stringly-typed blackboard.** The Granite/FrameGraph `typeid`/string-keyed
  `void*` blackboard is the usual implementation of this and clashes head-on with
  veng's identity (typed vocabulary enums, explicit structs, `Result<T>` over error
  strings). `PassContext` is a concrete typed struct; transients resolve by a typed
  `ResourceId`, not a name.
- **The alternative — stashing `m_CurrentView` on the renderer and having callbacks
  read it — is rejected, and the reason is threading.** Parallel pass recording
  (secondary command buffers across task-system workers) is most of why area 2
  exists; the stash idiom *encourages* per-frame scratch on the renderer, which is
  exactly what makes parallel recording unsafe. Pick the parallel-friendly shape
  now, while recording is still serial and the choice is free.

## Über pipeline, reusable passes

The pipeline is an **über-renderer**: many passes are interdependent (g-buffer →
lighting → AO reads depth+normals → bloom reads the HDR target → tonemap →
composite), so they are **wired in a fixed order**, not freely composed. But each
pass is a **reusable, self-contained unit** — so a material-preview renderer can
reuse the tonemap pass, a shadow-only path can reuse the shadow pass, and each pass
is testable in isolation.

The split that makes both true at once: **the renderer owns the wiring; each pass
owns itself.** The interdependence (lighting reads the g-buffer) lives in
`SceneRenderer`'s fixed wiring code, not inside the passes. A pass unit knows only
how to size its own transients, declare its reads/writes, and record its draws —
it does not know what feeds it.

```cpp
// Sketch — a reusable pass building block, distinct from RenderGraph::Pass.
class IRenderPass {
public:
    virtual void Configure(const SceneRendererSettings&) = 0;   // topology/sizing inputs
    virtual void Resize(uvec2) = 0;                             // recreate own transients
    virtual void Declare(RenderGraph& graph, const PassIO&) = 0; // contribute pass(es) + record cb
};
```

This **reconciles with the self-contained graph** (the resolved fork): each pass
unit contributes its `RenderGraph` pass(es) *into the renderer's single internal
graph* via `Declare`. "Reusable passes" is about *who authors the pass*, not *whose
graph it lands in* — they compose into one self-contained graph per `SceneRenderer`,
which keeps each renderer its own barrier domain (right for N independent editor
RTs). The contribute-into-a-caller's-shared-graph alternative buys cross-viewport
scheduling that veng's linear, no-reorder graph cannot exploit today; revisit only
if that changes.

## Frames-in-flight contract

Pin this **before** anything is compiled, because compilation is exactly where
transient allocation gets fixed and hand-waving stops being possible.

- **v1 (single-threaded):** a `SceneRenderer` instance is **single-in-flight** —
  one `Execute` resolves and completes before the next begins. Transients are
  **single-copy**, sized to the output. Within a frame, written-then-read transients
  (g-buffer, HDR target) are safe; the retire path covers *destruction* safety on
  resize/reconfigure, not reuse hazards.
- **When frames-in-flight > 1 (area 2):** the hazard is the **output** image — the
  compositor may sample frame N's result while the renderer writes frame N+1's. The
  result handed out by `GetOutput()` (and any transient read across a frame boundary)
  must then be **ring-buffered per frame-in-flight**, or execution fenced. This is a
  compile-time allocation decision for the compiled graph — make it there, once.

## Open forks & dependencies

- **Compiled [`RenderGraph`](compiled-rendergraph.md) is the enabling
  prerequisite** (area 9).
  `SceneRenderer` is designed to ship first against the immediate-mode graph and
  gain the compiled internals without an API change; the compiled graph can also
  land independently and `SceneRenderer` benefits for free.
- **Resolved — self-contained graph** (each renderer owns one graph) over
  contribute-to-a-shared-graph: simpler, correct for N independent offscreen RTs,
  and the shared-graph upside needs cross-pass reordering the linear graph lacks.
- **Resolved — über with reusable pass units:** fixed interdependent wiring owned by
  the renderer, passes authored as self-contained reusable blocks.
- **Depends on** the scene/entity model (area 7) for `Scene`/`Camera`, is consumed
  first and hardest by the [editor](editor.md) (area 6), and takes its
  frames-in-flight contract and parallel-record story from
  [threading](threading-task-system.md) (area 2).

## Status

Vision only. Becomes its own planset when taken up — most naturally alongside or
just after the compiled-`RenderGraph` work, and ahead of the editor's scene
viewport, which is its first real consumer.
