# veng plans

Plans are grouped into numbered **plansets**, each a coherent phase of work.

- **[planset-1](planset-1/README.md)** — API rework / insulation (✅ complete,
  2026-06). Ownership, error handling, deferred destruction, vocabulary enums,
  public/backend split, render graph, ImGui module, headless context, typed
  buffers. (Plan 12, runtime shader reflection, was intentionally *not* built —
  superseded by planset-2, see below.)

- **[planset-2](planset-2/README.md)** — rendering API surface cleanup
  (✅ done, 6 plans). Push-constant layout/buffer, attachment formats from render targets,
  retiring the legacy render-pass path, and minor ergonomics. Shader-facing work
  is deferred to the future-work areas below.

- **[planset-3](planset-3/README.md)** — unit testing & test infrastructure
  (✅ done, 6 plans). Delivered the first half of the future "testing" area (area
  5a): a header-only framework (doctest), CTest wiring, a death-test harness, and a
  base suite — pure-logic, type-mapping round-trips, an extracted barrier-decision
  rule + tests, death tests, and a consolidated GPU one-exe band that skips with no
  ICD. The in-process multi-case GPU integration suite (5b) and CI stay in
  `future/`, deferred until after the de-globalize change.

- **[planset-4](planset-4/README.md)** — de-globalize the context, then finish
  testing (✅ done, 6 plans). Removed the `Context::Instance()` singleton (future
  area 3) by threading an explicit `Context&` into every resource `Create`, then —
  on the explicit-device API — delivered the deferred testing work: the in-process
  multi-case GPU integration suite (area 5b, `veng_gpu`) and a local
  `ctest -L validation` gate over the documented validation-error allowlist
  (CI with a hosted software-ICD pipeline was explicitly descoped — local only).
  Order was `5a → 3 → 5b`, as the future roadmap fixed. Stays
  single-threaded/single-context; threading (area 2) is a later planset.

- **[planset-5](planset-5/README.md)** — the synchronous asset system (✅ complete,
  2026-06). Takes up future area 1's **synchronous slice**: a per-lib project
  reorg, a standalone in-repo **cooker** (`vengc`) that turns hand-written JSON
  **asset packs** into binary **archives**, a shared `assetformat` lib, and an
  engine-side `AssetManager` that loads by opaque `u64` `AssetId` via `LoadSync`.
  Delivers the **bindless** descriptor subsystem (set 0 bound once per frame) then
  texture (stb), mesh (assimp), shader (**Slang** + offline reflection), and a thin
  handle-based material on top of it, ending with hello-triangle rendering a cooked
  pack. Cooking is offline-only (no cook-on-demand); async loading (threading) is
  the named follow-on, not in scope.

- **[planset-6](planset-6/README.md)** — threading / task system, async loads
  (✅ done, 9 plans). Takes up future area 2 and closes area 1's remaining async
  half: a `TaskSystem` (fixed worker pool + work queue returning `Task<T>`, owned
  by `Application` and threaded explicitly, pumped once per frame), a dedicated
  **transfer queue** with per-worker command pools, a `TimelineSemaphore`
  primitive, queue-family-aware ownership transfer (the `DecideBarrier` rule
  extended to emit the acquire half on first graphics use), and a transfer-keyed,
  mutex-guarded retire path (`RetireOnTransfer`) for worker-dropped staging.
  `Buffer/Image::Upload` and `AssetManager::Load` become **async by default**;
  the blocking paths survive as `UploadSync`/`LoadSync`. The `Veng.h` contract is
  revised: the render thread stays single, but work runs off it through the task
  system. Hot-reload stays future (its re-cook half conflicts with offline-only
  cooking). MoltenVK's single-queue collapse is the tested path; the dual-queue
  discrete path is exercised by the pure barrier-decision unit test.

- **[future](future/README.md)** — work beyond the current plansets (📝 draft/vision,
  holding area; not a planset). Remaining areas: the **editor application** (a
  shared-library game-module model + a cooker-consuming editor with docking, a
  material node editor, and a scene editor — [editor.md](future/editor.md) /
  [game-module.md](future/game-module.md), several plansets), its prerequisite
  scene/entity model, and the event/input systems. Each becomes its own planset
  when taken up. (Testing areas 5a/5b, de-globalizing the context (area 3), the
  asset system's synchronous slice + bindless (area 1), and the threading/task
  system (area 2 — which also turned area 1's `LoadSync` into the async `Load`
  default) are done — planset-3, planset-4, planset-5, and planset-6
  respectively. Hot-reload remains future: its re-cook half conflicts with
  offline-only cooking and needs a dev-only watcher design.)
