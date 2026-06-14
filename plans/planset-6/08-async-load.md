# Plan 08 — `AssetManager::Load` async default + sample migration

**Goal:** add the async `Load(AssetId)` that planset-5's `LoadSync` left room for:
it returns a **not-yet-resident** handle immediately, runs decode + GPU upload on
the task system (plan 07's async `Upload`, transfer queue, no frame stall), and on a
**main-thread continuation** registers the resource into bindless and swaps it into
the cache entry. `LoadSync` keeps its name. The hello-triangle sample migrates at
least one load to async to prove the frame never stalls while an asset decodes.

## Why this is its own plan

Plan 07 delivered the async *upload* primitive; this is its first real *consumer*
and the one that closes future area 1's async half. It is also the **largest** plan
— it must first carve a worker/finalize seam into loaders that today fuse
create+upload+register synchronously (work item 0). It is sample-verifiable: the
asset-cache swap (an entry that exists but isn't resident until its continuation
runs) plus **main-thread bindless registration** is a distinct design from the
upload mechanics, and lands cleanly on
top of a proven async upload.

## API

`AssetManager` gains a `TaskSystem&` (the asset-system doc's intended ctor) and an
async `Load`:

```cpp
// constructed with both the context and the task system (today: just Context&):
AssetManager(Renderer::Context& context, TaskSystem& tasks, const AssetManagerInfo& info = {});

// Async: returns immediately; the handle becomes IsLoaded() later.
template <typename T>
AssetHandle<T> Load(AssetId id);

// Blocking (planset-5, unchanged name): Load(id) then block, returning AssetResult<…>.
template <typename T>
AssetResult<AssetHandle<T>> LoadSync(AssetId id);
```

- **`AssetHandle<T>::IsLoaded()` already exists** — it checks `m_Entry->Resource !=
  nullptr` (`AssetHandle.h`), and the cache entry already stores a nullable
  `Resource`. So a *pending* (not-yet-resident) entry needs **no new residency
  machinery**: create the entry with a null `Resource`, hand back a handle, and the
  continuation fills `Resource` in. planset-5's design already anticipated this; the
  consumer just polls `IsLoaded()`.
- **`LoadSync` is preserved** with its `AssetResult<…>` structured-error return.
  Tests, the smoke path, and one-shot tools keep using it.

## The thread split (the crux)

- **Workers** read the cooked blob, decode, create the GPU resource, and record the
  async upload (plan 07). They **never** touch the cache and **never** register into
  the `BindlessRegistry` — both are single-threaded global state.
- **The main-thread continuation** (`Task::Then`, drained by `PumpMainThread`,
  plan 02) does, atomically on the main thread: (1) **register** the resource into
  bindless / write its `MaterialData` SSBO entry, (2) **swap** the resource into the
  cache entry (mark it resident). This is why the continuation pump exists — the
  whole point is that *work* moves off-thread while *engine-state mutation* stays on
  the single render thread.

> **The seam does not exist today — building it is the bulk of this plan.** The
> current loaders **fuse** worker-illegal and main-only work in one synchronous
> call: `Texture`'s constructor (`engine/src/Asset/Texture.cpp`) creates the image,
> calls `Upload`, **and** `bindless.Register(...)`; `Material`'s constructor
> (`Material.cpp`) calls `RegisterMaterial`; `MaterialLoader::Load` synchronously
> `LoadSync`s the shaders/textures, reads each texture's *already-registered*
> bindless index to patch `MaterialData`, and builds a `GraphicsPipeline`. There is
> no existing "create+upload" / "register+finalize" boundary. So async load is **not
> a wiring change** — it requires restructuring the loaders (work item 0) before any
> of the `TaskSystem` plumbing matters. This is the single largest piece of the
> planset and the reason this plan is sequenced last.

## Work

0. **Split the loaders into a worker phase + a main-thread finalize phase (the big
   one).** Restructure each `AssetLoader::Load` and the `Texture` / `Material`
   constructors so the worker-legal work (decode, `Image`/`Buffer::Create`, record
   the async `Upload`) is separated from the main-only finalize (`bindless.Register`
   / `RegisterMaterial`, reading dependency bindless indices to patch `MaterialData`,
   building the `GraphicsPipeline`, the cache swap). Concretely: construction no
   longer registers; a resource is created *unregistered*, and a `Finalize()` step
   run on the continuation does the registration + index patching + pipeline build.
   A material's finalize depends on its textures being **finalized** (registered)
   first, so the dependency fan-out (item 3) must order finalizes, not just loads.
   This is the prerequisite the rest of the plan rests on — scope it explicitly,
   land it first within the plan, and keep `LoadSync` working throughout.
1. **Thread the `TaskSystem`** into `AssetManager` (ctor + `Application` wiring —
   `Application` owns the `TaskSystem` from plan 02; today `AssetManager` is built
   with just `m_RenderContext`).
2. **Async `Load`:** create the cache entry in a pending state (null `Resource`) and
   return a handle immediately. Submit blob-read + decode + resource-create + async
   `Upload` as a task; its `Then` continuation registers into bindless and swaps the
   resource into the entry. **All cache + bindless mutation is in the continuation,
   on the main thread.**
3. **Dependency loads.** A material pulls its textures + shaders; eager async loads
   fan out as sub-tasks, and the material's entry becomes resident only when all
   dependencies are resident (and registered). `MissingDependency` stays a
   first-class `AssetError` (a failed dependency fails the parent's `Result`).
4. **`LoadSync` without self-deadlock — and without returning before the upload
   completes.** Two hazards:
   - *Deadlock:* `LoadSync` must **not** be a naive `Load(id).Get()` — `Get()` blocks
     the main thread, but the swap + registration are scheduled onto the main-thread
     continuation queue that only `PumpMainThread` drains, which the blocked thread
     is no longer pumping. So `LoadSync` does the finalize **inline on return**,
     bypassing the continuation queue (it is already on the main thread and
     blocking). The continuation queue is for the *async* path only.
   - *Premature residency:* the GPU upload must be **complete** before `LoadSync`
     registers into bindless and returns a resident handle. The async `Upload` only
     *submits* and relies on a later frame's `AddFrameTransferWait` — but `LoadSync`
     blocks before any frame submits, so nothing would wait the timeline and the
     first draw samples a half-uploaded image. Therefore `LoadSync` must **either**
     route through `UploadSync` (blocking `WaitIdle`, the simplest correct choice)
     **or** host-wait the upload's `transferTimeline` value (`timeline.Wait(value)`)
     before finalizing. Do **not** leave it on the bare async `Upload`.
5. **`CollectGarbage` / eviction** unchanged from planset-5 — deferred through the
   per-frame retire queue. A pending (not-yet-resident) entry is **not** evicted.

## Sample migration

In `examples/hello-triangle`, change one load (e.g. the material) from `LoadSync`
to `Load`, gate the draw on `IsLoaded()`, and confirm the frame runs while it loads:

```cpp
void OnInitialize() override { m_Brick = m_Assets->Load<Material>(AssetId{1003}); }
void OnRender()     override { if (!m_Brick.IsLoaded()) return; /* … draw … */ }
```

The headless smoke path may keep `LoadSync` (it renders one frame and exits — async
buys it nothing) or exercise `Load` + a pump loop until resident; either is valid,
but keep the smoke's deterministic-sized PPM contract.

## Dependencies

Plan 07 (async `Upload`) and plan 02 (`TaskSystem` + the continuation pump). Builds
directly on planset-5's `AssetManager`/`AssetHandle`/`LoadSync`.

## Acceptance

- Clean build, `ctest` green, smoke binary writes a correct-sized PPM.
- **Validation-verified:** the async load path produces no new `Vulkan validation`
  ERROR from `build-debug/`; `ctest -L validation` green. (Bindless registration on
  the main thread is part of what keeps this clean.)
- The sample loads an asset via `Load` (async), gates its draw on `IsLoaded()`, and
  the frame does **not** stall on the load — confirm no `WaitIdle` on this path.
- `LoadSync` still works, still returns `AssetResult<…>`, and does **not** deadlock
  (regression guard — exercise it after the async path exists).

## Notes

- **The cache and the bindless registry are main-thread-only.** Workers decode and
  record uploads; the swap *and the registration* are the continuation. This keeps
  the single-render-thread contract intact while the *work* moves off-thread.
- Hot-reload (`Reload`) is **not** added here (README out-of-scope: its re-cook half
  conflicts with offline-only cooking). The in-place-swap mechanism this plan builds
  (resource swapped behind a stable handle) is exactly what a future hot-reload
  would reuse.
</content>
