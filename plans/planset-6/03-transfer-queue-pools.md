# Plan 03 — transfer queue + per-worker command pools

**Goal:** give `Context` a **dedicated transfer queue** (with the MoltenVK
single-family collapse handled as a first-class case) and a **transfer command
pool per worker**, set up once via `TaskSystem::ForEachWorker`. Plus the
**submission lock** that serializes `vkQueueSubmit` across the worker and main
threads. This is the Vulkan plumbing plan 07 records uploads onto — no async
upload yet, just the queue/pool/lock infrastructure standing up correctly under
validation.

## Why this is its own plan

`VkCommandPool` is **not shareable across threads** — the single hardest Vulkan
rule this planset enforces — and queue selection has a platform fork (dedicated
transfer family vs. MoltenVK's one family). Isolating the queue/pool/lock setup
from the upload *logic* (plan 07) means this lands and validates on its own: the
device comes up with the new queue, the pools exist per worker, and nothing yet
*uses* them — so a validation error here is unambiguously about setup, not about a
mis-synchronized copy.

## Work

### 1. Transfer family + queue selection

Extend `QueueFamilyIndices` (it lives in the **public** header
`engine/include/Veng/Renderer/Context.h`, not a backend path — a plain
`optional<u32>` is `include_hygiene`-safe, so adding the field there does not leak a
backend type):

```cpp
struct QueueFamilyIndices
{
    optional<u32> GraphicsFamily;
    optional<u32> PresentFamily;
    optional<u32> TransferFamily;   // NEW
    // ...
};
```

Selection (`FindQueueFamilies` in `Context.cpp`):

- **Prefer a queue family with `eTransfer` but *not* `eGraphics`** — the discrete
  DMA path on desktop GPUs.
- **Fall back to the graphics family** when no transfer-only family exists
  (MoltenVK). The code must treat **"transfer == graphics"** as a first-class case
  (no ownership transfer, one queue) — this is the **primary dev platform**, so it
  is the path that must be correct and tested, not the afterthought.
- Acquire the transfer `VkQueue` in device creation beside the graphics/present
  queues. When the family collapses to graphics, the transfer queue handle may be
  the same queue (or a second queue from the same family if the family exposes
  enough — but a *second queue from the same family still shares no submission
  thread-safety guarantee*, so the submission lock below applies regardless).

### 2. Per-worker transfer command pools

`VkCommandPool` is single-thread. Each worker gets its own transfer pool, created
once at init via `TaskSystem::ForEachWorker`, stored worker-indexed **on
`Context`** (not `thread_local` — indexed-on-`Context` ties lifetime to the device
for clean teardown and survives dynamic worker counts; README/threading-doc open
decision, resolved this way):

```cpp
// Context — keyed by worker index, set up once via ForEachWorker:
[[nodiscard]] CommandBuffer& Context::GetTransferCommandBuffer(u32 workerIndex);
```

Each pool is created with the transfer family index and `eResetCommandBuffer` (or
reset-per-use) so a worker can re-record across uploads. Pools are destroyed with
the `Context`.

**Command-buffer reuse is timeline-gated.** A transfer command buffer recorded for
upload N must not be reset/re-recorded until its **transfer-timeline value** is
reached (the GPU is done with it) — resetting it earlier corrupts an in-flight
submit. With one pool per worker doing back-to-back uploads, the worker either waits
its own last timeline value before re-recording, or the pool holds a small **ring**
of command buffers cycled by timeline value. Specify the rule here; plan 07's upload
records against it. (Same lifetime principle as plan 05's `RetireOnTransfer` — the
transfer timeline, not a frame fence, governs transfer-side lifetimes.)

### 3. The submission lock

`vkQueueSubmit` to a `VkQueue` is **not thread-safe**. Add a single
`std::mutex` on `Context` guarding every submit to a shared queue. On MoltenVK the
transfer and graphics queues are the same family (and may be the same queue), so a
worker's transfer submit and the main thread's frame submit **must** serialize
through this lock. Route `SubmitImmediateCommands`, the frame submit, and (plan 07)
the transfer submit through one locked helper.

**The transfer-timeline value is allocated *inside* the lock and returned.** A
timeline semaphore must signal strictly increasing values; if a worker does
`u64 v = ++counter` and then races for the submit lock, two workers can signal out
of order (B then A for values 6 then 5) — illegal. So the transfer submit helper
allocates the next value **while holding the lock**, immediately before
`vkQueueSubmit`, and returns it:

```cpp
// Allocates the next monotonic value under the submit lock, submits, returns it.
u64 Context::SubmitTransfer(CommandBuffer& cmd, const TimelineSemaphore& timeline);
```

The caller (plan 07) uses the **returned** value for `RetireOnTransfer` and
`AddFrameTransferWait` — never a value computed before the submit.

## Dependencies

Plan 02 (`TaskSystem::ForEachWorker` creates the per-worker pools). Independent of
plan 01 and the barrier rule (plan 06).

## Acceptance

- Clean build, `ctest` green, smoke binary writes a correct-sized PPM.
- **Validation-verified:** run `headless_smoke` / `veng_gpu` from `build-debug/`
  and `ctest -L validation` — the device creates the transfer queue and per-worker
  pools with **no** new `Vulkan validation` ERROR (queue family index valid, pools
  created against the right family, no allowlist widening).
- On the dev box (MoltenVK) `TransferFamily == GraphicsFamily` and the collapsed
  path is exercised; the selection logic still *prefers* a dedicated family where
  one exists (assert/log the chosen indices so the path taken is visible).

## Notes

- Nothing *uses* the transfer queue/pools yet — this plan only stands them up.
  Plan 07 records the first upload onto `GetTransferCommandBuffer`.
- Keep the "transfer == graphics" branch loud and central — every later piece
  (ownership transfer in plan 06, the upload in plan 07) keys off whether the
  families differ, and getting the fallback right here is what makes the dev-box
  path the tested one.
</content>
