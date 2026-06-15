# future — work beyond the current plansets (DRAFT / vision)

> Not a planset and not scheduled — a **holding area** for the larger phases
> ahead. Each area below becomes its **own planset** when taken up and detailed
> planset-1 style. Direction, not plans.

Captured now so the earlier phases stay coherent with where veng is going.

## Areas

Numbered for stable cross-references. **DONE** areas are delivered and fully
documented in their plansets — only a one-paragraph recap and any still-future
remainder is kept here. The substance of this document is the **remaining**
areas (4, 6, 8, 10) — plus area 7's still-future remainder (its runtime half is
done; its cooked `.scene` asset is area 10).

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
  the editor **consumes** it rather than introducing it, and its native-type
  introspection **reuses area 10's** module reflection to obtain a game's descriptors.
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
  and a **scene editor** whose area-7 (runtime scene model) gate is now **cleared by
  planset-10**; its remaining gate is the cooked `.scene` asset (area 10), which lands
  next.
- **Depends on** the [threading/async-load path](threading-task-system.md) (area 2)
  for non-stalling live preview / hot-reload, [game-module.md](game-module.md), and
  area 7 (scene model — runtime done) for the scene editor. Its native-type
  **inspectors reuse area 10's module reflection** rather than re-introducing it.

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
> **Still future — the cooked `.scene` asset and the module-ABI seam are
> [area 10](#10-cooker-side-module-reflection--the-cooker-loads-the-game-module)**
> (the prioritized next planset), *not* loose future: cooking a scene that contains
> game-defined components needs the cooker to reflect those components (load the game
> module), and the `VengModuleHost` `TypeRegistry&` registration seam planset-10 left
> additive is realized there. Also still future: a **systems** framework (planset-10
> ships storage + queries; the app writes its own update loops over `Each`/`View`);
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

### 10. Cooker-side module reflection — the cooker loads the game module

> **Prioritized — the next planset**, taken up immediately after area 7's runtime
> scene model ([planset-10](../planset-10/README.md)). Its prerequisites are in place:
> the game-module build model ([planset-9](../planset-9/README.md)) and the reflection
> layer + runtime `Scene`/`TypeRegistry` (planset-10).

The cooker gains the ability to **`dlopen` the game module and reflect its native
types**. `vengc` loads `libgame`, calls the C-ABI `VengModuleRegister` with a host
carrying a `TypeRegistry`, and the module registers its component types — descriptors
and all — into it, realizing the **`TypeRegistry&`-on-`VengModuleHost`** seam
planset-10 deferred as additive. The cooker then knows every component's shape, engine
builtins and game-defined alike.

**Primary deliverable — the cooked `.scene` asset.** A `*.scene.json` source (entities
+ components + field values) cooks into a binary scene blob in the pack, loaded at
runtime into a `Scene`. The importer **validates the source against the reflected
component descriptors** — unknown component, wrong field type, missing field caught at
cook time — exactly as the material importer validates `*.vmat.json` against a shader's
reflected `ShaderInterface` today. This is the piece planset-10 named as its hardest
deferred problem; module reflection is its enabler. (planset-10's **name-keyed,
schema-tolerant** serialization means the cooker can emit a layout-agnostic value tree
and let the runtime's reflection bind it on load — but with the module loaded the
cooker also gets full **validation**, not blind passthrough.)

**The cost, recorded honestly** — why it is its own phase, not a footnote:

- **It inverts the cooker → runtime dependency.** The cooker is Vulkan-free and never
  links `libveng` today; loading `libgame` pulls `libveng` (and its graphics stack)
  into the cooker's process. The clean separation is relaxed deliberately, scoped to
  the load path.
- **It needs a GPU-free registration contract.** Type registration must run with no
  live `Context`/device (the cooker is headless, no ICD on CI). `Register<T>` + the
  `Application`-factory lambda are GPU-free by design; this phase makes that a
  *guaranteed, tested* contract rather than an incidental property.
- **It adds a build-order edge.** A `.scene` cooks only after `libgame` is built, so
  the build graph grows a `lib → cook-scenes → bundle` edge (ordinary asset packs stay
  independent of the lib).
- **It ties cooking to host == target.** `dlopen` reflection cannot load a
  cross-compiled target lib on the build host; cooking stays a host-tool activity for
  host == target (a latent constraint for the anticipated Windows port).

**Secondary payoffs.** A reflected type table is a real source of truth for tooling: a
generated **type manifest** (names → `TypeId`s) for optional `generate-type-id` dedup,
and the **same seam the editor's native-type inspectors (area 6) will read** — the
cooker and the editor share one mechanism for "see a game's types," rather than each
inventing its own. Splitting it out here means the editor inherits it solved.

## Ordering & dependencies

A first cut at sequencing — the order to *take the areas up* (each becomes its own
planset), not a schedule.

**Done:** areas 5a/3/5b + the validation gate (planset-3, planset-4), **area 1**
(sync slice + bindless *and* async, planset-5 + planset-6), **area 2** (threading,
planset-6), **area 9** (compiled `RenderGraph`, planset-8), and **area 7's runtime
half** (scene/entity model, planset-10). The thin synchronous asset slice was
deliberately pulled forward as the "real client", then threading turned those sync
loads async; planset-8 then compiled the render graph and planset-10 landed the
runtime scene model — satisfying **both** of area 8's prerequisites. That whole asset
+ threading + scene chain is closed:

```
1 sync assets + bindless ✅ ──► 2 threading (async loads) ✅ ──► 1 async Load ✅
9 compiled RenderGraph ✅
7 scene/entity model (runtime) ✅ ──► planset-10

remaining:
  10 cooker module reflection + cooked .scene ──► PRIORITIZED NEXT
        (needs 7 ✅ + planset-9 module model ✅)
  6  editor (shell → material editor → scene editor)   (wants 2's async path ✅;
        area-7 gate cleared ✅; scene editor needs 10; inspectors reuse 10's
        module reflection)
  8  scene renderer ──► needs 6  (7 ✅ Scene/Camera, 9 ✅ — no scene/rendering gate left)
  4  events/input — independent, gameplay-driven (any time)
```

The remaining order:

1. **Cooker-side module reflection + cooked `.scene` (area 10) — PRIORITIZED NEXT.**
   The cooker `dlopen`s the game module to reflect its native types, realizing the
   `VengModuleHost` `TypeRegistry&` seam and delivering the cooked `.scene` asset
   (validated against the reflected descriptors, the way materials are validated
   against shader reflection). Gated on area 7 (planset-10) + the planset-9 module
   model — both now in place.

2. **Editor application (area 6).** The authoring environment, spanning several
   plansets: the [game-module build model](game-module.md) (shared lib + launcher,
   C-ABI app registration) is **done — planset-9** (in-tree); next is the
   [editor shell + framework](editor.md) (cook-on-demand, single-window docking, the
   texture editor), then the node-based **material editor**. Its native-type
   **inspectors reuse area 10's module reflection** rather than re-introducing it. It
   builds on area 2's async path (done) for non-stalling live preview. The **scene
   editor** within it has its scene/entity-model gate (area 7) **cleared by
   planset-10**; its remaining gate is the cooked scene asset (area 10), which lands
   ahead of it.

3. **Scene renderer (area 8).** Its `Scene`/`Camera` (area 7) and compiled
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
(testing, planset-3 + planset-4), 9 (compiled `RenderGraph`, planset-8), and **area
7's runtime half** (scene/entity model, planset-10); plus area 6's first sub-area, the
game-module build model (planset-9), and the **pipeline-caching** and
**content-hashes** cross-cutting concerns (planset-9).

**Next:** area 10 (cooker-side module reflection + the cooked `.scene` asset) is the
**prioritized next planset** — its prerequisites (area 7 + the planset-9 module model)
are now in place — followed by area 6 (editor), then area 8 (scene renderer).

**Undetailed / unscheduled:** area 4 (events/input), the rest of area 6 (editor shell,
material editor, scene editor — [editor.md](editor.md) / [game-module.md](game-module.md)),
and area 8 (scene renderer — [scene-renderer.md](scene-renderer.md)); plus two named
deferrals — **hot-reload** (area 1; its re-cook half conflicts with offline-only cooking,
needs a dev-only watcher design) and **unifying `ShaderInterface`/`MaterialField` onto
planset-10's reflection layer** (area 7; kept apart because GPU layout is cooker-reflected
from Slang, not `offsetof`-based, and `FieldDescriptor.Offset` would need to carry both a
CPU and a GPU offset). Each becomes its own planset when taken up.
