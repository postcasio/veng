# 04 — Deferred destruction queue

**Goal:** decouple C++ lifetime from GPU lifetime. Resource destructors retire
their Vulkan handles into a per-frame queue drained after the frame's fence is
waited; dropping the last `Ref`/`Unique` mid-frame becomes always safe. Delete
the manual `SubmitResource` path and both `Ref<void>` keep-alive lists.

**Dependencies:** 02 (teardown must exist, since final drain happens there).
Plan 05 builds on this.

## Current state — three overlapping mechanisms

1. `Create()` factories split `Unique` (Shader, Fence, Semaphore, pools,
   VertexBufferLayout) vs `Ref` (Buffer, Image, ImageView, pipelines,
   DescriptorSet) with no stated rule.
2. `CommandBuffer::m_BoundResources`
   (`include/Veng/Renderer/Backend/CommandBuffer.h:116`) — every
   `Bind*`/`BeginRendering` pushes `Ref<void>`, cleared on `Reset()`.
3. `SynchronizationFrame::SubmitResource`
   (`include/Veng/Renderer/Backend/SynchronizationFrame.h:20-29`) and
   `Command::SubmitResource(s)`
   (`include/Veng/Renderer/Backend/Command.h:14-31`) — manual keep-alive the
   consumer must remember, cleared at `BeginFrame`.

## Design

### Retire queue in `Context`

```cpp
// Context private
struct RetireBin {
    vector<std::pair<vk::Buffer, VmaAllocation>> Buffers;
    vector<std::pair<vk::Image,  VmaAllocation>> Images;   // managed images only
    vector<vk::ImageView>      ImageViews;
    vector<vk::Pipeline>       Pipelines;
    vector<vk::PipelineLayout> PipelineLayouts;
    vector<vk::Sampler>        Samplers;
    vector<vk::ShaderModule>   ShaderModules;
    vector<vk::DescriptorSet>  DescriptorSets;   // freed back to pool
    // extend as needed: render passes, framebuffers, ...
};
vector<RetireBin> m_RetireBins;   // size = MaxFramesInFlight + 1
```

- Public(ish) API: `Context::Retire(vk::Buffer, VmaAllocation)`, overloads per
  handle type. Typed bins instead of `function<void()>` callbacks: no
  allocation per retirement, and destruction order within a bin can be fixed
  (sets before pools, views before images).
- Index by `m_CurrentFrameInFlight`. One extra bin (`+1`) covers resources
  destroyed *before the first submit* or between `WaitIdle` and the next frame.
- **Drain point:** top of `AcquireNextFrame` (`src/Renderer/Backend/Context.cpp`),
  immediately after waiting that frame's `m_InFlightFence` — anything retired
  when that frame index was last recording is now GPU-idle.
- **Shutdown drain:** `Context::DisposeResources` (plan 02) drains *all* bins
  after `WaitIdle`.
- Destructors during/after context teardown: assert the context is alive in
  `Retire` — resources outliving the context are a consumer bug we want loud
  (per plan 03, that assert is fatal).

### Resource destructor changes

Each destructor stops calling `vkDestroy*`/`vmaDestroy*` directly and calls
`Context::Instance().Retire(...)` instead:
`Buffer.cpp`, `Image.cpp` (only when `m_Managed`), `ImageView.cpp`,
`Sampler.cpp`, `Shader.cpp`, `GraphicsPipeline.cpp`,
`DynamicGraphicsPipeline.cpp`, `ComputePipeline.cpp`, `PipelineLayout.cpp`,
`DescriptorSet.cpp`, `RenderPass.cpp`, `Framebuffer.cpp`.

Keep **immediate** destruction for objects that are never referenced by
in-flight GPU work or are themselves the sync mechanism: `Fence`, `Semaphore`
(owned per-frame, destroyed only at teardown after `WaitIdle`), `CommandPool`,
`DescriptorPool`, `SwapChain`.

### Deletions

- `SynchronizationFrame::SubmitResource` / `ResetResources` / `m_Resources`
  (`SynchronizationFrame.h:20-29,37`).
- `Command::SubmitResource` / `SubmitResources` (`Command.h:14-31`) and all
  call sites.
- `CommandBuffer::m_BoundResources` (`CommandBuffer.h:116`) and every
  `m_BoundResources.push_back` in `CommandBuffer.cpp`. The `Bind*` methods can
  then take `const T&` instead of `const Ref<T>&` where convenient (optional,
  can defer to plan 07's signature pass).
- `DescriptorSet::m_BoundResources` (`DescriptorSet.h:76`) — **keep for now**.
  This one is *ownership*, not frame-tracking: a descriptor set written with an
  ImageView dangles (beyond the in-flight window) if the view dies while the
  set is still bound in future frames. Plan 05 re-evaluates it.

### Ownership rule (document in a `docs/ownership.md` or header comment)

- **`Unique`** — single owner; GPU may use it, the retire queue makes that
  safe: per-frame sync primitives, pools, shaders owned by a renderer module,
  layouts.
- **`Ref`** — genuinely shared between systems. Convenience sharing allowed,
  never *required* for correctness.
- Audit `Create()` factories against the rule. Candidates to move `Ref` →
  `Unique` (or offer both): pipelines, `Buffer`, `RenderPass`, `Framebuffer`.
  Keep `Ref` where consumers actually share (`Image`/`ImageView`,
  `DescriptorSet`). Do the cheap moves now; don't force every caller to change
  in this plan — the point is that the *rule* exists.

## Risks / notes

- `m_MaxFramesInFlight = 2` (`Context.h:132`): bin count must follow it if it
  ever becomes configurable.
- `SubmitImmediateCommands` / `ImmediateCommands` (`Context.h:67,85`) wait
  idle internally — resources used there may retire into the current bin and
  be drained later than necessary; that's correct, just not optimal.
- Swapchain recreation: `SwapChain` images are unmanaged (`Image::Create(vk::Image, ...)`)
  and must not be retired — already guarded by `m_Managed`.

## Acceptance

- `grep -rn "SubmitResource\|m_BoundResources" include/ src/` → only the
  `DescriptorSet` ownership list remains.
- Validation layers clean while resizing (swapchain churn) and while
  creating/dropping buffers and images mid-frame without any manual keep-alive.
- A deliberate "drop Ref immediately after recording a draw that uses it" test
  renders correctly for `MaxFramesInFlight + 1` frames.
