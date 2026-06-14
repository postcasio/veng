# Plan 02 — `Compile()` → replay lifecycle

**Goal:** replace rebuild-every-frame with **compile once, replay per frame.**
`RenderGraph` becomes a pure **builder**; `RenderGraph::Compile()` returns a
`CompiledGraph` that has computed its barrier/transition schedule, transient
allocation, per-graphics-pass `RenderingInfo` skeleton, and validation **once**.
`CompiledGraph::Execute(cmd, imports)` replays that schedule each frame and runs only
the pass callbacks. The immediate-mode `RenderGraph::Execute` is removed.

## Why this is its own plan

Plan 01 made resources addressable by id and stopped callbacks capturing concrete
views — the two prerequisites for caching a schedule. This plan is the lifecycle flip:
the work that stops being redone per frame (barrier derivation, transient sizing,
validation) moves into `Compile()`, and replay becomes a separate object. It is the
heart of the planset and the seam the scene renderer's `Create`/`Resize`/`Configure`
hang off.

## The two types — `engine/include/Veng/Renderer/RenderGraph.h`

`RenderGraph` keeps the authoring surface (`AddPass`/`CreateTransient`/`Import`/the
`PassBuilder`) and gains one terminal call; its per-frame `Execute` from plan 01 is
removed:

```cpp
// Compile the declared passes into a replayable graph: derive the barrier schedule,
// allocate transients, build per-graphics-pass RenderingInfo, validate once. Single
// owner — nothing holds a Ref to a CompiledGraph → Unique, per docs/ownership.md.
[[nodiscard]] Unique<CompiledGraph> Compile();
```

```cpp
// A compiled, replayable render graph. Built by RenderGraph::Compile(); replayed each
// frame. Owns its transient images; retires them through the per-frame deferred-
// destruction path on destruction.
class CompiledGraph
{
public:
    ~CompiledGraph();

    // Replay the baked schedule: resolve transients, bind the supplied imports,
    // emit the scheduled transitions, drive rendering, run each pass callback. Every
    // declared import must appear in `imports`; a graph with no imports takes {}.
    void Execute(CommandBuffer& cmd, std::span<const RenderGraph::ImportBinding> imports = {});

private:
    friend class RenderGraph;
    struct Native;            // the vk:: schedule + allocated transient images
    Unique<Native> m_Native;  // defined in the .cpp — keeps RenderGraph.h backend-free
};
```

The `vk::` schedule (`Backend::SubresourceState`: layout/stage/access) lives in
`CompiledGraph::Native`, defined in `RenderGraph.cpp` — the same public/backend split
every resource uses, so `RenderGraph.h` stays backend-free and `include_hygiene` stays
green.

`Native` holds, per pass, an ordered list of **transitions** — each a resolved
destination `SubresourceState` (from `ScopeFor`) plus the subresource range and the
resource-table slot it targets — and, for graphics passes, the `RenderingInfo`
skeleton (attachment load/store/clear, layer count, view mask, the extent-from-base-mip
rule). Plus the graph-allocated transient images.

## Lifecycle — `engine/src/Renderer/RenderGraph.cpp`

- **`RenderGraph::Compile()`**: allocate each transient (own allocation; aliasing is
  plan 03) via `Image::Create` from its `TransientDesc`; for every pass, derive the
  per-access destination scope via `ScopeFor` and record it as a baked transition keyed
  by resource slot + subresource range; build each graphics pass's `RenderingInfo`
  skeleton; run **one-time validation** (below). Move it all into a fresh
  `CompiledGraph`.
- **`CompiledGraph::Execute(cmd, imports)`** (every frame, never recompiles): for each
  pass, replay its baked transitions — resolve each slot to a concrete `Image`
  (transient → allocated; import → the view supplied for its id in `imports`) and call
  the existing tracked-state `Backend::TransitionImage(cmd, image, dstLayout, dstStage,
  dstAccess, range)`, which still reads each image's tracked state and emits a barrier
  **iff** one is needed. (**Tracked-state source resolution stays at replay**: the
  swapchain import is a different image each frame and transfer-produced imports carry
  a runtime queue-ownership-acquire. The schedule bakes the *destination* scope and the
  *structure*; the *source* comes from live tracked state.) Then build the resolved
  `RenderingInfo` from the skeleton + resolved attachment views, `BeginRendering`, run
  the callback with a `PassContext`, `EndRendering`.

## Invalidation — the consumer re-compiles

There is **no internal dirty flag or structural hash.** A `CompiledGraph` is immutable;
when the structure changes — a pass added/removed (topology) or a transient's
extent/format changed (allocation) — the consumer rebuilds the `RenderGraph` and calls
`Compile()` again, replacing its `Unique<CompiledGraph>`. The consumer is the one that
knows when it changed structure, so detecting change inside the graph would be
redundant. (For the future scene renderer these triggers are exactly `Configure` and
`Resize`; this plan builds the seam those will hang off.)

**Per-frame data never recompiles.** **Data-driven emptiness is not a recompile:** a
pass with nothing to draw this frame stays compiled-in and records **zero draws** in
its callback.

## One-time validation (moved out of the per-frame path)

`Compile()` is where validation runs once instead of being implicitly re-walked each
frame:

- **read-before-write** — a transient read by a pass before any pass writes it,
- **format/usage** — a transient used as an attachment without the matching
  `ImageUsage`, or sampled without `Sampled`,
- **attachment-extent agreement** — color/depth attachments of one graphics pass must
  share an extent (the current code silently takes the first; make the mismatch a
  `VE_ASSERT`).

Cycles are impossible in the linear model; dead-pass culling is not built (imports
already pin outputs — note the seam).

## Sample migration — `examples/hello-triangle/main.cpp`

The two graphs are now **compiled once and held across frames**:

- Build the scene graph and the composite graph once in `OnInitialize`, `Compile()`
  each, and hold the results as members (`m_SceneGraph`, `m_CompositeGraph`, each a
  `Unique<Renderer::CompiledGraph>`). `OnRender` calls `Execute` on them, not rebuild.
- **Per-frame data leaves the closures.** A compiled callback is registered once, so it
  may not close over frame-varying *values*. The scene callback captures only `this`
  and reads per-frame members the app updates in `OnUpdate` (`m_Angle` → the MVP; the
  viewport extent from the imported scene image). The swapchain view and scene-image
  view arrive as `Execute` import bindings, not captures. No callback captures a
  per-frame value.
- A window/swapchain **resize** recreates the app-owned scene image and the depth
  transient's extent, so the sample **rebuilds + re-`Compile()`s** both graphs in its
  existing swapchain-invalidation callback (`AddSwapChainInvalidationCallback`).
  hello-triangle's headless smoke path has a fixed extent, so no recompile fires there.

## Acceptance

- Clean build; `ctest` green; `include_hygiene` green (the `vk::` schedule stays behind
  `CompiledGraph::Native`).
- Smoke binary exits 0, writes a correct-sized PPM; the scene renders identically
  (compile/replay is behaviour-preserving for a static topology).
- `Compile()` runs once for the fixed-topology smoke path, not per frame — confirmed by
  a one-time debug log or a test hook.
- `ctest --test-dir build-debug -L validation` green; allowlist unchanged.
