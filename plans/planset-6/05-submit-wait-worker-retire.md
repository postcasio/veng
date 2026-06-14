# Plan 05 — frame-submit timeline waits + worker-safe deferred retirement

**Goal:** open up the two single-threaded chokepoints the async upload path needs
but that today's `Context` cannot express:

1. **The graphics frame submit must be able to wait a transfer timeline value.**
   `Context::SubmitFrame` builds a fixed `vk::SubmitInfo` (one wait semaphore:
   image-available; no timeline). The render side has no way to make the frame
   *wait* `transferTimeline >= value` before sampling an async-uploaded resource.
2. **Deferred retirement must be thread-safe and transfer-aware.** A resource
   dropped on a **worker thread** mid-upload retires through `Context::Retire`,
   which today does an *unsynchronized* `emplace_back` into
   `RetireBins[CurrentFrameInFlight]` — a data race with the main thread, and worse,
   it pins the resource to the **frame fence**, not the **transfer timeline** that
   actually governs the upload's GPU lifetime. A staging buffer can be destroyed
   while its transfer copy is still in flight (GPU use-after-free).

This plan makes both correct **before** any worker records an upload (plan 07), so
the load-bearing sync has a real home and the retire path is proven thread-safe in
isolation rather than discovered broken under a worker.

## Why this is its own plan

The original draft folded these into the async-upload plan as one-line claims ("the
render graph waits the timeline"; "staging rides the existing retire queue,
untouched"). Both are false against the real code: there is **no** submit path that
carries a timeline wait, and the retire path is **not** thread-safe and pins to the
wrong lifetime. These are infrastructure changes to `Context`, independent of the
upload *logic*, and each is a correctness hazard the smoke render cannot catch — so
they land alone, on the main thread, validation-verified, exactly like
planset-4/01 isolated the retire reroute.

## Part 1 — frame submit carries timeline waits

`Context::SubmitFrame` (`engine/src/Renderer/Backend/Context.cpp`) submits the frame
command buffer with a single binary wait semaphore. Extend it so the render side can
register **transfer-timeline waits** that the next frame submit folds in:

```cpp
// Context — accumulate per-frame transfer waits; SubmitFrame consumes them.
void Context::AddFrameTransferWait(const TimelineSemaphore& timeline, u64 value);
```

- `SubmitFrame` builds a `vk::TimelineSemaphoreSubmitInfo` (chained via `pNext`):
  the timeline semaphore(s) join the `pWaitSemaphores` array with their `u64` wait
  values, at the `eFragmentShader` (sampled-image) wait stage.
- **Both submit branches.** `SubmitFrame` has a **windowed** path (waits the
  image-available binary semaphore) *and* a **headless** path (today: no wait
  semaphores at all — `Context.cpp:644-654`). The accumulated transfer waits must
  be folded into **both** — and the headless path is the one that matters most,
  because `headless_smoke` and the `veng_gpu` async-upload case (plan 07's mandated
  test) run headless. A wait dropped on the headless branch would let the pass
  sample the image before the transfer signals — exactly the race the validation
  gate exists to catch. The headless submit, which has no `pWaitSemaphores` today,
  gains the timeline wait array + `TimelineSemaphoreSubmitInfo`.
- The pending-wait set is cleared each frame after submit. A resource sampled in a
  frame registers its `(timeline, value)` via the render graph (plan 06's emission
  site / plan 07's consumer) so the frame does not begin GPU work that reads the
  resource until the transfer has signalled.
- **Binary present/acquire semaphores are unchanged** — the timeline rides
  *alongside* them (windowed) or alone (headless), never replacing them.

> This is the home for "the render graph waits the timeline." The render graph
> records barriers into the command buffer; the **wait** belongs on the queue
> submit, which only `SubmitFrame` performs — so the value is threaded out to here,
> not expressed inside `RenderGraph::Execute`.

## Part 2 — worker-safe, timeline-keyed retirement

`Context::Retire` / the `RetireBins` / `DrainRetireBin` machinery
(`Backend/Context.cpp`, `ContextNative.h`) is single-threaded today, and a `Buffer`
destructor *unconditionally* routes its handle through the **frame** bin
(`Buffer::~Buffer`, `Backend/Buffer.cpp:43-46` → `CurrentRetireBin()`). Three fixes:

1. **Serialize retirement.** Guard `Retire`, the transfer list below, *and* their
   drains with **one** mutex on `Context`, so a worker dropping a resource cannot
   race the main thread's `CurrentFrameInFlight` read, the bin vector's
   reallocation, or the transfer-list drain. The drain holds the lock across both
   the `GetValue()` read and the erase.

2. **A transfer-retire list keyed on timeline value — taking the *raw* handle.**
   A staging buffer created on a worker must be destroyed when its **transfer
   timeline** value is reached, not the frame fence. Crucially, it **cannot** go
   through `Buffer::~Buffer` — that dtor re-defers the handle into the frame bin,
   which would reintroduce the wrong-lifetime bug one level down (and can hit the
   "resource outlived the context" assert at teardown). So `RetireOnTransfer` takes
   the **released raw VMA handle**, not a `Ref<Buffer>`:

   ```cpp
   // Take ownership of a raw allocation whose GPU lifetime is the transfer
   // timeline. Reclaimed (vmaDestroyBuffer) when transferTimeline.GetValue() >= value.
   void Context::RetireOnTransfer(vk::Buffer handle, VmaAllocation alloc, u64 timelineValue);
   ```

   This needs a **"release without retiring"** path on `Buffer`: a method that
   hands out `(vk::Buffer, VmaAllocation)` and nulls the wrapper's handles so its
   destructor is a no-op (it does not touch `Retire`). Plan 07's async `Upload`
   creates the staging `Buffer`, fills it, records the copy, then `release()`s it
   into `RetireOnTransfer(value)`. Drained on the **main thread** beside the
   frame-bin drain in `AcquireNextFrame`: under the retire mutex, destroy every
   entry whose `value <= transferTimeline.GetValue()` via `vmaDestroyBuffer`.

3. **Drain the transfer list at teardown too.** The per-frame drain in
   `AcquireNextFrame` does not run after the *last* upload before shutdown, and the
   headless smoke renders one frame then exits — so the transfer list would leak (or
   a late `Ref` drop would hit the `Disposed` assert). `DisposeResources` /
   `Dispose` (`Context.cpp:270,286`, which today drain only `RetireBins` via
   `DrainAllRetireBins`) must, **before `Disposed = true`**, `WaitIdle` (or host-wait
   the transfer timeline's last value) and then destroy every remaining transfer
   entry. `WaitForAll()` (plan 02) joins workers first, so no new entries arrive
   during teardown.

- **The ordinary frame-bin path is unchanged** for main-thread drops (draws,
  per-frame resources). Only worker-created upload scratch uses the
  release-into-`RetireOnTransfer` path.
- The resource `m_Context` back-reference (planset-4) is untouched; what changes is
  that worker-created scratch is *released* (not retired) and tracked against the
  transfer timeline, under the retire mutex.

## Dependencies

Plan 04 (`TimelineSemaphore` — both parts reference a timeline value). Independent
of plans 01/02/03 in code, though it exists to serve plan 07.

## Acceptance

- Clean build, `ctest` green, smoke binary writes a correct-sized PPM.
- **Validation-verified:** `SubmitFrame` with an added timeline wait produces no new
  `Vulkan validation` ERROR from `build-debug/`; `ctest -L validation` green. (No
  consumer yet — a GPU-suite case may exercise `AddFrameTransferWait` +
  `RetireOnTransfer` directly to prove the plumbing before plan 07 uses it.)
- A focused test (in `veng_gpu`) releases a buffer's raw handle into
  `RetireOnTransfer(value)` and confirms it is **not** destroyed until the timeline
  reaches `value`, then is — and that a buffer's `release()` path leaves the wrapper
  dtor a no-op (no frame-bin retire).
- `Retire`, the transfer list, and both drains share one mutex — provable by
  inspection; the data race is gone.
- Teardown destroys any remaining transfer entries (no leak, no `Disposed` assert):
  exercise upload-then-immediate-dispose without an intervening frame.

## Notes

- This plan exists because the async-upload draft assumed two capabilities the
  engine doesn't have. Building them here, observable and tested, means plan 07 is
  pure composition: record the copy, signal the timeline, `release()` the staging
  into `RetireOnTransfer`, `AddFrameTransferWait` on first graphics use — every
  primitive already proven.
- The mutex on `Retire` is uncontended on the main-thread-only paths (draws), so the
  single-threaded render loop pays effectively nothing; it exists for the worker
  drops the async path introduces.
</content>
