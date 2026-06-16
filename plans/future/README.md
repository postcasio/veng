# future — work beyond the current plansets (DRAFT / vision)

> Not a planset and not scheduled — a **holding area** for the larger phases
> ahead. Each area below becomes its **own planset** when taken up and detailed
> planset-1 style. Direction, not plans.

Captured now so the earlier phases stay coherent with where veng is going.

## Areas

Numbered for stable cross-references. **DONE** areas are delivered and fully
documented in their plansets — only a one-paragraph recap and any still-future
remainder is kept here. The substance of this document is the **remaining**
areas (4, 6, 8) — plus the still-future remainders of areas done in part (area 7's
systems framework + the `ShaderInterface`/`MaterialField` unification; the
hot-reload tail of area 1).

### 1. Asset system — DONE (planset-5 + planset-6)

> **DONE.** The synchronous slice + bindless ([planset-5](../planset-5/README.md))
> and the async half ([planset-6](../planset-6/README.md)) shipped: the standalone
> `vengc` cooker, JSON asset packs cooked into `.vengpack` archives, the shared
> `assetformat` lib, the engine-side `AssetManager`/`AssetHandle`/`LoadSync`, the
> texture/mesh/shader/material types with **offline Slang reflection →
> `ShaderInterface`**, the **`BindlessRegistry` set-0 subsystem** that makes the
> material thin, a structured `AssetLoadError`, and async-default `Load` over a
> dedicated transfer queue (`LoadSync` the blocking sibling). **Design overview:**
> [asset-system.md](asset-system.md).
>
> **Still future — hot-reload (`Reload`).** Its re-upload half is exactly what the
> async path delivers, but its re-cook half conflicts with offline-only cooking,
> so it needs a dev-only watcher design.

### 2. Threading / task system — DONE (planset-6)

> **DONE** ([planset-6](../planset-6/README.md), 9 plans). A `TaskSystem` (fixed
> worker pool + work queue returning `Task<T>`, owned by `Application`, threaded
> explicitly, pumped once per frame) runs decode + upload off the main thread, over
> a dedicated **transfer queue** with per-worker command pools, a
> **`TimelineSemaphore`** primitive, queue-family-aware ownership transfer, and a
> transfer-keyed retire path — composing into async-default `Buffer/Image::Upload`
> and `AssetManager::Load`. The render thread stays single; the `Veng.h` contract
> says work runs off it *through the task system*. **Design overview:**
> [threading-task-system.md](threading-task-system.md).
>
> **Still future:** a task *graph* (inter-job dependencies), staging-buffer pooling,
> and cancellation.

### 3. De-globalize the rendering context — DONE (planset-4)

> **DONE** ([planset-4](../planset-4/README.md), plans 01–04). `Context::Instance()` /
> `s_Instance` are gone: every resource `Create` takes an explicit `Context&` and
> holds it as a back-reference for deferred-destruction `Retire`. veng remains
> single-threaded / single-context.

### 4. Event & input systems

Thin and partially stubbed: `EventType` declares focus/move events with no
classes; input lives ad-hoc on `Window` (`MouseButton` unused; no actions/
bindings/devices). Off the critical path — revisit when gameplay drives the
requirements.

### 5. Unit testing / test infrastructure — DONE (planset-3 + planset-4)

> **DONE.** 5a ([planset-3](../planset-3/README.md)): doctest + CTest wiring, a
> separate-process death harness (traps SIGABRT/SIGTRAP/SIGILL, gates on the assert
> message), and base coverage — pure-logic (`Result`, `VertexBufferLayout`),
> `ToVk`/`FromVk` round-trips, an extracted pure `DecideBarrier`/`ScopeFor` rule, and
> a consolidated GPU band that skips (not fails) with no ICD. 5b + the validation
> gate ([planset-4](../planset-4/README.md), plans 05–06): the in-process `veng_gpu`
> integration suite with per-case `Context` fixtures, and a local `ctest -L
> validation` gate that fails on any new unallowlisted `Vulkan validation` ERROR
> (allowlist now empty — the descriptor-pool / `UPDATE_AFTER_BIND` gap was closed by
> [planset-2/06](../planset-2/06-descriptor-update-policy.md)). CI with a hosted
> software ICD was explicitly descoped.

### 6. Editor application

The authoring environment — and the "demanding second consumer" flagged in the
cross-cutting concerns below. Spans **several plansets**; **design overview:**
[editor.md](editor.md), with the prerequisite build-model change in
[game-module.md](game-module.md). The shape:

- **Games become a shared library + a launcher — DONE (planset-9, in-tree)**
  ([game-module.md](game-module.md)). A game is `libgame` (shared, the runtime) + a
  thin launcher exe; the launcher `dlopen`s `libgame` and calls one C-ABI
  `VengModuleRegister` entry, into which the module registers its `Application`
  factory (`ApplicationRegistry`), with a `VengModuleAbiVersion` handshake and a
  relocatable launcher/lib/pack trio ([planset-9](../planset-9/README.md)). The
  editor-only `libgame_editor` (shared, never shipped) + the `EditorRegistry` it
  registers into + the editor host that loads it are **still future** — the ABI
  reserves a null `EditorRegistry*` slot for them. The **type-reflection layer**
  (`TypeRegistry` + `FieldDescriptor`/`TypeInfo`, the mechanism that lets the editor
  see a game's **native types** — C++ has no reflection — with hand-written field
  descriptors driving auto-inspectors) **now exists**: planset-10 pulled it forward to
  serialize components, and its `FieldDescriptor` already carries the optional editor
  metadata (`DisplayName`/`Tooltip`/`Min`/`Max`/`Hidden`/…) the inspectors read — so
  the editor **consumes** it rather than introducing it. The **module-reflection
  wiring it reads it through** is delivered too: planset-11 added `TypeRegistry& Types`
  to `VengModuleHost` (game-module.md seam 2), so a game registers its descriptors via
  `VengModuleRegister` and a host reflects them — the editor's native-type inspectors
  **reuse that exact seam** rather than reintroducing it.
  Also still future: **installing `veng_add_game` for downstream `find_package(veng)`
  consumers** (see [game-module.md](game-module.md)).
- **The editor is a cooker consumer** ([editor.md](editor.md)). The runtime never
  links importers; the editor — a tool — links `libveng_cook` for **cook-on-demand**,
  reading *source* assets, cooking live (off-thread), and previewing through the
  normal `AssetManager` path. The planset-5 boundary (importers never reach
  `libveng`/`libgame`) is preserved exactly.
- **Docking is already enabled** (ImGui `v1.92.4-docking`); the open call is
  **single-window docking vs. multi-viewport** — multi-viewport fights the current
  single-offscreen-image → swapchain compositing model, so v1 stays single-window.
  Previews reuse the sample's existing `ImGuiLayer::CreateTexture` → `ImGui::Image`
  render-to-panel pattern, one preview generalized to N.
- **A `libveng_editor` framework** (panels, an `AssetType`→editor registry,
  reflection-driven inspectors) that games extend from `libgame_editor` to add
  **custom views/tools** for their own asset types.
- **Concrete editors:** texture viewer/settings (the first slice), the node-based
  **material editor** (imnodes is already vendored; v1 binds params to an
  author-provided Slang shader — the *loaded* `.vmat` path planset-5 left open),
  and a **scene editor** whose **both gates are now met**: the area-7 runtime scene
  model (planset-10) and the area-10 cooked prefab asset + module reflection
  (planset-11).
- **Depends on** the [threading/async-load path](threading-task-system.md) (area 2)
  for non-stalling live preview / hot-reload, [game-module.md](game-module.md), and
  area 7 (scene model — runtime done) for the scene editor. Its native-type
  **inspectors reuse area 10's module reflection** (delivered by planset-11) rather
  than re-introducing it.

### 7. Scene / entity model

> **DONE (runtime) — delivered by [planset-10](../planset-10/README.md)** (5 plans).
> A `Scene` is a runtime ECS world: a generational `Entity` free-list plus one
> type-erased **sparse-set** pool per component type, with templated
> `Add`/`Remove`/`Get`/`Has` and multi-component `View`/`Each` queries. An
> engine-owned **`TypeRegistry`** (threaded into `Scene::Create`) records every
> reflected type under a stable `u64` **`TypeId` authored like an `AssetId`**, and
> the pulled-forward **reflection layer** (`Veng/Reflection/`: one open `TypeId`
> space + closed `FieldClass`, `FieldDescriptor`/`TypeInfo` authored via
> `VE_REFLECT`/`VE_FIELD` with optional editor metadata) drives a tolerant,
> name-keyed, recursive generic serializer. Builtins `Name`, `Transform` (local TRS)
> + `Parent` + the world-matrix walk, `Camera` (+ `CameraComponent`), and
> `MeshRenderer` are pre-registered through the same path a **game-defined** type
> uses — hello-triangle builds a one-entity `Scene` with a game-defined `Spinner` and
> renders through a `Camera`.
>
> **The cooked asset + the module-ABI seam are DONE —
> [area 10](#10-cooker-side-module-reflection--the-cooker-loads-the-game-module)
> (planset-11).** The deferred "scene asset type that cooks and loads like the
> others" is **delivered as the cooked prefab asset**: a `Scene` stays an engine
> primitive (never an asset), and the cooked thing is a `*.prefab.json` (entities +
> components + values, validated against the reflected descriptors) that loads as a
> cached `AssetHandle<Prefab>` and is **spawned** into a `Scene` (`Prefab::SpawnInto`).
> The `VengModuleHost` `TypeRegistry&` registration seam planset-10 left additive is
> realized there too. **Still future** — its named follow-ons: a **systems** framework
> (planset-10 ships storage + queries; the app writes its own update loops over
> `Each`/`View`);
> **archetype storage** and **dirty-flag** transform propagation (perf optimizations
> behind the same API); migrating `VE_REFLECT` to inline `[[=…]]` **annotation
> reflection** once AppleClang gains P2996/P3394; and the named follow-on
> **re-expressing `ShaderInterface`/`MaterialField`** on this reflection layer (below).

**Named follow-on — unify `ShaderInterface`/`MaterialField` onto the reflection
layer.** A later planset re-expresses the GPU-data field tables (the cooker's reflected
shader/material interface) on planset-10's reflection layer, so the editor inspects
material parameters through the **same generic field-walker** it uses for components,
rather than a parallel mechanism. planset-10 deliberately **does not** fold this in:
the two are different mechanisms — `VE_REFLECT` describes **CPU struct layout**
(`offsetof`-based, hand-authored, in `libveng`), whereas `ShaderInterface`/
`MaterialField` describe **GPU data layout** (std140/430 offsets, descriptor slots)
reflected **offline by the cooker from Slang**. Folding it into the CPU-only ECS stream
would drag the cooker and the material runtime into it. The reflection layer is built
generic enough to host it (one open `TypeId` space, closed `FieldClass`, a walker that
is proven *non-component-bound* by planset-10's round-trip tests), so this is additive
when taken up. **Design caveat to settle when it is:** `FieldDescriptor.Offset` is
single-purpose today — a C++ `offsetof`. A GPU field's offset is a *different* number
(a std140/430 buffer offset), so the unification needs `FieldDescriptor` to carry both
(or a layout/`FieldClass` distinction) for one walker to drive a **CPU memcpy** in the
component case and a **GPU buffer write** in the material case. Decide that representation
up front; it is painful to retrofit onto a populated descriptor table.

### 8. Scene renderer / render-pipeline architecture

A long-lived, configurable **`SceneRenderer`** sitting on top of `RenderGraph`:
constructed with an output format + a settings block, it owns its pass resources
and (eventually) a **compiled** graph, and renders a `Scene` from a camera into an
**offscreen target it owns**, handing back a sampleable result. The game's main
view and every editor preview panel are the same object — the editor renders **one
`Scene` through N `SceneRenderer`s**. An **über-pipeline of interdependent passes**
(fixed wiring) composed of **reusable, self-contained pass units**. **Design
overview:** [scene-renderer.md](scene-renderer.md). **Both** its prerequisites are
now met: the **compiled `RenderGraph`** (area 9) has landed, and its `Scene`/`Camera`
input (area 7's runtime model) is delivered by planset-10 — so no rendering or scene
gate remains. It takes its first/hardest consumer from area 6 (editor) and its
frames-in-flight contract + parallel-record story from area 2 (threading), and is
sequenced **after** area 10 (cooked `.scene`) and area 6 (editor).

### 9. Compiled RenderGraph — DONE (planset-8)

> **DONE** ([planset-8](../planset-8/README.md), 4 plans). `RenderGraph` is a pure
> **builder** whose `Compile()` bakes the barrier/transition schedule, transient
> allocation/aliasing, per-graphics-pass `RenderingInfo`, and one-time validation
> into a `CompiledGraph` that **replays** per frame — only the per-pass callbacks
> run. The resource model splits into **graph-owned transients** (logical
> `ResourceId` handles, resolved per frame and aliased onto shared backing) and
> late-bound **imports**; the callback takes a typed **`PassContext`** (`Cmd()` +
> `Resolved(ResourceId)`). Transient aliasing rides a pure, device-free, unit-tested
> live-range rule. Satisfies area 8's enabling prerequisite. **Design overview:**
> [compiled-rendergraph.md](compiled-rendergraph.md).
>
> **Still future:** dead-pass culling, multi-queue / async-compute scheduling, and
> parallel pass recording into secondary command buffers (area 2's seam).

### 10. Cooker-side module reflection — the cooker loads the game module — DONE (planset-11)

> **DONE** ([planset-11](../planset-11/README.md), 5 plans). The cooker gained the
> ability to **`dlopen` the game module and reflect its native component types**:
> `vengc cook --module <lib>` loads `libgame`, calls the C-ABI `VengModuleRegister`
> with a host carrying a `TypeRegistry` (`VengModuleHost` gained `TypeRegistry& Types`,
> ABI `1u→2u`), and the module registers its component descriptors into it — realizing
> the additive seam planset-10 named. On that capability landed the **cooked prefab
> asset**: a `*.prefab.json` (entities + components + values) cooks into an
> `AssetType::Prefab` blob — **validated against the reflected descriptors** (unknown
> component / wrong field type / malformed value caught at cook time, the way materials
> are validated against shader reflection) — that loads as a cached
> `AssetHandle<Prefab>` through the same `AssetManager::Load` path as every asset and
> **spawns** into a mutable `Scene` (`Prefab::SpawnInto`, entity references remapped,
> `AssetHandle` fields rehydrated). A `Scene` stays an engine primitive, never an
> asset — the deferred "scene asset type" is delivered as the prefab. The cooked blob
> reuses planset-10's name-keyed `WriteFields` record encoding; the prefab-cooking path
> links `veng::veng` and reuses `ModuleLoader` (the one scoped relaxation of the
> Vulkan-free cooker, graphics stack linked but never initialized). The registry
> ownership moved **host-side** (launcher/cooker constructs it, pre-registers builtins
> via a GPU-free `RegisterBuiltinTypes`, fills it through `VengModuleRegister`, threads
> it into the `Application`, which borrows a `TypeRegistry&`) — **superseding planset-10
> decision 4**. The **type manifest / `vengc generate-type-id`** secondary payoff
> shipped too.
>
> **Still future.** **Cross-compiled cooking** stays out: `dlopen` reflection loads a
> **host == target** lib, so cooking a cross-compiled target lib on the build host is a
> latent constraint for the anticipated Windows port — recorded, not solved. The
> **editor's native-type inspectors (area 6) reuse this same module-reflection seam**,
> so the editor inherits it solved.

## Ordering & dependencies

A first cut at sequencing — the order to *take the areas up* (each becomes its own
planset), not a schedule.

**Done:** areas 5a/3/5b + the validation gate (planset-3, planset-4), **area 1**
(sync slice + bindless *and* async, planset-5 + planset-6), **area 2** (threading,
planset-6), **area 9** (compiled `RenderGraph`, planset-8), **area 7's runtime half**
(scene/entity model, planset-10), and **area 10** (cooker module reflection + the
cooked prefab asset, planset-11). The thin synchronous asset slice was deliberately
pulled forward as the "real client", then threading turned those sync loads async;
planset-8 then compiled the render graph, planset-10 landed the runtime scene model —
satisfying **both** of area 8's prerequisites — and planset-11 closed area 10. That
whole asset + threading + scene + cook chain is closed:

```
1 sync assets + bindless ✅ ──► 2 threading (async loads) ✅ ──► 1 async Load ✅
9 compiled RenderGraph ✅
7 scene/entity model (runtime) ✅ ──► planset-10
10 cooker module reflection + cooked prefab ✅ ──► planset-11
        (realized the VengModuleHost TypeRegistry& seam)

remaining:
  6  editor (shell → material editor → scene editor)   (PRIORITIZED NEXT; wants 2's
        async path ✅; area-7 gate cleared ✅; scene editor's area-10 gate cleared ✅;
        inspectors reuse 10's module reflection ✅)
  8  scene renderer ──► needs 6  (7 ✅ Scene/Camera, 9 ✅ — no scene/rendering gate left)
  4  events/input — independent, gameplay-driven (any time)
```

The remaining order:

1. **Editor application (area 6) — PRIORITIZED NEXT.** The authoring environment,
   spanning several plansets: the [game-module build model](game-module.md) (shared
   lib + launcher, C-ABI app registration) is **done — planset-9** (in-tree); next is
   the [editor shell + framework](editor.md) (cook-on-demand, single-window docking,
   the texture editor), then the node-based **material editor**. Its native-type
   **inspectors reuse area 10's module reflection** (delivered by planset-11) rather
   than re-introducing it. It builds on area 2's async path (done) for non-stalling
   live preview. The **scene editor** within it has **both** its gates met — the
   area-7 runtime scene model (planset-10) and the area-10 cooked prefab asset +
   module reflection (planset-11).

2. **Scene renderer (area 8).** Its `Scene`/`Camera` (area 7) and compiled
   `RenderGraph` (area 9) prerequisites are both met; it now waits only on its
   first/hardest consumer, the editor (area 6).

**Event & input (area 4)** is off the critical path — independent of the
rendering/asset/threading work and driven by gameplay needs, so slot it in whenever
it's wanted.

## Cross-cutting concerns (weigh when opening each phase)

Not areas of their own — considerations that span the work above and are cheaper
to decide early than to retrofit.

**Open:**

- **The editor is the demanding second consumer.** hello-triangle (one pipeline,
  one push constant) won't surface multi-material/mesh/scene friction; the
  node-based editor will. Develop the editor and the engine API together so it
  exercises the asset/material surface as it's built — it doubles as the richer
  sample. Now a detailed area of its own — see [area 6](#6-editor-application)
  ([editor.md](editor.md), [game-module.md](game-module.md)).
- **Process discipline.** Keep planset-1's cadence — small, sample-verified,
  per-plan increments. planset-4 (de-global), planset-6 (threading), and planset-8
  (compiled graph) all followed it; the same discipline applies to the editor (6)
  ahead.

**Resolved:**

- **Design infrastructure against a real client** — planset-5 pulled a thin
  synchronous asset-loading slice (area 1) forward as a real consumer, so planset-6
  built the threading API against a delivered client, not a guess.
- **Higher-level descriptor management + bindless** — shipped in planset-5 as the
  `BindlessRegistry` set-0 subsystem; [planset-2/06](../planset-2/06-descriptor-update-policy.md)
  first corrected the flag-policy altitude of the existing layer. Sketch:
  [bindless-descriptors.md](bindless-descriptors.md). *(area 1.)*
- **Structured error type for the asset/import pipeline** — `AssetLoadError`
  (`AssetError::Kind` ∈ NotFound / WrongType / Corrupt / VersionMismatch /
  MissingDependency / LoadFailed); `AssetManager` returns `AssetResult<T>`. *(area 1,
  planset-5.)*
- **CI with a software Vulkan ICD** — explicitly descoped (planset-4): veng has no
  hosted pipeline. The GPU/headless suite and the validation gate stay local-dev-only.
  *(area 5.)*
- **Pipeline caching** — `Context` owns a `vk::PipelineCache` reused across every
  pipeline build, with opt-in disk persistence via
  `ApplicationInfo::PipelineCachePath`. A default cache directory and off-thread
  pipeline creation stay future. *(area 1, planset-9.)*
- **Content hashes in the vengpack archives** — format **v2**: a content hash per
  cooked blob + a TOC digest, cooker-written (xxh3-128), `vengc verify`-checked; the
  loader never verifies and `assetformat`/`libveng` gain no hash dependency. Unblocks
  **deduplication** and **incremental cooking** with no further format bump. *(area 1,
  planset-9.)*

## Status

Vision only beyond what is marked **DONE** above. **Done:** areas 1 (asset system,
planset-5 + planset-6), 2 (threading, planset-6), 3 (de-global, planset-4), 5
(testing, planset-3 + planset-4), 9 (compiled `RenderGraph`, planset-8), **area
7's runtime half** (scene/entity model, planset-10), and **area 10** (cooker-side
module reflection + the cooked prefab asset, planset-11); plus area 6's first
sub-area, the game-module build model (planset-9), and the **pipeline-caching** and
**content-hashes** cross-cutting concerns (planset-9).

**Next:** area 6 (the editor) is the **prioritized next planset** — its game-module
prerequisite (planset-9) and the module-reflection seam its inspectors reuse
(planset-11) are both in place — followed by area 8 (scene renderer). Area 4
(events/input) is off the critical path, slotted in whenever wanted.

**Undetailed / unscheduled:** area 4 (events/input), the rest of area 6 (editor shell,
material editor, scene editor — [editor.md](editor.md) / [game-module.md](game-module.md)),
and area 8 (scene renderer — [scene-renderer.md](scene-renderer.md)); plus two named
deferrals — **hot-reload** (area 1; its re-cook half conflicts with offline-only cooking,
needs a dev-only watcher design) and **unifying `ShaderInterface`/`MaterialField` onto
planset-10's reflection layer** (area 7; kept apart because GPU layout is cooker-reflected
from Slang, not `offsetof`-based, and `FieldDescriptor.Offset` would need to carry both a
CPU and a GPU offset). Each becomes its own planset when taken up.
