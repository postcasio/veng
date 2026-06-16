# Plan 01 — the `SceneRenderer` shell: lifetime split + the `ScenePass` framework

**Goal:** land the `SceneRenderer` **architecture** with the current forward draw as
its single pass unit, and migrate hello-triangle's main view onto it — **changing no
pixels**. This plan delivers the lifetime split (`Create`/`Resize`/`Configure`/
`Execute`/`GetOutput`), the reusable-pass-unit abstraction (`ScenePass` + the
named-slot `PassIO` + `ScenePassContext`), and the one small `RenderGraph` change the
record-time channel needs. The deferred pipeline (plans 02–04) is built *inside* this
shell afterward; landing the shell against the **existing** render output first
de-risks the architecture from the pipeline change.

This plan is the largest of the set and splits into **two independently committable
parts**: **01a** lands the `RenderGraph` change alone (a one-line API addition, all
existing tests green); **01b** lands the `SceneRenderer`/`ScenePass` shell and the
sample migration on top of it. Run them as two commits — one session if it flows, two
if 01b runs long. 01a verifies on its own, so the bigger shell work never blocks on a
half-built graph change.

## Why this is its own plan, and on the main thread

It fixes three contracts everything downstream depends on: the **lifetime split**
(which method recompiles, decision 2), the **`ScenePass`/`PassIO` boundary** (renderer
owns wiring, pass owns itself, decision 4), and the **record-time channel** for the
`SceneView` (the opaque user pointer, decision 3). Getting these right against the
known-good forward draw — where `smoke_golden` is a byte-exact oracle — means plans
02–04 change only the *pass set*, never the shell.

---

<a id="01a"></a>
## 01a — the `RenderGraph` record-time user pointer (decision 3)

`PassContext` today carries `Cmd()` + `Resolved(id)`. Add an **opaque per-`Execute`
user pointer** so a caller can thread per-frame data into pass callbacks without
`RenderGraph` knowing its type:

```cpp
class PassContext
{
public:
    CommandBuffer& Cmd() const;
    ImageView&     Resolved(ResourceId) const;
    void*          UserData() const { return m_UserData; }   // ← new; null unless Execute sets it
    // …
};
```

`CompiledGraph::Execute` forwards it:

```cpp
void Execute(CommandBuffer& cmd,
             std::span<const RenderGraph::ImportBinding> imports = {},
             void* userData = nullptr);                       // ← new trailing arg, defaulted
```

- **No new include**, no `Scene` dependency — it is a bare `void*` set per call and
  read back by whoever owns the type. `include_hygiene` is unaffected.
- Existing callers (hello-triangle's composite, the graph tests) pass nothing and are
  unchanged by the default argument.
- This is the **only** `RenderGraph` change in planset-12. Notably it does **not** add
  a `Ref<ImageView>` accessor for transients — the deferred chain samples
  renderer-owned imported images instead (01b / decision 6), so that accessor is never
  needed.

**01a acceptance:** clean build; the existing graph tests and `smoke_golden` stay
green with the defaulted argument; nothing else changes. Commit: `Plan 01a:
RenderGraph record-time user pointer on PassContext/CompiledGraph::Execute`.

---

<a id="01b"></a>
## 01b — the `SceneRenderer` shell + the sample migration

### The public surface — `engine/include/Veng/Renderer/SceneRenderer.h`

```cpp
namespace Veng::Renderer
{
    // Topology/sizing knobs. A change here is a Configure → recompile (decision 2).
    struct SceneRendererSettings
    {
        // Plan 01 ships the struct (empty/near-empty); plan 04 adds DebugView etc.
    };

    struct SceneRendererInfo
    {
        Context&              Context;
        Format                OutputFormat;   // a format, not a caller-owned target
        uvec2                 Extent;
        SceneRendererSettings Settings;
    };

    // Per-frame input. NOT owned by the renderer; shared across N renderers.
    // World/Camera are borrowed; Light is a per-frame VALUE the renderer computes
    // (plan 03 adds it). Fields are named for their role, not their type.
    struct SceneView
    {
        const Scene&  World;
        const Camera& Camera;
        f32           Delta = 0.0f;
    };

    class SceneRenderer
    {
    public:
        static Unique<SceneRenderer> Create(const SceneRendererInfo&);
        ~SceneRenderer();

        void           Resize(uvec2 extent);
        void           Configure(const SceneRendererSettings&);
        void           Execute(CommandBuffer& cmd, const SceneView& view);
        Ref<ImageView> GetOutput() const;

    private:
        // factory-only; backend/pass state hidden in the .cpp
    };
}
```

- **`Unique`, single-owner** (decision 1). No public constructor; `Create` is the
  factory, consistent with the resource-ownership rule.
- **Owns its output image, imported into the graph.** `Create` allocates an `Image`
  (`OutputFormat`, `Extent`, `ColorAttachment | Sampled | TransferSrc` — `Sampled` so
  a consumer/composite samples it, `TransferSrc` so the smoke path can `Download()`
  it) + its `ImageView`, registers nothing of its own yet (the forward pass writes the
  output as a color attachment; downstream consumers register `GetOutput()` on their
  side), and **`Import`s** the output into the internal graph. `GetOutput()` returns
  the view. `Resize` recreates the image/view through the deferred-destruction retire
  path (drop the old `Ref`s, `Create` new), rebinds the import, and rebuilds +
  recompiles the internal graph.
- **The pixels-unchanged invariant:** the output image is created with `OutputFormat =
  Context::GetOutputFormat()` — exactly today's `m_SceneFormat` — and the forward pass
  uses today's clear color and load/store ops (below). That equality is what makes
  `smoke_golden` byte-identical; it is an acceptance precondition, not an incidental.
  No HDR intermediate exists in plan 01 — it arrives in plan 03.
- **`GetOutput()`'s `Ref` is invalidated by `Resize`/`Configure`** (decision 1): the
  old view retires, a new one is created. A consumer caching a bindless `TextureHandle`
  or ImGui texture from it must re-register after those calls. The sample never resizes
  its renderer (fixed internal extent), so its one-time registration stays valid.
- **Holds the internal graph** as `Unique<CompiledGraph>`, rebuilt + re-`Compile()`d
  on `Create`/`Resize`/`Configure`, replayed on `Execute`.

### The pass-unit abstraction — `engine/include/Veng/Renderer/ScenePass.h`

```cpp
namespace Veng::Renderer
{
    // The record-time context a ScenePass callback receives: the RenderGraph
    // channel plus this frame's SceneView, typed back from PassContext::UserData().
    // SceneRenderer is the Scene-aware layer that owns this wrapper (decision 3).
    class ScenePassContext
    {
    public:
        CommandBuffer&   Cmd() const;
        const SceneView& View() const;   // asserts UserData() is non-null, then reinterprets
        ImageView&       Resolved(ResourceId id) const;
    private:
        friend class SceneRenderer;
        // wraps a RenderGraph::PassContext& + reinterprets UserData() as const SceneView*
    };

    // Inputs a ScenePass needs from the renderer's wiring — a NAMED-SLOT struct, not
    // a flat in/out pair. The renderer fills the slots a given pass reads/writes;
    // resources flow both directions (a pass may produce one a later pass consumes),
    // and a pass may declare multiple RenderGraph passes (decision 4). Plan 01's
    // forward case is degenerate: one output id.
    struct PassIO
    {
        // Plan 01 (forward): the imported output id the forward pass writes.
        ResourceId Output;
        // Plans 02-04 add named slots: g-buffer ids + their bindless TextureHandles,
        // the HDR id + handle, the depth id + handle — filled by the renderer's wiring.
    };

    // A reusable, self-contained pipeline stage. Knows how to size its own
    // resources, declare its reads/writes, and record — never what feeds it.
    // Renamed from the design's IRenderPass: the I- interface prefix is a
    // forbidden kind-tag (CLAUDE.md). Distinct from RenderGraph::Pass: a ScenePass
    // contributes RenderGraph passes, it is not one.
    class ScenePass
    {
    public:
        virtual ~ScenePass() = default;
        virtual void Configure(const SceneRendererSettings&) {}
        virtual void Resize(uvec2) {}
        virtual void Declare(RenderGraph& graph, const PassIO& io) = 0;
    };
}
```

`SceneRenderer` owns a `vector<Unique<ScenePass>>` in fixed wiring order. Its
internal rebuild walks them: `Configure`/`Resize` each, then `Declare` each into a
fresh `RenderGraph`, then `Compile()`. `Execute` sets `userData = &view` and replays.
The `PassIO` shape is frozen here (named-slot) so plans 02–04 add slots without
changing `Declare`'s signature; the wiring it carries is *first exercised for real* in
plan 02 (g-buffer ids threaded into the lighting pass).

### The one forward pass unit — `ForwardScenePass` (engine `.cpp`, internal)

Move hello-triangle's current `BuildSceneGraph` draw verbatim into a `ScenePass`:

- **`Declare`:** create the depth target as a **graph transient** (plan 01's depth is
  write-only — cleared, never sampled — so a transient is correct here; plan 02
  promotes it to a renderer-owned imported image once the lighting pass samples it);
  `AddPass("Scene")` with the imported output (`io.Output`) as `.Color(Clear → Store)`
  using **today's clear color** (`{0.05, 0.05, 0.08, 1.0}`) and depth as
  `.Depth(Clear → DontCare)` — the **same load/store ops as today's pass**, so the
  barriers and pixels are identical. The record callback iterates
  `ctx.View().World.Each<Transform, MeshRenderer>(…)`, binds the bindless registry, and
  draws each submesh with its material — **identical** draw logic to today's sample,
  only sourced from `ScenePassContext` instead of captured `this`/`m_Camera`/`m_Scene`.
- The camera matrices come from `ctx.View().Camera.ViewProjection()`; the per-entity
  world matrix from `WorldMatrix(ctx.View().World, entity)` — no renderer scratch.

The output image is the renderer's owned, **`Import`ed** image; the forward pass's
`.Color` targets it, bound per `Execute` to the owned `ImageView`. (In plans 02–04 the
chain grows g-buffer/HDR images upstream and the **tonemap** pass writes the imported
output; plan 01's forward pass writing the output directly is the degenerate one-pass
case of that.)

### Sample migration — `examples/hello-triangle/main.cpp`

- **Drop:** `m_SceneImage`/`m_SceneImageView` ownership, `m_SceneFormat`/`m_DepthFormat`
  wiring for the scene target, `BuildSceneGraph`/`RenderScene`/`m_SceneGraph`,
  `m_SceneId`, and the scene-depth transient — all now inside `SceneRenderer`.
- **Add:** a `Unique<Renderer::SceneRenderer> m_SceneRenderer = Create({ .Context =
  context, .OutputFormat = context.GetOutputFormat(), .Extent =
  context.GetInternalRenderExtent(), .Settings = {} })` in `OnInitialize`.
- **`OnRender`:** build the `SceneView{ *m_Scene, m_Camera, delta }` and call
  `m_SceneRenderer->Execute(cmd, view)`; then the composite samples
  `m_SceneRenderer->GetOutput()` exactly where it sampled `m_SceneImageView` before
  (the `ImGui::Image` preview + the fullscreen composite both retarget to
  `GetOutput()`). The composite graph, ImGui, and `PrepareForAccess(GetOutput(),
  Sample)` are unchanged in shape. Because the sample never resizes its renderer, the
  one-time `bindless.Register(GetOutput())` + ImGui-texture creation against the output
  view stay valid for the run (decision 1's re-registration caveat applies only to a
  resizing consumer — the editor).
- **`WriteSceneCapture`/smoke:** `Download()` from `GetOutput()`'s image
  (`TransferSrc`) instead of `m_SceneImage`.
- **`OnDispose`:** `m_SceneRenderer.reset()` before the context tears down, like every
  other engine resource.
- The composite pipeline, bindless texture registration for the scene texture, and the
  `CompositePushConstants` path stay app-side — the composite is the **drop-to-
  `RenderGraph`** path the über-pipeline deliberately does not absorb (README decision
  on batteries-not-extensible).

### Tests

- **`include_hygiene`:** add `SceneRenderer.h`, `ScenePass.h` (and the `SceneView`
  header if split out). They compile against PUBLIC deps only — no backend leak.
- **GPU (`veng_gpu`):** a fixture creates a `SceneRenderer`, `Execute`s one frame over
  a minimal scene (a primitive cube + camera), and asserts `GetOutput()` is a valid
  sampleable view of the requested extent/format; then `Resize` to a new extent,
  **composites/samples the new `GetOutput()`**, and asserts the output view updates and
  is still sampleable (proving the re-fetch-after-resize contract, not just that a Ref
  changed). Skips with no ICD.
- **`smoke_golden`:** **unchanged golden.** The migrated sample renders the same
  forward draw at the fixed `SmokeAngle`; a green `smoke_golden` against the existing
  `hello_triangle_scene.png` is this plan's core proof that the shell changed nothing.
- **Validation gate:** `ctest --test-dir build-debug -L validation` stays green; the
  allowlist stays empty (same barriers — the forward pass's declarations are the old
  graph's).

### 01b acceptance

Clean build; `ctest` green; `include_hygiene` compiles the new headers;
`smoke_golden` passes against the **unchanged** golden (output image created at
`GetOutputFormat()`); the smoke binary writes a correct-sized 1280×720 PPM and exits
0; the validation gate is green with an empty allowlist. Commit: `Plan 01b:
SceneRenderer shell — lifetime split, ScenePass/PassIO framework, record-time channel;
migrate hello-triangle (pixels unchanged)`.
