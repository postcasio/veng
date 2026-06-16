# Scene renderer — design overview

> **Mostly delivered — [planset-12](../planset-12/README.md).** The shell + the
> minimal-deferred spine are shipped; the remaining über-pipeline batteries, multiple
> & typed lights, frames-in-flight > 1, and parallel pass recording are the named
> next increments behind the same mechanism. This document records **what shipped**
> against **what is still future**, so the direction stays legible. It builds on the
> shipped renderer / bindless / material foundation (planset-5), the **compiled
> [`RenderGraph`](compiled-rendergraph.md)** (area 9, planset-8), and the runtime
> [scene/entity model](README.md#7-scene--entity-model) (area 7, planset-10); the
> [editor](editor.md) (area 6) is its prime future consumer, and the
> [threading/task system](threading-task-system.md) (area 2) carries the
> frames-in-flight and parallel-record increments.

## What it is — DELIVERED

`SceneRenderer` is a **long-lived, opinionated render pipeline**: constructed once
with an output format and a settings block, it owns the GPU resources its passes
need and an internal **compiled** graph, and renders a `Scene` from a camera into
an **offscreen target it owns**, handing back a sampleable result. It is **`Unique`,
single-owner** (nothing holds a `Ref` to one), per [docs/ownership.md](../../docs/ownership.md).
The game's main view and every editor preview panel are the same object wired two
ways — the editor renders **one `Scene` through N `SceneRenderer`s**, each with its
own camera, settings, and target ([editor.md](editor.md): each preview surface
renders into its own offscreen RT). planset-12 wires **one** (hello-triangle's main
view) and unit-tests **two** interleaved over one scene (the design-for-N proof); the
editor is the real N consumer.

It is **batteries-included, not an extension mechanism.** The über-pipeline (the
fixed wiring of interdependent passes) is the product; authoring a bespoke pass
graph still means dropping to `RenderGraph` directly (the hello-triangle composite
pattern, which the sample retains). Keeping that line sharp is what stops
`SceneRenderer` becoming a god-object whose settings block tries to express every
possible pipeline.

## The lifetime split (the core shape) — DELIVERED

The whole design falls out of **separating state by how often it changes**. Baking
target, settings, or the graph into construction forces a full rebuild on exactly
the things that change most in the editor (panel resizes and UI toggles fire
constantly).

| Phase | Rate | Job |
|---|---|---|
| `Create(info)` | once | Allocate persistent resources; build + compile the graph. Output **format** + initial extent + settings are inputs. |
| `Resize(extent)` | often (panel drag) | Recreate extent-sized images via the deferred-destruction retire path; re-register them into bindless; rebuild + re-`Compile()`. |
| `Configure(settings)` | often (UI toggle) | Recreate only affected resources; rebuild + re-`Compile()` the topology. |
| `Execute(cmd, view)` | every frame | Replay the compiled graph; run each pass's record callback against this frame's `SceneView`. **Never** reallocates or recompiles. |
| `GetOutput()` | per consumer | Return the `Ref<ImageView>` of the owned result for compositing / `ImGui::Image`. |

```cpp
struct SceneRendererSettings {
    DebugView Mode     = DebugView::Final;  // re-wires the pass set (Final/Albedo/Normal/Depth)
    f32       Exposure = 1.0f;              // the tonemap pass's exposure scale
};

struct SceneRendererInfo {
    Context&               Context;
    AssetManager&          Assets;        // the passes load their engine shaders through it
    Renderer::Format       OutputFormat;  // a format, not a caller-owned target
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
    const Scene&  World;     // borrowed; shared across N renderers in the editor
    const Camera& Camera;    // borrowed
    Light         Light;     // a per-frame VALUE the renderer selects from the scene
    f32           Delta;
};
```

The `Settings` block as shipped is the **minimal** one — `Mode` (the `DebugView`
selector, the live settings-drive-recompile proof) + `Exposure`. The batteries the
design sketch shows (`Shadows`, `AO`, `ShadowRes`, …) are still-future fields that
land with their passes.

### Output & target ownership — DELIVERED

`SceneRenderer` takes an **output format and owns the render target**, rather than
accepting a caller-owned target. The editor wants the renderer to own its RT —
sized to the panel, recreated on resize through the retire path, handed back as a
sampleable view to drop into an `ImGui::Image`. Passing in a caller-owned target
re-introduces the resize-ownership tangle the lifetime split exists to avoid. The
game's swapchain path then composites `GetOutput()` exactly as an editor panel
samples it — **one handoff model, both consumers** (it is `ImGuiLayer`'s
single-offscreen-image → swapchain compositing, one level down).

**`GetOutput()`'s `Ref<ImageView>` is invalidated by `Resize`/`Configure`** (the old
image retires; a new one is created). A consumer caching a bindless `TextureHandle`
or ImGui texture from it must re-fetch and re-register after those calls — a
contract, not an incidental. The sample never resizes its `SceneRenderer` (fixed
internal extent), so its one-time registration holds; the editor (the N consumer)
re-registers per resize.

## Compile vs. record (designing for the compiled graph) — DELIVERED

`RenderGraph` is **compiled** ([compiled-rendergraph.md](compiled-rendergraph.md),
planset-8): a builder whose `Compile()` returns a `CompiledGraph` that replays per
frame. The `SceneRenderer` API is **built around that lifecycle**. Compilation
splits the work into two phases:

- **Compile** (once, at `Create` / `Resize` / `Configure`): pass set + order, the
  derived barrier/layout schedule, transient allocation/aliasing, attachment
  descriptions. The expensive part you stop redoing per frame.
- **Record** (every `Execute`): the per-pass callbacks still run — *which objects
  you draw, camera matrices, push-constant indices, cull results* cannot be baked.
  The compiled graph replays a fixed barrier schedule and invokes the callbacks.

So a compiled graph caches **topology, not draw calls**, and the things that
invalidate it — topology (a pass on/off) and sizing (extent/format) — are **exactly
`Configure` and `Resize`**. Those two methods are the recompile seams; `Execute`
never recompiles.

This rests on **two invariants**:

1. **Settings drive recompile; per-frame data never does.** A knob that changes
   topology is a `Settings` field (→ `Configure` → recompile). A value that varies
   per frame is a `SceneView` field (→ never recompile). The corollary for *empty*
   work: settings-driven topology is compile-time; **data-driven emptiness** ("no
   transparent objects this frame") is runtime — keep the pass compiled-in and let
   it record **zero draws** rather than recompiling the graph.
2. **Compile-time callbacks capture only stable state.** A record callback,
   registered at compile time, closes over only `this` + config — never frame-varying
   data. Everything per-frame arrives through the record-time context (next section).
   This keeps callbacks recompile-safe (they don't go stale when the graph rebuilds)
   and parallel-safe (no reach into shared mutable state).

## Feeding the passes: the record-time channel — DELIVERED (reconciled with the sketch)

In a compiled graph, **transient aliasing settles how callbacks get their data.**
A pass's attachment is not a fixed `ImageView` — the concrete resource it resolves
to can differ frame to frame, and at compile time does not exist yet. So the
callback **cannot capture its attachment views**; the graph resolves allocation per
frame and hands the concrete view to the callback **at record time**. That
record-time channel already exists by necessity — so the per-frame `SceneView` rides
it too, rather than being delivered some second way.

**The design sketch put `View()` directly on `RenderGraph::PassContext`. What shipped
keeps `RenderGraph` scene-agnostic instead:** the channel is an **opaque per-`Execute`
user pointer** on `PassContext` (forwarded through `CompiledGraph::Execute(cmd,
imports, void* userData)`), which `SceneRenderer` sets to `&view`. The `Scene`-aware
typing lives one layer up, in a `ScenePassContext` wrapper a pass reads back through:

```cpp
class ScenePassContext {
public:
    CommandBuffer&    Cmd() const;
    const SceneView&  View() const;          // asserts the user pointer is non-null, then reinterprets
    ImageView&        Resolved(ResourceId) const;
};
```

This is what keeps `RenderGraph`/`PassContext` free of any `Scene`-layer dependency
(`include_hygiene` enforces it). A `void*` is less type-safe than a `View()` baked
into `PassContext`, so the "always set, always the right type" invariant is enforced
loud at the wrapper (`ScenePassContext::View()` asserts non-null before the
reinterpret), rather than left as UB.

Two house-style guardrails held:

- **No stringly-typed blackboard.** The Granite/FrameGraph `typeid`/string-keyed
  `void*` blackboard clashes with veng's identity (typed vocabulary enums, explicit
  structs). `PassContext` is a concrete typed struct; transients resolve by a typed
  `ResourceId`, not a name. The one `void*` is the per-`Execute` user pointer, typed
  at the `ScenePassContext` boundary.
- **The alternative — stashing `m_CurrentView` on the renderer and having callbacks
  read it — is rejected, and the reason is threading.** The stash idiom *encourages*
  per-frame scratch on the renderer, which is exactly what makes parallel pass
  recording unsafe. The user pointer is per-call data (no renderer scratch) — the
  parallel-friendly shape, picked while recording is still serial and the choice is
  free.

## Über pipeline, reusable passes — DELIVERED

The pipeline is an **über-renderer**: many passes are interdependent (g-buffer →
lighting → tonemap; later AO reads depth+normals, bloom reads the HDR target), so
they are **wired in a fixed order**, not freely composed. But each pass is a
**reusable, self-contained unit** — so a material-preview renderer can reuse the
tonemap pass, and each pass is testable in isolation.

The split that makes both true at once: **the renderer owns the wiring; each pass
owns itself.** The interdependence (lighting reads the g-buffer) lives in
`SceneRenderer`'s fixed wiring code, not inside the passes. A pass unit knows only
how to size its own resources, declare its reads/writes, and record its draws — it
does not know what feeds it.

**The design sketch's `IRenderPass` shipped as `ScenePass`** — the `I` interface
prefix is a kind-tag forbidden by veng's naming rule (CLAUDE.md), and the name is
distinct from `RenderGraph::Pass` deliberately: a `ScenePass` *contributes*
`RenderGraph` passes, it is not one.

```cpp
class ScenePass {
public:
    virtual void Configure(const SceneRendererSettings&) {}   // topology/sizing inputs
    virtual void Resize(uvec2) {}                             // recreate own resources
    virtual void Declare(RenderGraph& graph, const PassIO&) = 0; // contribute pass(es) + record cb
};
```

`PassIO` is the wiring contract, and it shipped as a **named-slot struct, not a flat
in/out pair**: the renderer hands each pass the specific resources it wired in (the
g-buffer ids + their bindless `TextureHandle`s it reads, the HDR/output ids it
writes). Resources flow **both directions**, and a `ScenePass` may declare
**multiple** `RenderGraph` passes — so a future shadow pass = N settings-driven
sub-passes + a produced shadow-map id; SSAO = a produced AO id the lighting pass
consumes.

Each pass unit contributes its `RenderGraph` pass(es) **into the renderer's single
internal graph** via `Declare`. "Reusable passes" is about *who authors the pass*,
not *whose graph it lands in* — they compose into **one self-contained graph per
`SceneRenderer`**, which keeps each renderer its own barrier domain (right for N
independent editor RTs). The contribute-into-a-caller's-shared-graph alternative
buys cross-viewport scheduling that veng's linear, no-reorder graph cannot exploit;
revisit only if that changes.

### Renderer-owned imported images (not graph transients) — DELIVERED

The g-buffer (albedo, world-normal), the depth target, the HDR target, and the
output are **renderer-owned `Image`/`ImageView`s**, allocated by `SceneRenderer` and
**`Import`ed** into the internal graph — *not* graph-owned transients. This is forced
by the sampling path: a fullscreen pass samples an upstream target through the
**bindless** set-0 array, which needs a `Ref<ImageView>` to `Register` — a graph-owned
transient exposes only a per-frame `ImageView&` (`Resolved`), no `Ref`, so it cannot
be registered. The renderer-owned images are registered into bindless **once** at
`Create` (re-registered on `Resize`), and the pass that samples one gets its
`TextureHandle` through `PassIO`. This is exactly the sample's existing cross-graph
sampling pattern (own an image, register it once, import it).

## The deferred chain & the material contract — DELIVERED (the v1 spine)

The v1 pipeline is a **minimal deferred chain**: a g-buffer geometry pass (an opaque
material's fragment shader writes albedo + world-normal into an MRT g-buffer; depth
is the depth attachment), a fullscreen **deferred lighting pass** applying the
scene's directional light into an HDR target, and a **tonemap** pass (ACES-approx +
`Exposure`) mapping HDR → the output format.

**The deferred material contract.** Going deferred changes what an opaque material's
fragment shader outputs — from final color to **g-buffer channels**, written through
a single engine-provided `GBufferOutput { float4 Albedo : SV_Target0; float4 Normal
: SV_Target1; }` struct. Albedo is sRGB-encoded (sampled back as linear); the normal
is world-space in a signed float format. The layout (channels, formats, usage) is
fixed in `Renderer/GBuffer.h` and agreed on by the geometry pass's `RenderingInfo`
and every material pipeline. Two forward-looking constraints are baked in so later
batteries do not force a silent breaking change:

- **The v1 g-buffer is the minimum set, designed to grow.** PBR shading needs
  roughness/metallic/AO — a future **G2** target every material then also writes, by
  extending the one `GBufferOutput` struct (one place, not a per-material edit).
- **The deferred material contract is not the forward/transparent contract.** A
  future transparent/forward pass needs materials whose fragment shader outputs
  *final color* — a **second** material entry, not a breaking change to this one. v1
  picks the deferred output as the *opaque* material contract, not the universal one.

Set-0 bindless, `MaterialData`, and texture handles are unchanged — only the
fragment shader's outputs move to the g-buffer.

A directional **`Light`** builtin (`vec3 Direction; vec3 Color; f32 Intensity;`)
joins `Name`/`Transform`/`Parent`/`CameraComponent`/`MeshRenderer` in `BuiltinTypes`,
pre-registered by `RegisterBuiltinTypes` (GPU-free). `SceneRenderer::Execute` selects
the scene's directional light into the `SceneView` **by value** — the first `Light`
entity, or a zero-intensity default plus the lighting pass's small ambient term, so a
scene with no light renders flat-ambient (never pure black, never asserts).

**Known cost (stated, not hidden):** the primary platform is macOS/MoltenVK, a
tile-based deferred GPU. The deferred chain stores a full-screen MRT g-buffer and
runs additional fullscreen passes, each a store-then-sample of a full-res target —
strictly more bandwidth than the forward draw it replaces for a one-cube scene. v1
trades that bandwidth for the architecture; on-tile/subpass-fused deferred (the
Apple-friendly form) is a future optimization behind the same `ScenePass` mechanism.

## Frames-in-flight contract — DELIVERED (v1 single-in-flight)

- **v1 (single render thread):** a `SceneRenderer` instance is **single-in-flight**.
  The renderer-owned images (g-buffer, depth, HDR, output) are **single-copy**, sized
  to the output. One `Execute` resolves and completes before the next begins; within
  a frame, written-then-read images (g-buffer, HDR) are correctly ordered by the
  graph's derived barriers, and the retire path covers *destruction* safety on
  resize/reconfigure. The cross-frame ring-buffer hazard does not arise: the output
  is **consumed in the frame it is written** — a compositor samples `GetOutput()` for
  the same frame the renderer wrote it. The two-renderer interleaved test pins
  instance independence (each its own barrier domain).
- **Still future — frames-in-flight > 1 (area 2):** the hazard is the **output**
  image (a compositor sampling frame N's result while the renderer writes N+1's) and
  any image read across an `Execute` boundary. The fix — ring-buffer those per
  frame-in-flight, or fence execution — is a **compile-time allocation decision** for
  the compiled graph, recorded here as the named next increment. It has no consumer
  while the render thread is single and nothing drives FIF > 1 yet.

## Still future — the named next increments

Each is its own later increment behind the **same** `ScenePass` + `Configure`-recompile
mechanism this delivers:

- **The rest of the über-pipeline's batteries:** shadows, SSAO, bloom, MSAA, a
  transparent/forward pass (a second material contract), a post stack, and the **G2
  PBR g-buffer target** that extends the `GBufferOutput` struct.
- **Multiple & typed lights:** point/spot/area lights, multiple lights, and
  clustered/tiled light culling. v1's lighting pass reads **one** directional light
  from the `SceneView`.
- **Frames-in-flight > 1** with ring-buffered output (above).
- **Parallel pass recording** into secondary command buffers (area 2's seam): the
  `SceneView`-rides-the-record-context choice is made *for* it, but no parallel
  recording is built.
- **On-tile / subpass-fused deferred** (the MoltenVK-friendly form) as a bandwidth
  optimization behind the same wiring.

## Dependencies

- **Compiled [`RenderGraph`](compiled-rendergraph.md)** (area 9, planset-8) — the
  enabling rendering prerequisite; its `PassContext::Resolved` / `Compile` /
  structural-recompile seams are what the `SceneRenderer` API is built around.
- **Scene/entity model** (area 7, planset-10) for `Scene`/`Camera`.
- Consumed first and hardest by the [editor](editor.md) (area 6 — its viewport panels
  each drive one `SceneRenderer`); takes its frames-in-flight and parallel-record
  increments from [threading](threading-task-system.md) (area 2).

## Status

**Delivered — [planset-12](../planset-12/README.md):** the shell + lifetime split,
the owned imported target, `SceneView`, the `ScenePass` + `ScenePassContext`
reusable-unit framework, the self-contained-graph-per-renderer resolution, the
minimal deferred **g-buffer → directional-light → tonemap** chain, and the directional
`Light`. **Still future:** the remaining batteries, multiple & typed lights,
frames-in-flight > 1 with ring-buffered output, and parallel pass recording — each
named above as a next increment behind the same mechanism.
