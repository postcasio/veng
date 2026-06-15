# future — work beyond the current plansets (DRAFT / vision)

> Not a planset and not scheduled — a **holding area** for the larger phases
> ahead. Each area below becomes its **own planset** when taken up and detailed
> planset-1 style. Direction, not plans.

Captured now so the earlier phases stay coherent with where veng is going.

## Areas

Numbered for stable cross-references. **DONE** areas are delivered and fully
documented in their plansets — only a one-paragraph recap and any still-future
remainder is kept here. The substance of this document is the **remaining**
areas (4, 6, 7, 8, 10).

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
  (`TypeRegistry`, the mechanism that lets the editor see a game's **native types** —
  C++ has no reflection — with hand-written field descriptors driving
  auto-inspectors) was **deferred out of this prerequisite into the editor-shell
  sub-area**, to be designed against the real inspector. Also still future:
  **installing `veng_add_game` for downstream `find_package(veng)` consumers** (see
  [game-module.md](game-module.md)).
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
  and a **scene editor** that is gated on area 7.
- **Depends on** the [threading/async-load path](threading-task-system.md) (area 2)
  for non-stalling live preview / hot-reload, [game-module.md](game-module.md), and
  area 7 (scene model) for the scene editor. Its native-type **inspectors reuse area
  10's module reflection** rather than re-introducing it.

### 7. Scene / entity model

A prerequisite the **scene editor** (area 6) cannot proceed without, and useful in
its own right: a transform hierarchy, a component system, component types described
through the reflection layer (so inspectors + serialization work), and a **scene asset
type** that cooks and loads like the others (planset-5 explicitly descoped scene
assets). Interacts with area 4 (events/input) and the `TypeRegistry`.

The **runtime model** — `Scene`/`Entity`, type-erased components, queries, the
transform hierarchy, `Camera`, the reflection layer with game-defined types, and a
runtime-built scene — is taken up by **[planset-10](../planset-10/README.md)** (in
progress). The **cooked `.scene` asset** is split off into **[area 10](#10-cooker-side-module-reflection--the-cooker-loads-the-game-module)**
(prioritized next), because cooking a scene that contains game-defined components needs
the cooker to reflect those components — i.e. to load the game module.

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
overview:** [scene-renderer.md](scene-renderer.md). Its enabling prerequisite, the
**compiled `RenderGraph`** (area 9), has landed; it takes its `Scene`/`Camera` from
area 7, its first/hardest consumer from area 6 (editor), and its frames-in-flight
contract + parallel-record story from area 2 (threading).

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
planset-6), and **area 9** (compiled `RenderGraph`, planset-8). The thin synchronous
asset slice was deliberately pulled forward as the "real client", then threading
turned those sync loads async; planset-8 then compiled the render graph, satisfying
area 8's one rendering prerequisite. That whole asset + threading chain is closed:

```
1 sync assets + bindless ✅ ──► 2 threading (async loads) ✅ ──► 1 async Load ✅
9 compiled RenderGraph ✅

remaining:
  7  scene/entity model (runtime) ──► planset-10 (in progress)
  10 cooker module reflection + cooked .scene ──► PRIORITIZED NEXT
        (needs 7 + planset-9 module model)
  6  editor (shell → material editor → scene editor)   (wants 2's async path ✅;
        scene editor needs 7 + 10; inspectors reuse 10's module reflection)
  8  scene renderer ──► needs 7 (Scene/Camera) + 6     (9 ✅ — no rendering gate left)
  4  events/input — independent, gameplay-driven (any time)
```

The remaining order:

1. **Scene/entity model — runtime (area 7).** The `Scene`/`Entity`/`TypeRegistry`
   core, reflection layer, transform hierarchy, `Camera`, and game-defined component
   types — **in progress as [planset-10](../planset-10/README.md)**.

2. **Cooker-side module reflection + cooked `.scene` (area 10) — PRIORITIZED NEXT.**
   The cooker `dlopen`s the game module to reflect its native types, realizing the
   `VengModuleHost` `TypeRegistry&` seam and delivering the cooked `.scene` asset
   (validated against the reflected descriptors, the way materials are validated
   against shader reflection). Gated on area 7 (planset-10) + the planset-9 module
   model — both in place once planset-10 lands.

3. **Editor application (area 6).** The authoring environment, spanning several
   plansets: the [game-module build model](game-module.md) (shared lib + launcher,
   C-ABI app registration) is **done — planset-9** (in-tree); next is the
   [editor shell + framework](editor.md) (cook-on-demand, single-window docking, the
   texture editor), then the node-based **material editor**. Its native-type
   **inspectors reuse area 10's module reflection** rather than re-introducing it. It
   builds on area 2's async path (done) for non-stalling live preview. The **scene
   editor** within it is gated on the scene/entity model (area 7) **and** the cooked
   scene asset (area 10), which land ahead of it.

4. **Scene renderer (area 8).** Needs `Scene`/`Camera` (area 7) and its first/hardest
   consumer, the editor (area 6); area 9's compiled `RenderGraph` prerequisite is met.

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
(testing, planset-3 + planset-4), and 9 (compiled `RenderGraph`, planset-8); plus area
6's first sub-area, the game-module build model (planset-9), and the
**pipeline-caching** and **content-hashes** cross-cutting concerns (planset-9).

**In flight / next:** area 7 (scene/entity model — runtime) is **in progress as
planset-10**, and area 10 (cooker-side module reflection + the cooked `.scene` asset)
is the **prioritized next planset** once it lands.

**Undetailed / unscheduled:** area 4 (events/input), the rest of area 6 (editor shell,
material editor, scene editor — [editor.md](editor.md) / [game-module.md](game-module.md)),
and area 8 (scene renderer — [scene-renderer.md](scene-renderer.md)); plus two named
deferrals — **hot-reload** (area 1; its re-cook half conflicts with offline-only cooking,
needs a dev-only watcher design) and **unifying `ShaderInterface`/`MaterialField` onto
planset-10's reflection layer** (area 7; kept apart because GPU layout is cooker-reflected
from Slang, not `offsetof`-based, and `FieldDescriptor.Offset` would need to carry both a
CPU and a GPU offset). Each becomes its own planset when taken up.
