# planset-6 — threading / task system (async loads)

**Phase goal:** lift veng's single-threaded v1 upload contract — give the engine a
**task/job system**, a **dedicated transfer queue**, and the Vulkan-queue-correct
machinery (per-thread command pools, queue-family ownership transfer, timeline
semaphores) to **decode + upload an asset without stalling the frame**. By the
end, `Buffer/Image::Upload` and `AssetManager::Load` are **async by default** and
the hello-triangle sample loads at least one asset off the main thread, the frame
never blocking on `WaitIdle`.

This is **future [area 2](../future/README.md) — the threading / task system**,
taken up as its own planset. It also delivers **area 1's remaining async half**:
the non-blocking `Load` over a transfer queue that planset-5 deliberately named
`LoadSync` to leave room for. The design vision is
[threading-task-system.md](../future/threading-task-system.md); the async-load
end-state it serves is [asset-system.md](../future/asset-system.md). This planset
builds the foundation those describe.

## The decisions that shape this planset

Resolved up front (they change the structure of every plan):

1. **A task/job system, not raw threads and not a task graph.** A fixed thread
   pool draining a work queue and returning `Task<T>` futures. It matches the
   workload (many small, independent decode+upload jobs), gives **one** place to
   enforce the Vulkan threading rules (per-thread pools), and is a far smaller
   surface than a dependency graph. The `Task<T>` handle is designed so a task
   *graph* (inter-job dependencies) can be layered on later without breaking
   callers — but no graph this planset.

2. **No global singleton — owned by `Application`, threaded explicitly.** The
   `TaskSystem` is constructed by `Application` beside the `Context` and threaded
   into every consumer the same way the context now is. This is the planset-4
   lesson applied from the start: a thread-local "current task system" is just a
   global with extra steps.

3. **The render thread stays single.** This phase adds *worker* threads for
   decode+upload, **not** parallel rendering. `Context::BeginFrame`/`EndFrame`,
   draw recording, ImGui, input, and `Time` remain main-thread-only. What changes
   is that resource *creation and upload* may now run on a worker, with the result
   handed to the render thread queue-correctly. The `Veng.h` contract is revised
   to say exactly this: work may run off the main thread *through the task
   system*; direct concurrent veng API calls remain illegal.

4. **Async is the default; sync is the one you ask for.** Today's `Upload` /
   `LoadSync` become the *blocking* paths under the marked-verbose names
   (`UploadSync`, `LoadSync`), and the unmarked `Upload` / `Load` become the
   non-stalling ones. The obvious call is the right one. This is why planset-5
   named its blocking loader `LoadSync` — that name survives unchanged, and gains
   an async `Load` sibling.

5. **MoltenVK's single-queue collapse is the *tested* path.** On the primary dev
   platform there is effectively one queue family, so `TransferFamily` falls back
   to the graphics family and the cross-queue ownership transfer becomes a no-op.
   The async machinery still buys main-thread non-blocking (work moves off the
   main thread; only the cross-queue handoff collapses). The dual-queue discrete
   path is exercised by the **pure barrier-decision unit test** (queue-family-aware
   `DecideBarrier`) and on real multi-family hardware where available — but the
   collapsed path is what every dev-box run actually executes, so it is the one
   the GPU suite asserts on.

> **Queue submission is externally synchronized.** `vkQueueSubmit` to a given
> `VkQueue` is not thread-safe. Even where transfer and graphics share a queue
> (MoltenVK), a worker submitting an upload and the main thread submitting a frame
> must serialize through a single submission lock on `Context`. This is true
> regardless of how many *families* exist and is called out in plans 03/07.

## Scope of this phase

| In scope | Out of scope (later / other phases) |
|---|---|
| A `TaskSystem` (fixed pool + work queue) returning `Task<T>` | A task *graph* / inter-job dependencies / job stealing |
| A dedicated **transfer queue** + per-worker command pools | Multi-threaded *rendering* (the single render thread stays) |
| Queue-family **ownership transfer** for uploaded resources | Async compute |
| **`TimelineSemaphore`** loader↔render sync | Lifting the single-`Context` assumption for *draw* recording |
| Async `Buffer/Image::Upload` + `AssetManager::Load` (async default) | Hot-reload / file watching (see below) |
| Main-thread continuation pump (`Then` / per-frame drain) | A full frame-graph / parallel pass recording |

## Why hot-reload is *not* in this planset

`asset-system.md` lists `Reload(id)` ("re-cook + re-upload, swap in place") as part
of the async end-state. The **re-upload** half is exactly what this planset's async
path enables, but the **re-cook** half conflicts head-on with planset-5's standing
decision that **cooking is offline-only** — `libveng` has no importer, no source
parser, no re-cook path. A runtime `Reload` that re-cooks would reintroduce the
cook-on-demand the whole asset design rejected. Hot-reload therefore needs its own
design (most likely a *dev-only* file watcher that shells out to `vengc` and
re-mounts the archive, not in-process cooking) and is **deferred**, named here so it
isn't half-built. This planset delivers the async *load* and *upload* it would sit
on top of.

## How the upload path changes (the core mechanic)

Today (planset-5), every upload stalls the device:

```
Image::Upload(span)
  → staging = Buffer::Create(...); staging->Upload(span)          // memcpy
  → SubmitImmediateCommands(cmd{ CopyBufferToImage })
      → GraphicsQueue.submit(...); WaitIdle()                     // ← STALL
```

After this planset, async is the default and `WaitIdle` is gone from that path:

```
Upload(tasks, data):                          // capture Ref<Image>, not raw this
  tasks.Submit([=] {
     staging = Buffer::Create(ctx, ...); staging->UploadSync(data);   // worker memcpy (host-visible+coherent, no flush)
     cmd = ctx.GetTransferCommandBuffer(thisWorker);                  // per-worker transfer pool ONLY (never the shared pool)
     cmd.CopyBufferToImage(staging, image);                           // transfer queue
     cmd.ReleaseToQueue(graphicsFamily);                              // ownership release half (no-op if families match)
     image.MarkProducedOn(transferFamily);                           // the barrier rule reads this
     u64 value = ctx.SubmitTransfer(cmd, transferTimeline);          // value allocated UNDER the submit lock, returned (monotonic)
     auto [buf, alloc] = staging->Release();                         // null the wrapper so its dtor does NOT frame-bin retire
     ctx.RetireOnTransfer(buf, alloc, value);                        // raw handle pinned to the timeline value, not the frame fence
  })
  // first graphics-queue use: the render graph emits the acquire half (plan 06)
  // and ctx.AddFrameTransferWait(transferTimeline, value) so the FRAME SUBMIT
  // (not RenderGraph::Execute, which only records) waits before sampling `image`.
```

`SubmitImmediateCommands`'s `WaitIdle()` survives **only** on `UploadSync` — the
blocking path tests and the smoke render still use.

## The end-to-end target (plan 08 acceptance)

```cpp
// in the sample:
void OnInitialize() override
{
    // Async — returns immediately; the handle becomes resident later.
    m_Brick = m_Assets->Load<Material>(AssetId{1003});   // no frame stall while it decodes/uploads
}

void OnRender() override
{
    if (!m_Brick.IsLoaded()) return;                     // not yet resident — skip the draw
    // … bind set 0, bind material, draw mesh …
}
```

No `WaitIdle` on the load path; the frame keeps running while `brick` decodes and
uploads on a worker, lands on the main thread via the continuation pump, and the
draw begins on the first frame it is resident.

## Plans

| # | Plan | Summary | Status |
|---|---|---|---|
| 01 | [`Upload` → `UploadSync` rename](01-upload-sync-rename.md) | Rename the blocking `Buffer/Image::Upload` to `UploadSync` and migrate every caller (incl. `AssetManager` loaders, the sample, tests). Frees the default name. Behaviour-preserving. | done |
| 02 | [`TaskSystem` + `Task<T>`](02-task-system.md) | Fixed-pool work queue returning futures; `Submit`/`WaitForAll`/`ForEachWorker`; main-thread continuation pump. CPU-only, unit-tested, no Vulkan. | done |
| 03 | [Transfer queue + per-worker command pools](03-transfer-queue-pools.md) | `QueueFamilyIndices::TransferFamily` + selection (dedicated-transfer-preferred, MoltenVK collapse); per-worker transfer pools on `Context`; the submission lock. | done |
| 04 | [`TimelineSemaphore`](04-timeline-semaphore.md) | New `Unique` sync primitive beside `Semaphore`/`Fence`; host/queue signal + wait on a monotonic `u64`. | done |
| 05 | [Frame-submit timeline waits + worker-safe retire](05-submit-wait-worker-retire.md) | Open `Context::SubmitFrame` to carry transfer-timeline waits (`AddFrameTransferWait`); make `Retire` mutex-guarded and add a transfer-timeline-keyed retire path (`RetireOnTransfer`). The two `Context` capabilities the async path needs but doesn't have today. | done |
| 06 | [Cross-queue ownership transfer (barrier rule)](06-ownership-transfer-barrier.md) | Extend planset-3's pure `DecideBarrier`/`ScopeFor` to be queue-family-aware (acquire half on first graphics use; no-op when families match). Rule + unit tests only; the emission site lands in 07. | done |
| 07 | [Async `Buffer/Image::Upload` (default)](07-async-upload.md) | Wire 01–06 together: the unmarked `Upload` routes through the task system + transfer queue + timeline, records the release half + producing-family marker + `RetireOnTransfer`; first graphics use acquires + `AddFrameTransferWait`. GPU-suite tested. | proposed |
| 08 | [`AssetManager::Load` async default + sample migration](08-async-load.md) | Async `Load(AssetId)` returns a not-yet-resident handle filled via a `Task`; the main-thread continuation registers into bindless + swaps it into the cache. Sample loads one asset async. | proposed |
| 09 | [Docs + roadmap re-cut + `Veng.h` contract](09-docs-roadmap.md) | Revise the `Veng.h` single-threaded note, `ownership.md` (timeline primitive + transfer-keyed retire), `CLAUDE.md`, and `future/README` / `threading-task-system.md` / `asset-system.md`. | proposed |

## Dependency analysis & implementation order

### The dependency graph

The edges that actually constrain ordering (a plan must land after every plan it
points from):

```
01 rename ────────────────────────────────────────────────┐
                                                           │
02 TaskSystem ──┬──────────► 03 transfer queue/pools ──────┤
                │                                          ├─► 07 async Upload ─┬─► 08 async Load ─► 09 docs
                └──────────────────────────────────────────┼───────────────────┘
                                                           │
04 TimelineSemaphore ───────► 05 submit-wait + retire ─────┤
                                                           │
06 barrier rule ───────────────────────────────────────────┘
```

Edge list: `02→03`, `04→05`, `{01,02,03,04,05,06}→07`, `{02,07}→08`, `08→09`
(and 09 documents all of 01–08). Two edges the prose flags but the old diagram
hid: **03 depends on 02** (`ForEachWorker` creates the per-worker pools), and
**08 depends on 02 directly** (the `AssetManager` ctor takes `TaskSystem&` and the
continuation pump), not only transitively through 07.

### Roots, waves, and the critical path

- **Roots (no dependencies): 01, 02, 04, 06.** These can start immediately and in
  any relative order — they touch disjoint files (the rename sweep, a new CPU
  subsystem, a new sync primitive, the pure barrier rule).
- **The DAG resolves into five waves** — if plans were delegated concurrently this
  is the dependency-imposed shape, and even run one-at-a-time it is the order:

  | Wave | Plans | Why now |
  |---|---|---|
  | 1 | **01, 02, 04, 06** | no dependencies |
  | 2 | **03** (needs 02), **05** (needs 04) | their single root landed |
  | 3 | **07** | the join — needs 01·02·03·04·05·06 |
  | 4 | **08** | needs 07 (+ 02) |
  | 5 | **09** | documents 01–08 |

- **Critical path = 5 plans.** Two chains tie for longest: `02→03→07→08→09` and
  `04→05→07→08→09`. **07, 08, 09 are the unavoidable serial tail** — every long
  path runs through them, so no amount of parallelism shortens the back half. The
  front (01, 02, 04, 06) is where concurrency, if any, pays off.

### Recommended order

**`01 → 02 → 04 → 06 → 03 → 05 → 07 → 08 → 09`** — a topological sort that front-
loads the four roots (cheapest + most delegatable first), then the wave-2 pair,
then the serial tail. (The plain numeric order `01…09` is *also* a valid
topological sort, since 03 follows 02 and 05 follows 04; this ordering only pulls
the two independent roots 04 and 06 ahead of 03 so the timeline chain `04→05` and
the upload chain `02→03` can advance in parallel when delegating.) Land the roots
before anything that consumes them, then converge on 07.

### Risk & delegation

- **05 and 07 carry the Vulkan-correctness risk.** 05 makes the retire path
  thread-safe + transfer-lifetime-correct (a worker-dropped staging buffer freed
  on the wrong fence is a GPU use-after-free) and opens the frame submit to
  timeline waits; 07 composes the upload (a resource on the wrong queue, or sampled
  before its timeline value, is a GPU race). The smoke render misses both —
  **main thread, validation-verified under `VE_DEBUG`.**
- **08 is the largest plan.** Before any `TaskSystem` wiring it must carve a
  worker/finalize seam into loaders that today fuse decode + upload +
  bindless-registration + pipeline-build in one synchronous constructor/`Load`
  call. **09** is roadmap-only.
- **Delegatable to `model: sonnet`** (API/design reviewed on the main thread):
  **01** (mechanical rename sweep — but it edits the sample and tests, so land it
  before the freed name is depended on), **02** (self-contained CPU subsystem with
  unit tests), and **03 / 04 / 06** (each isolated in its own area: `Context` queue
  selection / a new primitive / the pure barrier rule). **Keep 05 / 07 / 08 on the
  main thread** — the queue-correctness, the deferred-destruction reroute, and the
  async-cache design.

## New dependencies

**None.** Everything here is the C++ standard library (`std::thread`,
`std::mutex`, `std::condition_variable`, `std::future`/promise machinery — or a
hand-rolled equivalent) plus Vulkan features veng already requires (timeline
semaphores are core in Vulkan 1.2 / `VK_KHR_timeline_semaphore`; confirm MoltenVK
support and enable the feature in `Context` device creation if not already on).

## Validation discipline

Plans that touch the GPU / device path (**03, 04, 05, 07, 08**) must pass the `VE_DEBUG`
validation gate — run the relevant binary from `build-debug/` and confirm no new
`Vulkan validation` ERROR line, and run `ctest -L validation` (the
`validation_gate` over the `gpu`-labelled band). The async upload path is precisely
where a missing queue-ownership barrier or an under-synchronized timeline wait
produces a validation error that the smoke render alone would not catch. **No plan
may widen the validation allowlist** (currently empty); plan 07 should add an
async-upload case to the `veng_gpu` suite so the gate covers it.

## Out of scope (named, so it isn't half-built)

- **Hot-reload / file watching** — its re-cook half conflicts with offline-only
  cooking; needs its own design (see above). `Reload(id)` is not implemented.
- **A task graph / inter-job dependencies / job stealing** — `Task<T>` is shaped to
  allow a graph later; none is built. The pool runs independent jobs.
- **Multi-threaded rendering / parallel pass recording** — the render thread stays
  single. Only resource *creation + upload* moves off-thread.
- **Async compute** — transfer + graphics only.
- **Staging-buffer pooling** — per-upload create/destroy (the deferred-destruction
  queue already makes it safe, just wasteful). Pool when profiling says so; an open
  decision, not built here.
- **Task cancellation** — dropping a `Task<T>` does **not** cancel the job (v1:
  run-to-completion, result discarded). Revisit if streaming churn demands it.

## Process & conventions

Same cadence as every planset: implement → migrate `examples/hello-triangle` in the
same pass → verify (clean build, `ctest` green, smoke binary writes a correct-sized
PPM ≈ 2,764,816 bytes; for GPU-path plans, a `VE_DEBUG` validation check) → update
this table → one commit per plan, `Plan NN: <summary>` with a `Co-Authored-By`
trailer (`planset-6:` for roadmap-only edits).

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and
> approved; `done` = landed and verified.

## On completion

Closes **future area 2 (threading / task system)** and **area 1's remaining async
half**. Update [future/README.md](../future/README.md): mark area 2 taken up by
planset-6 and area 1 complete (async `Load` lands), leaving **area 4
(events/input)** as the only remaining roadmap area — independent and
gameplay-driven. Re-cut the ordering diagram. Update [plans/README.md](../README.md)
with the planset-6 entry, revise the `Veng.h` single-threaded contract note, and
trim [threading-task-system.md](../future/threading-task-system.md) /
[asset-system.md](../future/asset-system.md) to whatever enduring vision remains
(hot-reload, the editor consumer, staging pooling, a task graph).
</content>
</invoke>
