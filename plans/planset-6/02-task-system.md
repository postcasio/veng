# Plan 02 — `TaskSystem` + `Task<T>`

**Goal:** stand up the CPU-side concurrency foundation: a `TaskSystem` (a fixed
thread pool draining a work queue) and a `Task<T>` result handle, plus the
main-thread continuation pump. **No Vulkan in this plan** — it is a pure
threading subsystem, unit-tested on its own, that plans 03–08 build the GPU work
onto.

## Why this is its own plan

The concurrency model is the load-bearing design decision of the planset (README
decision 1), and it is fully testable without a GPU: submit jobs, observe results,
prove the main-thread pump runs continuations on the calling thread and never on a
worker. Landing it alone means the threading semantics are nailed down and
covered before any Vulkan queue correctness is layered on, so a later bug is
unambiguously in the GPU path, not the scheduler.

## API

Owned by `Application`, threaded explicitly — **no global singleton** (the
planset-4 lesson).

```cpp
namespace Veng
{
    struct TaskSystemInfo
    {
        // 0 = derive from hardware_concurrency() - 1 (leave the main thread a core).
        u32 WorkerCount = 0;
    };

    template <typename T>
    class Task;                        // result handle (below)

    class TaskSystem
    {
    public:
        explicit TaskSystem(const TaskSystemInfo& info = {});
        ~TaskSystem();                 // joins all workers

        template <typename Fn>
        auto Submit(Fn&& fn) -> Task<std::invoke_result_t<Fn>>;

        // Run fn on each worker once, for per-thread setup (plan 03's transfer
        // pools). Blocks until every worker has run it.
        void ForEachWorker(const function<void(u32 workerIndex)>& fn);

        // Drain the main-thread continuation queue. Application calls this once
        // per frame at the TOP of Frame(), BEFORE BeginFrame() — this is where
        // Task::Then continuations and async-load "lands" run, on the main thread.
        void PumpMainThread();

        // Block until the queue is empty and all in-flight tasks finish. For
        // OnDispose ordering — assets must finish before teardown.
        void WaitForAll();

        [[nodiscard]] u32 GetWorkerCount() const;
    };

    template <typename T>
    class Task
    {
    public:
        [[nodiscard]] bool      IsReady() const;          // non-blocking poll
        [[nodiscard]] Result<T> Get();                    // blocks, then moves out
        void                    Then(function<void(Result<T>)>);  // main-thread continuation
    };
}
```

## Work

1. **`TaskSystem`** — a `vector` of worker threads, a mutex-guarded work queue, a
   condition variable. `Submit` wraps the callable in a shared task state, pushes
   it, and returns a `Task<T>` referencing that state. The destructor sets a stop
   flag, notifies, and joins.
2. **`Task<T>`** — a future-like handle over the shared state. `IsReady` is a
   non-blocking flag check; `Get` waits and moves the `Result<T>` out. **`Result<T>`
   is the error channel, not exceptions** (the engine contract) — a worker that
   fails returns `Result<T>` and the handle carries it through. `void` results use
   `VoidResult` (a `Task<void>` specialization or `Result<std::monostate>`).
3. **Main-thread continuations.** `Then` enqueues onto a separate, mutex-guarded
   *main-thread* queue that only `PumpMainThread()` drains. A continuation that
   touches engine state (registering a texture into bindless, swapping a cache
   entry) must never run on a worker — this is how plan 08's async load lands safely
   on the single render thread. `Submit`'s callable runs on a worker; its `Then`
   runs on main.

   **`PumpMainThread()` runs at the top of `Application::Frame()`, before
   `BeginFrame()`.** `BeginFrame` → `AcquireNextFrame` advances the frame index and
   drains that frame's retire bin / waits its fence; a continuation that registers a
   resource or retires a handle must run *before* that advance, or its GPU-state
   mutation lands in an ambiguous frame window. Place the pump first, not merely
   "near" `BeginFrame`.
4. **`ForEachWorker`** — dispatches a one-shot task to each worker and waits for
   all, used by plan 03 to create a transfer command pool per worker.
5. **Shape `Task<T>` for a future graph.** Keep the handle's surface minimal so a
   later task *graph* (dependencies, scheduling) can wrap it without breaking
   callers (README decision 1) — do not bake polling-only or `Then`-only
   assumptions into the public type.

## Tests (`tests/unit`, no GPU)

- Submit N independent jobs; all results come back correct.
- A job returning an error `Result<T>` surfaces it through `Get()` (no throw —
  the build is `-fno-exceptions`).
- `Then` continuations run **only** during `PumpMainThread()` and **only** on the
  pumping thread (assert the thread id matches the test thread, never a worker).
- `WaitForAll` blocks until every in-flight job is done.
- `WorkerCount = 0` derives a sane count (≥ 1) from `hardware_concurrency`.
- Stress: many small jobs across the pool, no data races (run under TSan locally
  if available; not a CI gate — veng has no hosted pipeline).

## Dependencies

None — independent of 01 and of the GPU plans. Pure CPU subsystem.

## Acceptance

- Clean build, `ctest` green (the new `tasksystem` unit cases included), smoke
  binary writes a correct-sized PPM.
- `Application` owns a `TaskSystem` (constructed beside the `Context`) and calls
  `PumpMainThread()` at the top of `Frame()` (before `BeginFrame`). No consumer yet
  — wiring it in here keeps plans 07/08 to just the GPU/asset work.

## Notes

- **`Result<T>`, not exceptions**, end to end — consistent with `Result.h`.
- **Ownership:** the `TaskSystem` is a single-owner subsystem on `Application`
  (a plain member or `Unique`), not a `Ref` — nothing shares it; it is threaded by
  reference into consumers (`TaskSystem&`), like the `Context`.
- **Destruction order in `Application` is exact.** Today's teardown is
  `WaitIdle → OnDispose → ImGui.reset → AssetManager.reset → DisposeResources →
  Dispose`. Insert: `WaitForAll()` **before `OnDispose`** (so continuations that
  hand resources to the app have all run), and destroy/join the `TaskSystem`
  **after `AssetManager.reset()`** (a pending load's worker holds a `Context&` and
  touches `AssetManager` state, so neither may be torn down while a worker is live).
</content>
