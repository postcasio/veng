# Resource ownership in veng

veng resources (`Buffer`, `Image`, `ImageView`, `Sampler`, `Shader`, pipelines,
`PipelineLayout`, `RenderPass`, `Framebuffer`, `DescriptorSet`, …) are handed
out as smart pointers from their `Create()` factories. Which smart pointer to
use is governed by one rule.

## The rule

- **`Unique<T>`** — a resource with a *single* owner. The GPU may still be using
  it when the owner drops it; that is safe because destruction is deferred (see
  below). Use `Unique` for: per-frame sync primitives, pools, layouts, and
  resources a single renderer module owns (e.g. the shaders and pipelines a
  pass creates and never shares).

- **`Ref<T>`** — a resource *genuinely shared* between independent systems, where
  more than one of them needs to keep it alive. Convenience sharing is allowed,
  but `Ref` should never be *required* purely for correctness — that is what
  deferred destruction is for. Use `Ref` where sharing is real: `Image` /
  `ImageView` handed to several passes, a `DescriptorSet` referenced by both the
  code that wrote it and the code that binds it.

If you are unsure, prefer `Unique`. Reach for `Ref` only when two systems must
both observe the resource's lifetime.

## Why dropping a resource mid-frame is safe

Resource destructors do **not** call `vkDestroy*` directly. They *retire* the
Vulkan handle into the current frame's bin on `Context`. The handle is only
destroyed once that frame's fence has been waited again — i.e. once the GPU has
finished the work that could still reference it (`Context::AcquireNextFrame`).

This means you can drop the last `Ref`/`Unique` to a resource the instant after
recording a draw that uses it; the in-flight GPU work finishes against a still-
valid handle, and the handle is reclaimed a couple of frames later. There is no
manual "keep this alive until the frame is done" bookkeeping — the old
`SubmitResource` / command-buffer keep-alive lists are gone.

The one deliberate exception is `DescriptorSet`, which retains `Ref`s to the
resources it was written with (`DescriptorSet::m_BoundResources`). That is
*ownership*, not frame-tracking: a set written with an `ImageView` would dangle
beyond the in-flight window if the view died while the set is still bound in a
future frame. (Re-evaluated in plan 05.)

## Factory audit

The factories are not all aligned to the rule yet. The rule is what matters; the
cheap `Ref → Unique` moves (pipelines, `Buffer`, `RenderPass`, `Framebuffer`)
can happen opportunistically without forcing every caller to change at once.
`Image` / `ImageView` / `DescriptorSet` stay `Ref` because consumers genuinely
share them.
