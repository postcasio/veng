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

- **[future](future/README.md)** — work beyond the current plansets (📝 draft/vision,
  holding area; not a planset). Remaining areas: a threading/task system
  (Vulkan-queue-correct async asset loading — which also turns planset-5's
  `LoadSync` into the async default), the **editor application** (a shared-library
  game-module model + a cooker-consuming editor with docking, a material node
  editor, and a scene editor — [editor.md](future/editor.md) /
  [game-module.md](future/game-module.md), several plansets), its prerequisite
  scene/entity model, and the event/input systems. Each becomes its own planset
  when taken up. (Testing areas 5a/5b, de-globalizing the context
  (area 3), and the asset system's synchronous slice + bindless (area 1) are done
  — planset-3, planset-4, and planset-5 respectively; the asset system's async
  half folds into the threading area.)
