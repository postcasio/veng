# Threading / task system — design overview (future)

> **Vision / design sketch, not scheduled.** Detail for [area 2](README.md) of
> the future roadmap. Direction, API surfaces, and decisions — not a firm plan;
> it becomes its own planset when taken up. Designed against the explicit-device
> API now that [de-globalizing the context](README.md#3-de-globalize-the-rendering-context--done-planset-4)
> (planset-4) is done — `Context::Instance()` is gone, so off-thread resource
> creation is finally *possible*; this phase makes it *correct*.

## Why

veng v1 is **single-threaded by design** (the contract in `Veng.h`): the
`Context` is constructed by `Application` and threaded into every resource;
`Time`, input, and ImGui all assume one driving thread. Today every GPU upload is
synchronous and main-thread-blocking:

```
Image::Upload(span)
  → stagingBuffer = Buffer::Create(...); stagingBuffer->Upload(span)   // memcpy
  → ImmediateCommands([&](cmd){ cmd.CopyBufferToImage(...); })
      → SubmitImmediateCommands(cmd)
          → GraphicsQueue.submit(...); WaitIdle()                      // ← STALL
```

`Context::SubmitImmediateCommands` (`src/Renderer/Backend/Context.cpp`) submits on
the **graphics queue** and `WaitIdle()`s the whole device. Loading one texture
mid-frame stalls the GPU. That is the thing this phase removes: **decode + upload
an asset without stalling the frame.**

This system is the prerequisite for the headline [asset system](asset-system.md)
(area 1) — async loading is the payoff — but the two are separable: the asset API
and *synchronous* loading can land first (see the cross-cutting "design against a
real client" note), with this layer turning those loads non-blocking afterward.

## Scope of this phase

| In scope | Out of scope (later / other phases) |
|---|---|
| A task/job abstraction for off-main-thread work | A full frame-graph / parallel command recording across passes |
| A dedicated **transfer queue** + per-thread command pools | Multi-threaded *rendering* (v1's single render thread stays) |
| Queue-family **ownership transfer** for uploaded resources | Job stealing / fancy scheduler tuning |
| Timeline-semaphore sync between loader and render | Lifting the single-`Context` assumption for *draw* recording |
| A non-blocking `Buffer/Image::Upload` (async by default) | Async compute |

**The render thread stays single.** This phase adds *worker* threads for
decode+upload, not parallel rendering. `Context::BeginFrame`/`EndFrame`,
command recording for draws, ImGui, input, and `Time` remain main-thread-only.
What changes is that resource *creation and upload* may now happen on a worker,
and the result is handed to the render thread queue-correctly.

## Design axis 1 — the concurrency model

The open call (README area 2): **threads vs. a task/job system vs. a task graph.**
Recommendation, to be confirmed when the planset opens:

> A **task/job system** (a fixed thread pool + a work queue returning futures),
> not raw `std::thread` per load and not a full task graph yet. A thread pool
> matches the workload (many small, independent decode+upload jobs), gives a
> single place to enforce the Vulkan threading rules (per-thread pools), and is a
> smaller surface than a dependency graph. A task *graph* (inter-job
> dependencies, a scheduler) is the natural follow-on once gameplay/streaming
> needs it — design the job handle so a graph can be layered on without breaking
> callers.

### `TaskSystem`

Owned by the `Application` (alongside the `Context`), threaded explicitly the
same way the context now is — **no global singleton** (the lesson of planset-4).

```cpp
namespace Veng
{
    struct TaskSystemInfo
    {
        // 0 = derive from hardware_concurrency() - 1 (leave the main thread a core).
        u32 WorkerCount = 0;
    };

    template <typename T>
    class Task;                       // a handle to an in-flight result (below)

    class TaskSystem
    {
    public:
        explicit TaskSystem(const TaskSystemInfo& info);
        ~TaskSystem();                // joins all workers

        // Enqueue work onto the pool. Returns a Task<T> the caller polls or waits.
        template <typename Fn>
        auto Submit(Fn&& fn) -> Task<std::invoke_result_t<Fn>>;

        // Run a function on each worker once — used to set up per-thread state
        // (the transfer command pools below). Called once at init.
        void ForEachWorker(const std::function<void(u32 workerIndex)>& fn);

        [[nodiscard]] u32 GetWorkerCount() const;

        // Drain: block until the queue is empty and all in-flight tasks finish.
        // For OnDispose() ordering — assets must finish loading before teardown.
        void WaitForAll();
    };
}
```

### `Task<T>` — the result handle

A future-like handle. Deliberately **poll-first** (a game loop polls; it does not
block) with an opt-in blocking wait for teardown/tests.

```cpp
template <typename T>
class Task
{
public:
    [[nodiscard]] bool IsReady() const;          // non-blocking
    [[nodiscard]] Result<T> Get();               // blocks until ready, then moves out
    void Then(std::function<void(Result<T>)>);   // main-thread continuation (see below)
};
```

- `Result<T>` (not exceptions) is the error channel — consistent with the engine
  contract. Asset loads fail recoverably (file missing, corrupt), so the worker
  returns `Result<T>` and the handle carries it through.
- **`Then` runs on the main thread.** Continuations that touch engine state
  (registering a finished texture, swapping in a mesh) must not run on a worker.
  The `TaskSystem` keeps a main-thread continuation queue drained once per frame
  by `Application` (a new `TaskSystem::PumpMainThread()` call near `BeginFrame`).
  This is how an async load "lands" safely on the single render thread.

## Design axis 2 — Vulkan-queue correctness (the hard part)

The README is explicit that this must be "done correctly, not bolted on." Four
concrete pieces, all touching `Context` (`src/Renderer/Backend/Context.cpp`):

### 1. A dedicated transfer queue

Today `QueueFamilyIndices` (in `Context.h`) tracks only `GraphicsFamily` /
`PresentFamily`. Add a **transfer** family and queue:

```cpp
struct QueueFamilyIndices
{
    optional<u32> GraphicsFamily;
    optional<u32> PresentFamily;
    optional<u32> TransferFamily;   // NEW — prefer a dedicated transfer-only family
    // ...
};
```

- Selection: prefer a queue family with `eTransfer` **but not** `eGraphics`
  (discrete DMA path on desktop). On MoltenVK there is effectively one family, so
  `TransferFamily` falls back to the graphics family — the code path must handle
  "transfer == graphics" (no ownership transfer needed, single queue) as a
  first-class case, since that is the **primary dev platform**. The async
  machinery still buys main-thread non-blocking even with a shared queue (work
  moves off the main thread; only the cross-queue handoff collapses).

### 2. Per-thread command pools

`VkCommandPool` is **not shareable across threads** — the single hardest Vulkan
rule this phase enforces. Each worker gets its own transfer command pool, created
via `TaskSystem::ForEachWorker` at init and stored thread-local (or indexed by
worker id inside `Context`). Recording an upload command buffer always uses
*this worker's* pool.

```cpp
// On Context, keyed by worker index (set up once via ForEachWorker):
CommandBuffer& Context::GetTransferCommandBuffer(u32 workerIndex);
```

### 3. Queue-family ownership transfer

A resource uploaded on the transfer queue and later sampled on the graphics queue
needs a **queue-family ownership transfer** (a release barrier on the transfer
side, an acquire barrier on the graphics side) — unless the two families are the
same (MoltenVK), where it is a no-op. This is the piece most easily gotten wrong.

The natural home is the render graph, which **already derives barriers from
declared use** (`docs` / `RenderGraph.h`). Extend the barrier-decision rule
(planset-3 extracted `DecideBarrier`/`ScopeFor` as a pure function — reuse it) so
that a view whose backing resource was last touched on the transfer queue emits
the acquire half on first graphics-queue use. The release half is recorded by the
upload job. Keep the two halves' `srcQueueFamilyIndex`/`dstQueueFamilyIndex`
paired and skip both when families match.

### 4. Timeline-semaphore sync

Loader↔render synchronisation should use a **timeline semaphore**, not fences or
binary semaphores: the loader signals a monotonically increasing value as uploads
complete; the render thread waits the value a resource depends on. This composes
far better than the per-frame binary `Semaphore`s veng has today
(`Renderer/Semaphore.h`) and avoids a fence per upload.

```cpp
class TimelineSemaphore                    // NEW — sits beside Semaphore/Fence
{
public:
    static Unique<TimelineSemaphore> Create(Context& context, u64 initialValue = 0);
    void  Signal(u64 value);               // host signal
    void  Wait(u64 value) const;           // host wait
    [[nodiscard]] u64 GetValue() const;
    // queue-side wait/signal go through the submit info, like today's semaphores
};
```

Ownership: `Unique` — single-owner sync primitive, per the ownership rule (which
already lists `Semaphore`/`Fence` as `Unique`).

## How the upload path changes

**Async is the default; sync is the one you have to ask for.** Today's
`Upload` *becomes* the async path, routing through the task system — the
unmarked name is the non-stalling one, so a caller who reaches for the obvious
method gets the right behaviour. The blocking variant survives but is renamed
`UploadSync`, deliberately the more verbose spelling, for the simple cases that
genuinely want to block (tests, the smoke path, a one-shot tool). The resource's
deferred-destruction back-reference (`m_Context`, planset-4) is untouched —
async only changes *when* and *on which queue* the copy is recorded.

```cpp
// Image.h — async is the default Upload; the blocking path is the marked one:
[[nodiscard]] Task<void> Image::Upload(TaskSystem& tasks, std::span<const u8> data);
void                     Image::UploadSync(std::span<const u8> data);   // blocks (WaitIdle)
```

> **Migration note.** This is a breaking rename of today's synchronous
> `Buffer/Image::Upload` → `UploadSync`. Every current caller (incl.
> `Image::Upload`'s own staging-buffer fill in `Backend/Image.cpp`, and the
> hello-triangle sample) becomes `UploadSync` in the same pass — then the new
> async `Upload` is added. Doing the rename first keeps the default name free for
> the behaviour we want callers to land on.

Internally the async `Upload`:

```
Upload(tasks, data):
  tasks.Submit([=] {
     staging = Buffer::Create(ctx, ...); staging->UploadSync(data);  // worker: memcpy (host-visible, no GPU wait)
     cmd = ctx.GetTransferCommandBuffer(thisWorker);              // per-thread pool
     cmd.CopyBufferToImage(staging, image);                       // transfer queue
     cmd.ReleaseToQueue(graphicsFamily);                          // ownership release
     transferTimeline.SignalOnSubmit(value);                      // timeline signal
     return value;                                                // render waits this
  })
  // first graphics-queue use of `image`: render graph emits the acquire barrier
  // + waits transferTimeline >= value before the pass that samples it.
```

`SubmitImmediateCommands`'s `WaitIdle()` is **not** used on this path — that is the
whole point. `UploadSync` keeps it.

## Touch points (what this phase modifies)

- `Context` (`Context.h` + `Backend/Context.cpp`): transfer queue selection,
  per-worker transfer pools, timeline-semaphore plumbing, an async submit that
  does **not** `WaitIdle`.
- `QueueFamilyIndices`: add `TransferFamily` + selection logic.
- `Buffer` / `Image`: async `Upload` becomes the default; the old blocking
  `Upload` is renamed `UploadSync` (breaking, callers migrated in the same pass).
  No change to ownership or the `m_Context` retire back-ref.
- Render graph / barrier rule (planset-3's extracted `DecideBarrier`): the
  acquire half of cross-queue ownership transfer on first graphics use.
- New: `TaskSystem`, `Task<T>`, `TimelineSemaphore`.
- `Application`: owns the `TaskSystem`, pumps main-thread continuations once per
  frame, drains it in `OnDispose` ordering (before the context tears down).
- `Veng.h`: revise the single-threaded v1 contract note — work may now run off
  the main thread *through the task system*; direct concurrent veng API calls
  remain illegal.

## Usage sketch (the consumer's view)

```cpp
void OnInitialize() override
{
    // Application owns the TaskSystem; assets load against it.
    auto handle = m_Assets->LoadAsync<Texture>(tasks, "textures/brick.ktx2");
    m_PendingBrick = std::move(handle);          // a Task<Ref<Texture>>
}

void OnUpdate(f32) override
{
    if (m_PendingBrick.IsReady())
        m_Brick = m_PendingBrick.Get().value();  // lands on the main thread
}
// no frame stalls while brick.ktx2 decodes + uploads on a worker.
```

## Open decisions

- **Thread pool vs. task graph** — recommend pool now, graph-able later (above).
- **Continuation model** — `Then` main-thread queue vs. polling `IsReady` only.
  Recommend supporting both; polling is the floor, `Then` the ergonomic path.
- **Transfer == graphics fallback** (MoltenVK): collapse to one queue with no
  ownership transfer — confirm this is the *tested* path since it is the dev
  platform, with the discrete dual-queue path exercised where hardware allows.
- **Staging buffer pooling** — a per-worker ring of reusable staging buffers vs.
  create/destroy per upload (the deferred-destruction queue already makes
  per-upload safe, just wasteful). Pool when profiling says so.
- **Where the transfer pools live** — `thread_local` vs. a worker-indexed array on
  `Context`. Indexed-on-`Context` keeps lifetime tied to the device (cleaner
  teardown) and avoids `thread_local` + dynamic worker counts.
- **Cancellation** — does dropping a `Task<T>` cancel the load? v1: no
  (run-to-completion, result discarded); revisit if streaming churn demands it.
