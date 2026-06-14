# Plan 04 — `TimelineSemaphore`

**Goal:** add a `TimelineSemaphore` sync primitive beside `Semaphore` / `Fence`,
so the loader can signal a monotonically increasing value as uploads complete and
the render thread can wait the value a given resource depends on. This is the
loader↔render synchronisation channel plan 07 uses — it composes far better than a
fence-per-upload or the per-frame binary semaphores veng has today.

## Why this is its own plan

A new sync primitive is small, self-contained, and independently testable (host
signal/wait round-trips, monotonicity) — there is no reason to fold it into the
upload logic. Landing it alone also surfaces the one device-feature question early:
timeline semaphores must be enabled and supported on MoltenVK before anything
depends on them.

## API

```cpp
// engine/include/Veng/Renderer/TimelineSemaphore.h
class TimelineSemaphore
{
public:
    static Unique<TimelineSemaphore> Create(Context& context, u64 initialValue = 0);

    void                Signal(u64 value);        // host-side signal
    void                Wait(u64 value) const;     // host-side wait until >= value
    [[nodiscard]] u64   GetValue() const;          // current counter
    // queue-side wait/signal go through the submit info, like today's semaphores
};
```

- **Ownership: `Unique`** — a single-owner sync primitive, consistent with the
  ownership rule, which already lists `Semaphore` / `Fence` as `Unique`. It takes a
  `Context&` and stores the back-reference for deferred-destruction `Retire`, like
  every other resource (planset-4).
- The `Native` idiom applies: the `VkSemaphore` (created with a
  `VkSemaphoreTypeCreateInfo{ eTimeline }`) lives in a `.cpp`-defined `Native`; the
  public header pulls in no `vk::` type. `include_hygiene` covers it.

## Work

1. **Enable the feature — a required edit, not a confirmation.** veng targets
   Vulkan 1.3 and already builds a `vk::PhysicalDeviceVulkan12Features` chain in
   `Context` device creation, but **`timelineSemaphore` is not set in it today** —
   it is definitively off. Set `.timelineSemaphore = vk::True` on that existing
   `vulkan12Features` struct (a one-liner; the chain is already wired). **Fatal-
   assert** (`VE_ASSERT`) if the device reports it unsupported — it is a hard
   requirement for the async path, not a fallback.
2. **`TimelineSemaphore::Create`** — create the timeline `VkSemaphore` with the
   given initial value; store the `Context&` back-ref.
3. **Host `Signal` / `Wait` / `GetValue`** — `vkSignalSemaphore`,
   `vkWaitSemaphores`, `vkGetSemaphoreCounterValue`, each wrapped in the engine's
   `VK_ASSERT` / `VK_RAW_ASSERT` policy.
4. **Queue-side wait/signal** go through the submit-info path (a `u64` value per
   wait/signal semaphore via `VkTimelineSemaphoreSubmitInfo`) — plan 07 uses this
   to signal on the transfer submit and wait on the graphics submit. Expose
   whatever the `Context` submit helper needs (the timeline handle + value), not a
   second submit API.

## Tests (`tests/gpu`, in the `veng_gpu` suite — needs a device)

- Host signal then host wait on the same value returns immediately.
- `GetValue` reflects the latest host signal.
- Monotonic: signalling increasing values advances the counter; the suite skips
  with no ICD like the rest of the `gpu` band.

## Dependencies

Independent (needs only `Context` device creation). Lands before plan 07, which
consumes it.

## Acceptance

- Clean build, `ctest` green (the `timeline_semaphore` GPU case included, skipping
  with no ICD), smoke binary writes a correct-sized PPM.
- **Validation-verified:** the feature is enabled and the primitive creates/signals
  /waits with no new `Vulkan validation` ERROR from `build-debug/`.
- `include_hygiene` still builds — the public header leaks no `vk::` type.

## Notes

- This replaces *nothing* — the per-frame binary `Semaphore`s
  (`Renderer/Semaphore.h`) stay for present/acquire sync. The timeline is purely
  the loader↔render channel.
- A fence-per-upload would also work but scales badly (one `VkFence` per asset); the
  timeline is one object the render thread waits a value on. This is the reason it
  earns a place beside `Fence`, not a replacement for it.
</content>
