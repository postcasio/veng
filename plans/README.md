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

- **[planset-7](planset-7/README.md)** — runtime primitive meshes (✅ done, 4 plans).
  A small, self-contained utility planset (not part of any future-area chain). First
  fixes the mesh's runtime material model — a `SubMesh::MaterialIndex` into a resident
  `vector<AssetHandle<Material>>` the `Mesh` owns, with `MeshLoader` eager-resolving
  cooked submesh ids into that list (superseding planset-5's "the mesh does not load
  its materials" rule). Then adds public CPU geometry (`CanonicalVertex` + `MeshData`)
  and a `Mesh::Create(Context&, const MeshData&, const string&)` upload factory
  (blocking `UploadSync`), so a runtime primitive and a cooked mesh are interchangeable
  to every pipeline and draw call. `Primitives::Cube`/`Plane`/`Sphere` generate
  `MeshData` with analytic normals/tangents/UVs and an optional material instance; the
  hello-triangle sample draws a runtime sphere carrying the brick material, no cooked
  mesh required to put geometry on screen. A runtime primitive is not an
  `AssetId`-addressable asset and never touches an archive; custom vertex layouts
  end-to-end stay future.

- **[planset-8](planset-8/README.md)** — compiled `RenderGraph` (✅ done, 4 plans).
  Takes up future area 9: moves `RenderGraph` from immediate-mode (a fresh vector of
  pass structs rebuilt every frame, every barrier re-derived per `Execute`) to a
  **compiled** graph. Splits the resource model — graph-owned **transients**
  (logical `ResourceId` handles the graph allocates and resolves per frame) vs.
  late-bound **imports** (external concrete views supplied per frame to `Execute`) —
  and replaces the bare `CommandBuffer&` callback with a typed **`PassContext`**
  (`Cmd()` + `Resolved(ResourceId)`), the record-time channel aliasing requires.
  `RenderGraph` becomes a pure builder whose **`Compile()`** bakes the
  barrier/transition schedule, transient allocation, per-graphics-pass
  `RenderingInfo`, and one-time validation into a `CompiledGraph` that **replays**
  per frame; the consumer re-`Compile()`s only on a structural change (the explicit
  recompile seam, no internal dirty flag). Transient **aliasing** lands behind a
  pure, device-free, unit-tested live-range rule (mirroring `DecideBarrier`), so
  non-overlapping transients share backing. Builds only on the shipped `RenderGraph`
  and is the enabling prerequisite for the scene renderer (area 8).

- **[planset-9](planset-9/README.md)** — game-module build model, pipeline cache
  & archive hashes (✅ done, 7 plans). Bundles three independent shipping-hygiene
  streams. **(A) Game-module build model** — a game stops being a self-contained
  exe and becomes `libhello_triangle`-style `libgame` (shared, the runtime) + a
  thin **launcher** (the shipped exe) that `dlopen`s it through one C-ABI
  `VengModuleRegister(VengModuleHost*)` entry, into which the module registers its
  `Application` factory (`ApplicationRegistry`); a `VengModuleAbiVersion` handshake
  rejects a stale module at load, and `veng_add_game` emits the lib + launcher as a
  **relocatable trio** (module resolved beside the launcher via `$ORIGIN`/
  `@loader_path`, pack + assets via `ExecutableDirectory()`). `hello-triangle` ships
  as `libhello_triangle` + a launcher and the smoke runs through it. Stream A is
  **future area 6's first sub-area** (the editor's prerequisite); its
  type-reflection layer is **deferred to the editor-shell planset**, designed
  against the inspector, and `libgame_editor`/`EditorRegistry` stay future (the ABI
  carries a reserved-null `EditorRegistry*` for them). **(B) Pipeline cache** — a
  context-owned `vk::PipelineCache` reused across every pipeline build, with opt-in
  disk persistence via `ApplicationInfo::PipelineCachePath`. **(C) Archive content
  hashes** — `.vengpack` format v2 carries a content hash per cooked blob + a
  table-of-contents digest, cooker-written (xxh3-128) and `vengc verify`-checked;
  the loader never verifies and `assetformat`/`libveng` gain no hash dependency. B
  and C each resolve a **cross-cutting concern** (pipeline caching; content hashes)
  from [future/README.md](future/README.md).

- **[planset-10](planset-10/README.md)** — scene / entity model (✅ done, 5 plans).
  Takes up future area 7's **runtime** half: a hand-rolled sparse-set ECS
  (`Scene`/`Entity`, type-erased components, queries), a reflection layer (one stable
  `TypeId` space authored like `AssetId`, `FieldClass`, `VE_REFLECT` describe-blocks
  with editor metadata, a tolerant name-keyed serializer), a transform hierarchy, a
  `Camera`, and **game-defined component types**. The cooked `.scene` asset, a systems
  framework, and the module-ABI `TypeRegistry&` registration seam are held back — the
  cooked scene and seam to area 10 (the prioritized next planset).

- **[planset-11](planset-11/README.md)** — cooker-side module reflection + the
  cooked prefab asset (✅ done, 5 plans). Takes up future area 10. The cooker
  `dlopen`s `libgame` to **reflect its native component types** — realizing the
  additive `VengModuleHost` `TypeRegistry&` seam (ABI `1u→2u`), with the registry
  now **host-owned** (the launcher/cooker constructs it, pre-registers the builtins
  via a GPU-free `RegisterBuiltinTypes`, fills it through `VengModuleRegister`, and
  threads it into the `Application`, which borrows a `TypeRegistry&`). On that, a
  **cooked prefab asset**: a `*.prefab.json` (entities + components + values) cooks
  into an `AssetType::Prefab` blob — **validated against the reflected descriptors**,
  the way materials are validated against shader reflection — that loads through the
  same `AssetManager::Load` path as every asset (a cached `AssetHandle<Prefab>`) and
  **spawns** its entities into a mutable `Scene` (`Prefab::SpawnInto`, entity
  references remapped, `AssetHandle` fields rehydrated). The cooked blob reuses
  planset-10's name-keyed `WriteFields` record encoding; the prefab-cooking path
  links `veng::veng` and reuses `ModuleLoader` (the one scoped relaxation of the
  Vulkan-free cooker). hello-triangle ships a cooked prefab it loads + spawns
  instead of building its world in code. **Supersedes planset-10 decision 4** — the
  `TypeRegistry` is now host-owned, not an `Application` member. Cross-compiled
  cooking (host ≠ target) stays out.

- **[planset-12](planset-12/README.md)** — the `SceneRenderer`, a deferred
  über-pipeline on `RenderGraph` (✅ done, 5 plans). Takes up future area 8. A
  long-lived, configurable **`SceneRenderer`** (`Unique`, single-owner) that owns an
  offscreen target and renders a `Scene` from a `Camera` through an **internal
  compiled `RenderGraph`** of reusable, self-contained **`ScenePass`** units, handing
  back a sampleable result. Its surface is the **lifetime split** —
  `Create`/`Resize`/`Configure`/`Execute`/`GetOutput` — where `Configure`/`Resize`
  rebuild + re-`Compile()` and `Execute` only replays; the per-frame `SceneView`
  reaches passes through an **opaque user-pointer** channel on `PassContext` (so
  `RenderGraph` stays scene-agnostic). On that shell it delivers a **minimal deferred
  spine** — g-buffer geometry pass (MRT albedo + world-normal + depth, written by an
  opaque material's fragment shader via a `GBufferOutput` contract) → deferred
  directional-lighting pass (→ HDR) → tonemap (HDR → output) — with a `DebugView`
  setting re-wiring the pass set as the settings-drive-recompile proof, plus a
  directional **`Light`** builtin (its `TypeId` minted with planset-11's `vengc
  generate-type-id`). hello-triangle migrates its **main view** onto one
  `SceneRenderer` (composite samples `GetOutput()`, ImGui unchanged); a two-renderer
  interleaved GPU test proves the design-for-N surface, one wired. Prerequisites met:
  the **compiled `RenderGraph`** ([planset-8](planset-8/README.md)) gives the
  builder/`Compile()`/replay lifecycle, the runtime **`Scene`/`Camera`**
  ([planset-10](planset-10/README.md)) the per-frame input, and
  [planset-11](planset-11/README.md) the `TypeId` minter. Taken up **before** the
  editor (area 6) by choice — the editor inherits the multi-viewport consumer solved.
  Held back as named future increments: the rest of the über-pipeline batteries
  (shadows, SSAO, bloom, MSAA, transparent/forward pass, post stack), multiple/typed
  lights + light culling, and parallel pass recording. **Frames-in-flight > 1 is
  delivered in planset-13** by a cross-graph reuse barrier (ring-buffered output
  reserved for a future temporal/async consumer).

- **[planset-13](planset-13/README.md)** — `SceneRenderer` frames-in-flight
  correctness + roadmap re-cut (✅ done). Corrects the false single-in-flight comment
  and closes the cross-graph output reuse hazard with a renderer-owned
  `PrepareForAccess(ColorAttachment)` barrier recorded before each `Execute` — the
  reverse of the consumer's `Sample` transition. The output stays **single-copy** with
  **zero added memory**; a per-frame-in-flight ring is rejected and documented as a
  future escalation (a temporal/history-buffer consumer reading an older frame, or a
  handoff side moving off the single graphics queue). The barrier suffices without a
  semaphore or ring because both halves of the handoff record on the single graphics
  queue in submission order. **Supersedes planset-12 decision 6** (which deferred FIF >
  1 to a ring).

- **[future](future/README.md)** — work beyond the current plansets (📝 draft/vision,
  holding area; not a planset). The remaining areas are the **editor application**
  (the prioritized next planset — its game-module build model is **delivered by
  planset-9** and its native-type inspectors **reuse area 10's module reflection**,
  delivered by planset-11; the editor shell + cooker-on-demand + docking, the
  material node editor, and the scene editor remain future —
  [editor.md](future/editor.md) / [game-module.md](future/game-module.md), several
  plansets) and the **event/input** systems (area 4). Each becomes its own planset
  when taken up. (Testing areas 5a/5b, de-globalizing the context (area 3), the asset
  system's synchronous slice + bindless (area 1), and the threading/task system (area
  2 — which also turned area 1's `LoadSync` into the async `Load` default) are done —
  planset-3, planset-4, planset-5, and planset-6 respectively; area 6's **game-module
  prerequisite** and the **pipeline-caching** and **content-hashes** cross-cutting
  concerns are resolved by planset-9; **area 7's runtime half is delivered by
  planset-10**; **area 10 — cooker-side module reflection + the cooked prefab asset —
  is delivered by planset-11**, realizing the `VengModuleHost` `TypeRegistry&` seam;
  and **area 8 — the `SceneRenderer` deferred über-pipeline — is delivered by
  planset-12** (its minimal-deferred spine + a directional `Light`, the remaining
  batteries named future), with its **frames-in-flight > 1** correctness delivered in
  **planset-13** by a cross-graph reuse barrier (ring-buffered output reserved for a
  future temporal/async consumer). Hot-reload remains future: its re-cook half conflicts with
  offline-only cooking and needs a dev-only watcher design.)
