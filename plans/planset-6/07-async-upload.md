# Plan 07 — async `Buffer/Image::Upload` (the default)

**Goal:** wire plans 01–06 together into a **non-blocking** `Upload` that is the
*default* spelling: it routes the staging fill + copy onto the task system and the
transfer queue, records the ownership-release half + producing-family marker
(plan 06), signals the timeline (plan 04), retires the staging buffer through the
transfer-keyed path (plan 05), and returns a `Task<void>`; the first graphics pass
that uses the resource registers a frame transfer-wait (plan 05) and emits the
acquire half (plan 06). **`WaitIdle` is gone from this path** — that is the whole
point. This is the join plan and carries the Vulkan-correctness risk.

## Why this is its own plan, on the main thread

Every prior plan stood up a piece in isolation; this one composes them on the GPU
where a missing ownership barrier, a too-early timeline wait, or a cross-thread pool
misuse becomes a real race. The smoke render can miss it (validation errors don't
fail tests; a race may only show on non-dev hardware). So it lands alone, on the
main thread, validation-verified, with a dedicated GPU-suite case — not folded into
the asset layer (plan 08).

## API

```cpp
// Image.h / Buffer.h — async is the default; the blocking path is the marked one (plan 01):
[[nodiscard]] Task<void> Image::Upload(TaskSystem& tasks, std::span<const u8> data);
[[nodiscard]] Task<void> Buffer::Upload(TaskSystem& tasks, std::span<const u8> data, u64 offset = 0);
//                        Image::UploadSync / Buffer::UploadSync  — still block (WaitIdle), from plan 01
```

## Work

The async `Upload` submits onto the task system:

```
Upload(tasks, data):                          // member of Image/Buffer
  Ref<Image> self = shared_from_this();        // capture a Ref, NOT raw this — see note
  return tasks.Submit([=] {
     staging = Buffer::Create(ctx, ...); staging->UploadSync(data);   // worker: host-visible memcpy, no GPU wait
     cmd = ctx.GetTransferCommandBuffer(thisWorker);                  // plan 03: per-worker transfer pool ONLY
     cmd.CopyBufferToImage(staging, self);                            // transfer queue
     if (transferFamily != graphicsFamily)
         cmd.ReleaseToQueue(graphicsFamily);                          // plan 06: release half (no-op when equal)
     self->MarkProducedOn(transferFamily);                           // plan 06: the rule reads this marker
     u64 value = ctx.SubmitTransfer(cmd, transferTimeline);          // plan 03: value allocated UNDER the lock, returned
     auto [buf, alloc] = staging->Release();                         // null the wrapper: its dtor will NOT frame-bin retire
     ctx.RetireOnTransfer(buf, alloc, value);                        // plan 05: raw handle, pinned to the timeline value
  });
  // First graphics use of `self`: the render graph emits the acquire half (plan 06)
  // and calls ctx.AddFrameTransferWait(transferTimeline, value) (plan 05) so the
  // frame submit waits transferTimeline >= value before the sampling pass.
```

0. **Prerequisite header change: `Buffer` must derive from
   `enable_shared_from_this`.** The async `Upload` lambda captures `Ref<Buffer>`/
   `Ref<Image>` via `shared_from_this()` (see the capture note). `Image` already
   derives from `std::enable_shared_from_this<Image>` (`Image.h`), but **`Buffer`
   does not** (`Buffer.h`) — so `Buffer::Upload`'s `shared_from_this()` won't
   compile, and `MeshLoader` uploads vertex/index data through `Buffer::Upload`.
   Add `: public std::enable_shared_from_this<Buffer>` to `Buffer` as part of this
   plan (or have async `Buffer::Upload` take the owning `Ref<Buffer>` from the
   caller). Also add `Buffer::Release()` → `(vk::Buffer, VmaAllocation)` that nulls
   the wrapper so its destructor does not retire (used below + by plan 05).
1. **Async `Upload` on `Buffer` and `Image`** routes through `TaskSystem::Submit`.
   The staging buffer is created and filled on the worker with `UploadSync` (a
   host-visible memcpy — `Buffer` is always `HOST_VISIBLE | HOST_COHERENT` and
   uploads via `vmaCopyMemoryToAllocation`, so **no manual flush is needed even from
   a worker**).
2. **Record onto the per-worker transfer command buffer *only*.** The async path
   **must not** reuse `Image::UploadSync`'s body — that allocates a command buffer
   from the **shared** `Context` command pool (`CommandBuffer::Create` →
   `Context::Native::CommandPool`), a `VkCommandPool` cross-thread violation. The
   worker records *exclusively* onto `GetTransferCommandBuffer(workerIndex)`
   (plan 03). `Buffer::Create` itself is safe concurrently (VMA is internally
   synchronized); command-pool allocation is not.
3. **Record the release half + mark the producing family** (`transferFamily !=
   graphicsFamily`) so plan 06's rule emits the acquire half on first graphics use.
   Both are no-ops on MoltenVK.
4. **Submit on the transfer queue under the submission lock** (plan 03). The lock
   helper `SubmitTransfer` **allocates the monotonic timeline value while holding
   the lock** and returns it — never compute the value before the submit, or two
   workers can signal out of order (an illegal non-monotonic timeline signal). **No
   `WaitIdle`.**
5. **Release the staging buffer's raw handle into `RetireOnTransfer(buf, alloc,
   value)`** (plan 05) — its GPU lifetime is the transfer timeline, not the frame
   fence. It **must not** go through `Buffer::~Buffer` (which unconditionally
   frame-bin retires, reintroducing the wrong-lifetime bug): call `staging->Release()`
   to take `(vk::Buffer, VmaAllocation)` and null the wrapper, then hand the raw
   handle to `RetireOnTransfer`, which `vmaDestroyBuffer`s it once
   `transferTimeline.GetValue() >= value`.
6. **Render-side wait + acquire.** On first graphics use, the render graph emits the
   acquire barrier (plan 06) and registers `AddFrameTransferWait(transferTimeline,
   value)` (plan 05) so the frame submit waits the value before the sampling pass. A
   resource whose value isn't yet reached simply isn't sampled until it is — the
   consumer (plan 08) gates the draw on residency.

> **Bindless registration is NOT done on the worker.** A `Texture`/`Image` that
> registers into the set-0 `BindlessRegistry` (a global free-list mutation, the
> single-threaded contract) must do so on the **main thread**. The worker produces
> an unregistered, uploaded image; registration happens in the main-thread
> continuation. The asset-layer consumer (plan 08) owns this — the low-level
> `Upload` here only uploads; it does not register.

## Tests (`tests/gpu`, `veng_gpu` suite)

- **Async upload + sample round-trip:** upload an image on a worker via
  `Image::Upload(tasks, …)`, then in a graphics pass sample it (with the frame
  transfer-wait registered) and read back the expected pixels — proving the timeline
  wait + ownership acquire produce a correct, race-free result. Skips with no ICD.
- **`RetireOnTransfer` lifetime** (may reuse plan 05's case): the staging buffer is
  not destroyed until the timeline value is reached.
- **The blocking `UploadSync` path is unchanged** (regression guard).
- This case is what makes the **validation gate** cover the async path.

## Dependencies

Plans **01** (the freed `Upload` name / `UploadSync` to build staging on), **02**
(`TaskSystem`), **03** (transfer queue, per-worker pools, submit lock), **04**
(`TimelineSemaphore`), **05** (`AddFrameTransferWait` + `RetireOnTransfer`), **06**
(the acquire-half rule + producing-family marker). The join of the planset.

## Acceptance

- Clean build, `ctest` green (the async-upload GPU case included), smoke binary
  writes a correct-sized PPM.
- **Validation-verified, mandatory:** run `veng_gpu` from `build-debug/` and
  `ctest -L validation` — the async upload + sample path produces **no** new
  `Vulkan validation` ERROR (no missing queue-ownership barrier, no
  under-synchronized timeline wait, no cross-thread pool use). **No allowlist
  widening.**
- No `WaitIdle` on the async path — confirm by inspection and by the frame not
  stalling on upload. `SubmitImmediateCommands`'s `WaitIdle` survives only on
  `UploadSync`.

## Notes

- **Capture a `Ref`, not `this`.** The worker lambda must capture `Ref<Image>`/
  `Ref<Buffer>` so the resource cannot be destroyed before the worker runs; `[=]`
  over a raw `this`/`&` is a use-after-free. `Image` already derives from
  `enable_shared_from_this`; **`Buffer` does not and must be made to** (work item 0)
  — until then `Buffer::Upload`'s `shared_from_this()` does not compile.
- The MoltenVK collapse means the dev box never exercises the *cross-queue* barriers
  at runtime — plan 06's unit test is their coverage. What the dev box *does* verify
  here is that work moved off the main thread, the timeline gates the sample
  correctly, the staging retires on the right fence, and the same-family path is
  race-free.
- Keep the release/acquire indices paired with plan 06's rule — a mismatch is a
  silent ownership bug. Skip both halves when families are equal.
</content>
