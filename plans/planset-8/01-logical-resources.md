# Plan 01 — Logical resources + `PassContext`

**Goal:** land the resource-model split and the record-time callback channel — the
load-bearing API shift — **while execution stays immediate-mode**, so the change is
verifiable on its own before the compile/replay lifecycle (plan 02) builds on it.
Accesses stop naming concrete `Ref<ImageView>`s and instead reference a vk-free
`ResourceId`; the pass callback stops taking a bare `CommandBuffer&` and takes a
`PassContext` that resolves a transient's concrete view per frame.

## Why this is its own plan

The resource model is the real API change; the `Compile()` lifecycle (plan 02) is a
mechanical consequence once resources are addressed by id and callbacks no longer
capture concrete views. Doing the model first, with the graph still rebuilding and
running each frame, isolates the surface change: the sample's pixels and the smoke
PPM must be identical before and after, because only *how resources are named*
changed, not *when barriers run*.

## Public surface — `engine/include/Veng/Renderer/RenderGraph.h`

A vk-free resource handle and a transient descriptor:

```cpp
// A handle into a render graph's resource table — either a graph-owned transient
// (allocated and resolved by the graph) or an imported external resource. Opaque;
// produced only by RenderGraph::CreateTransient / Import.
struct ResourceId
{
    u32 Index = InvalidIndex;
    static constexpr u32 InvalidIndex = ~0u;
    [[nodiscard]] bool IsValid() const { return Index != InvalidIndex; }
};

// A graph-owned transient: a logical image with no backing until the graph
// allocates it. Extent/format/usage are the allocation inputs; changing extent or
// format is a structural change that recompiles the graph (plan 02).
struct TransientDesc
{
    string Name;
    Format Format = Format::Undefined;
    uvec2  Extent = {};
    ImageUsage Usage;   // required, e.g. ImageUsage::ColorAttachment | ImageUsage::Sampled
};
```

The pass callback channel — a concrete typed struct, **not** a string/`void*`
blackboard:

```cpp
class PassContext
{
public:
    [[nodiscard]] CommandBuffer& Cmd() const;

    // The concrete view a declared transient resolves to this frame. Asserts if the
    // id is not a transient declared on this pass.
    [[nodiscard]] ImageView& Resolved(ResourceId id) const;

    // (The scene renderer later adds View() for its per-frame SceneView; that type
    // does not exist yet.)
private:
    // Backend-populated each frame; defined in the .cpp.
    friend class RenderGraph;
    // ...
};
```

`RenderGraph` gains the resource-registration calls and re-typed accesses:

```cpp
// Declare a graph-owned transient. Returns a handle usable in Color/Depth/Sample/…
[[nodiscard]] ResourceId CreateTransient(const TransientDesc& desc);

// Register an external concrete resource (swapchain image, app-owned target). Its
// concrete view is late-bound per frame via BindImport before Execute — the
// acquired swapchain view differs every frame. Returns a handle.
[[nodiscard]] ResourceId Import(string_view name);

// Supply an imported resource's concrete view for this frame. Called before Execute.
void BindImport(ResourceId id, const Ref<ImageView>& view);
```

`PassAttachment` and `Access` change their `Ref<ImageView> View` to `ResourceId
Resource`; the `PassBuilder` methods (`Color`/`Depth`/`Sample`/`StorageRead`/…) take
a `ResourceId` (attachments keep their `LoadOp`/`StoreOp`/`ClearValue`). The
`PassBuilder::Execute` overload takes `function<void(PassContext&)>`.

## Impl — `engine/src/Renderer/RenderGraph.cpp`

The graph owns a resource table: each entry is either a **transient** (a
`TransientDesc` plus the lazily-created `Ref<Image>`/`Ref<ImageView>` the graph
allocates) or an **import** (the late-bound `Ref<ImageView>` from `BindImport`).

`Execute(cmd)` keeps the current immediate-mode loop, with two changes:

- **Resolve before use.** Allocate any not-yet-backed transient (own allocation, no
  aliasing — that is plan 03) via `Image::Create` from its `TransientDesc`. Resolve
  each access's `ResourceId` to a concrete `ImageView` (transient → its allocated
  view; import → its bound view; assert an import has been bound). The barrier
  derivation (`ScopeFor` → `Backend::TransitionImage` reading tracked state) and the
  `BeginRendering` attachment build are **unchanged** — they now operate on the
  resolved view instead of `access.View`.
- **Build a `PassContext`** wrapping `cmd` and the pass's resolved-transient map;
  pass it to the callback instead of `cmd`.

Transients allocated here are owned by the graph for its lifetime and retire through
the per-frame deferred-destruction path like any other image; in immediate-mode this
plan they are allocated lazily and kept (re-resolved each frame), which already gives
the single-copy, persistent behaviour plan 02 formalizes.

## Sample migration — `examples/hello-triangle/main.cpp`

- The **depth buffer** becomes a **graph transient**: it is written-then-discarded
  entirely within the scene pass (`StoreOp::DontCare`), never read outside. Drop
  `m_DepthImage`/`m_DepthImageView` members; declare it with `CreateTransient` inside
  `RenderScene` and reference the returned id in `.Depth(...)`.
- The **scene image** stays **app-owned and imported**: it is sampled by ImGui and the
  composite pass (across graphs) and downloaded for the smoke capture, so the app must
  own it. Keep `m_SceneImage`/`m_SceneImageView`; register it with `Import` and
  `BindImport` it each frame.
- The **swapchain image** is imported and **late-bound per frame** —
  `BindImport(swapId, context.GetCurrentSwapChainImageView())` before the composite
  graph's `Execute`.
- Each pass lambda's signature changes to `(Renderer::PassContext& ctx)`; bodies use
  `ctx.Cmd()`. The depth attachment is referenced by its transient id (no resolved
  view is needed in the scene body, which only draws into the color/depth attachments
  the graph binds).

Per-frame data (MVP, viewport extent, push-constant indices) is still delivered by
the lambdas' existing `this`/`extent` captures — execution is per-frame here, so
nothing moves yet; that is plan 02's concern.

## Acceptance

- Clean build; `ctest` green; `include_hygiene` builds with the new vk-free public
  types (`ResourceId`/`TransientDesc`/`PassContext`).
- Smoke binary exits 0 and writes a correct-sized PPM (1280×720 RGB ≈ 2,764,816
  bytes); the rendered scene is unchanged (immediate-mode execution is preserved —
  only resource naming changed).
- `ctest --test-dir build-debug -L validation` green; the graph-allocated depth
  transient is validation-clean (allowlist stays empty).
