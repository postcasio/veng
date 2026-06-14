# Resource ownership in veng

veng resources (`Buffer`, `Image`, `ImageView`, `Sampler`, `Shader`, pipelines,
`PipelineLayout`, `RenderPass`, `Framebuffer`, `DescriptorSet`, …) are handed
out as smart pointers from their `Create()` factories. Which smart pointer to
use is governed by one rule.

## The rule

- **`Unique<T>`** — a resource with a *single* owner. The GPU may still be using
  it when the owner drops it; that is safe because destruction is deferred (see
  below). Use `Unique` for: single-owner sync primitives (`Fence`, `Semaphore`,
  `TimelineSemaphore`), pools, layouts, and resources a single renderer module
  owns (e.g. the shaders and pipelines a pass creates and never shares).

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

## The transfer-keyed retire path

An async upload runs on a worker thread, off the frame's fence. The staging
buffer it allocates is consumed by a copy submitted on the **transfer queue**,
which signals a monotonic value on the transfer timeline — not the frame fence —
when the copy completes. Binning that scratch on the current frame's fence would
free it on the wrong clock. So the upload job nulls the staging wrapper (so its
destructor does *not* frame-bin retire) and hands the raw handle to
`Context::Native::RetireOnTransfer(buffer, allocation, timelineValue)`, which
pins it to that transfer-timeline value; the handle is destroyed only once the
transfer timeline has reached it.

Because uploads drop resources from worker threads, the whole retire path is
mutex-guarded: every `Retire` overload and `RetireOnTransfer` take the context's
retire lock, so an off-thread drop never races the main thread advancing the
frame or draining a bin.

The one deliberate exception is `DescriptorSet`, which retains `Ref`s to the
resources it was written with (`DescriptorSet::m_BoundResources`). That is
*ownership*, not frame-tracking: a set written with an `ImageView` would dangle
beyond the in-flight window if the view died while the set is still bound in a
future frame.

## Asset handles and bindless handles are not `Ref`s

Two higher-level handle types sit *above* the `Ref`/`Unique` rule and must not be
confused with it.

- **`AssetHandle<T>` / `WeakAssetHandle<T>`** (`Veng/Asset/AssetHandle.h`) is a
  refcounted reference *to an asset*, not to a GPU resource. It is indirection
  into the `AssetManager`'s cache (`map<AssetId, Ref<AssetCacheEntry>>`): copies
  share the cache entry and keep the asset resident; dropping the last handle
  makes the asset evictable on the next `CollectGarbage()`. The *engine resources
  inside* an asset (its `Ref<Image>`, `Ref<Buffer>`, …) still follow the rule
  above unchanged — a handle is a reference *to* an asset, never a substitute for
  the `Ref<T>` *within* one. This indirection is what lets a future hot-reload
  swap the resource behind a handle without invalidating outstanding handles.

- **Bindless handles** (`TextureHandle`, `SamplerHandle`, `StorageImageHandle`,
  `MaterialHandle` in `Veng/Renderer/BindlessRegistry.h`) are plain `u32` slot
  ids, **not owners**. The owning `Ref` lives in the `BindlessRegistry`, which
  keeps a `Ref` to every registered resource so a live slot can't dangle. A
  handle is just the index a shader uses to reach the resource through set 0.

Eviction and bindless slot release both defer through the **existing per-frame
retire queue** — `CollectGarbage()` drops the cache entry's `Ref` (which retires
the GPU resource), and `BindlessRegistry::Release` returns the slot only after
`Context::AcquireNextFrame` has cycled past every frame-in-flight that could
still reference it. There is no second reclamation mechanism.

## Factory audit

The factories are not all aligned to the rule yet. The rule is what matters; the
cheap `Ref → Unique` moves (pipelines, `Buffer`, `RenderPass`, `Framebuffer`)
can happen opportunistically without forcing every caller to change at once.
`Image` / `ImageView` / `DescriptorSet` stay `Ref` because consumers genuinely
share them.
