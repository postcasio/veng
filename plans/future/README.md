# future — work beyond planset-2 (DRAFT / vision)

> Not a planset and not scheduled — a **holding area** for the larger phases
> after the surface cleanup. Each area below becomes its **own planset** when
> taken up and detailed planset-1 style. Direction, not plans.

Captured now so the earlier phases stay coherent with where veng is going.

## Areas

### 1. Asset system — synchronous slice + bindless DONE (planset-5)

> **The synchronous slice and the bindless rework are done — delivered by
> [planset-5](../planset-5/README.md)** (the standalone `vengc` cooker, JSON asset
> packs cooked into `.vengpack` archives, the shared `assetformat` lib, the
> engine-side `AssetManager`/`AssetHandle`/`LoadSync`, the texture/mesh/shader/
> material types with **offline Slang reflection → `ShaderInterface`**, and the
> **`BindlessRegistry` set-0 subsystem** that makes the material thin). What
> **remains** is the **async** half — non-blocking `Load` on a transfer queue —
> which depends on **area 2 (threading)**; `LoadSync` was deliberately named to
> keep its spelling when async lands. **Design overview:**
> [asset-system.md](asset-system.md) (trimmed to the enduring async/hot-reload
> vision); the delivered foundation lives in planset-5.

The roadmap began **by defining the asset API** — the general abstraction first,
concrete types after — and planset-5 followed that order. What it delivered, and
what is left:

- **Asset API (done):** assets are identified by an opaque `u64` `AssetId`,
  referenced by `AssetHandle<T>` (cache indirection, hot-reload-ready),
  loaded/cached through `AssetManager`, and imported by the offline cooker.
- **Asset types (done):** texture, mesh, shader, and material — each a cooker
  importer + a cooked runtime form on top of the asset API.
- **Materials (done):** the material is the rendering interface, not the shader —
  a thin bundle of a shader handle, texture **handles**, and a `MaterialData`
  SSBO entry, bound through bindless set 0. The **constructed** path (reference a
  shader + explicitly-typed fields, validated against the shader's reflected
  interface) shipped; the **loaded** path from a node-based material editor is the
  editor's job, still future.
- **Deferred shader work (done):** **offline shader reflection →
  serializable `ShaderInterface`** (descriptor bindings, push-constant blocks,
  vertex inputs) produced by the cooker via Slang, not at runtime; layouts derive
  from it and engine-provided set 0 is recognized without the author declaring it.
- **Remaining (async, area 2):** hot-reload (`Reload`) and non-blocking `Load`
  over a transfer queue — they need the threading/task system.

### 2. Threading / task system

Explore this deeply — veng has no standardized concurrency story, and async asset
loading needs one. planset-1 deliberately shipped a **single-threaded v1
contract** (documented in `Veng.h`); this phase revisits it. **Design overview:**
[threading-task-system.md](threading-task-system.md).

- **A standard way to run work off the main thread** — threads vs. a task/job
  system vs. a task graph. Open design area; pick a model deliberately rather
  than ad-hoc `std::thread`s.
- **Vulkan-queue-correct from the start.** Today asset uploads go through
  `Context::SubmitImmediateCommands` → `WaitIdle` on the graphics queue, i.e.
  fully synchronous and main-thread-blocking. Async loading needs: a dedicated
  **transfer queue**, per-thread command pools (pools are not shareable across
  threads), queue-family **ownership transfers** for resources handed to the
  render queue, and fence/timeline-semaphore sync between loader and render
  threads — done correctly, not bolted on.
- **Goal:** load assets (decode + upload) without stalling the frame.
- Touches the `Context` (queues, pools) and the resource upload paths
  (`Buffer`/`Image::Upload`). Interacts with de-globalizing the context (area 3).

### 3. De-globalize the rendering context — DONE (planset-4)

> ~~Taken up by [planset-4](../planset-4/README.md)~~ — **done**. Plans 01–04
> threaded an explicit `Context&` into every resource `Create`, converted the
> context-internal primitives off the global, and deleted `Context::Instance()` /
> `s_Instance` entirely. veng remains single-threaded/single-context (the freedom
> this buys is not yet used — see area 2).

`Context::Instance()` was a global singleton reached by every resource constructor
(`Buffer::Create` etc. secretly grabbed it) — the biggest "not-modern" smell:
blocked more than one device, hid the dependency, coupled tests to global state,
and fought multi-threaded creation. planset-1 kept it deliberately; planset-4
removed it via explicit threading (`X::Create(Context&, const XInfo&)`, each
resource holding a `Context&` back-reference for deferred-destruction `Retire`).

### 4. Event & input systems

Thin and partially stubbed: `EventType` declares focus/move events with no
classes; input lives ad-hoc on `Window` (`MouseButton` unused; no actions/
bindings/devices). Revisit when gameplay drives the requirements.

### 5. Unit testing / test infrastructure — DONE (5a: planset-3, 5b + validation gate: planset-4)

> **Area 5a is DONE — delivered by [planset-3](../planset-3/README.md)**: doctest
> framework + CTest wiring, a death harness (separate-process; traps
> SIGABRT/SIGTRAP/SIGILL and gates on the assert message via
> `PASS_REGULAR_EXPRESSION` — `WILL_FAIL` does *not* invert a signal death, so the
> original sketch below was wrong on that point), and base coverage: pure-logic
> (`Result`, `VertexBufferLayout`), `ToVk`/`FromVk` round-trips, an extracted pure
> `DecideBarrier`/`ScopeFor` rule + tests, death tests, and a consolidated one-exe
> GPU band that skips (not fails) with no ICD. Typed-buffer size math is covered
> end-to-end on the GPU, not extracted.
>
> **Area 5b + the validation gate are DONE — delivered by
> [planset-4](../planset-4/README.md)** (plans 05–06), written *after* its
> de-global change (plans 01–04), per the `5a → 3 → 5b` ordering below:
>
> - **5b — in-process multi-case GPU integration suite** (`veng_gpu`, plan 05):
>   a doctest-based executable with per-case `Context` fixtures
>   (`tests/gpu/fixture.h`), giving real per-case isolation now that
>   `Context::Instance()` is gone. Ports the focused GPU exercises from
>   planset-3 plan 06 (buffer/typed-buffer roundtrip, image clear+format,
>   descriptor write paths) plus a new per-case isolation proof. Same
>   skip-with-no-ICD contract as the rest of the `gpu`-labelled band.
> - **Local validation-error gate** (plan 06): `ctest -L validation` runs the
>   `gpu`-labelled binaries under `build-debug/` (`VE_DEBUG=ON`) and fails if a
>   new `[ERROR] Vulkan validation` line appears, via an allowlist
>   (`cmake/ValidationGate.cmake`) of the one documented, pinned gap below. CI
>   with a hosted software-ICD pipeline was explicitly descoped — veng has no
>   hosted pipeline and none is planned; this gate is local-only, dependency-free
>   (`cmake -P`), and runs as part of `ctest`.

- ~~**Known descriptor-pool / `UPDATE_AFTER_BIND` validation gap**~~ (storage-image,
  and — surfaced by planset-3's `descriptor_write_paths` — sampled-image pool
  sizes): closed by [planset-2/06](../planset-2/06-descriptor-update-policy.md)
  — static-by-default bindings, the `descriptorBindingStorageImageUpdateAfterBind`
  feature, and a Primary Pool budget for every `DescriptorType`. The validation
  gate's allowlist is now empty.

### 6. Editor application

The authoring environment — and the "demanding second consumer" flagged in the
cross-cutting concerns below. Spans **several plansets**; **design overview:**
[editor.md](editor.md), with the prerequisite build-model change in
[game-module.md](game-module.md). The shape:

- **Games become a shared library + a launcher** ([game-module.md](game-module.md)).
  A game is `libgame` (shared, the runtime) + a thin launcher exe + an editor-only
  `libgame_editor` (shared, never shipped). The editor and the launcher are both
  *hosts* that load `libgame`; only the editor also loads `libgame_editor`. This is
  what lets the editor see a game's **native types** — registered through a C-ABI
  entry point into a `TypeRegistry` (C++ has no reflection), with hand-written field
  descriptors driving auto-inspectors.
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
  area 7 (scene model) for the scene editor.

### 7. Scene / entity model

A prerequisite the **scene editor** (area 6) cannot proceed without, and useful in
its own right: a transform hierarchy, a component system, component types described
through the area-6 reflection layer (so inspectors + serialization work), and a
**scene asset type** that cooks and loads like the others (planset-5 explicitly
descoped scene assets). Interacts with area 4 (events/input) and the
`TypeRegistry`. Undetailed — gets its own design pass when taken up, ahead of the
scene editor.

## Ordering & dependencies

A first cut at sequencing — the order to *take the areas up* (each becomes its own
planset), not a schedule. Refine when each is detailed.

Areas 5a, 3, 5b (+ the validation gate), and area 1's **synchronous slice +
bindless** are **done** (planset-3, planset-4, planset-5). The thin synchronous
asset slice was deliberately pulled forward as the "real client" (the
cross-cutting concern below), so the remaining chain inverts the original order —
threading now turns the delivered sync loads async:

```
1 sync assets + bindless ✅ ──► 2 threading (async loads)
4 events/input — independent, gameplay-driven (any time)
```

~~1. Test harness + pure-logic tests (area 5, first half).~~ Done — planset-3.

~~2. De-globalize the context (area 3).~~ Done — planset-4 (plans 01–04).

~~3. GPU / integration tests (area 5, second half) + validation gate.~~ Done —
planset-4 (plans 05–06).

~~4. Asset system (area 1) — synchronous slice + bindless.~~ Done — planset-5
(cooker, packs/archives, `AssetManager`/`LoadSync`, texture/mesh/shader/material,
offline Slang reflection, the `BindlessRegistry` set-0 subsystem).

1. **Threading / task system (area 2).** Design against the explicit-device API
   (`Context::Instance()` is gone) and the real asset client planset-5 delivered;
   this is where the single-threaded v1 contract is deliberately lifted, and
   Vulkan-queue correctness is the hard part. It also delivers **area 1's
   remaining async half** — non-blocking `Load` over a transfer queue and
   hot-reload — turning planset-5's `LoadSync` into the async default.

2. **Editor application (area 6).** The authoring environment, spanning several
   plansets: the [game-module build model](game-module.md) (shared lib + launcher,
   native-type registration) first, then the [editor shell + framework](editor.md)
   (cook-on-demand, single-window docking, the texture editor), then the node-based
   **material editor**. It wants area 2's async/hot-reload path for non-stalling
   live preview, so it follows threading. The **scene editor** within it is gated on
   the **scene/entity model (area 7)**, which lands ahead of it.

**Event & input (area 4)** is off the critical path — independent of the
rendering/asset/threading work and driven by gameplay needs, so slot it in
whenever it's wanted. **The scene/entity model (area 7)** is the one prerequisite
the editor's scene view cannot skip; take it up before that view.

~~Open question: how much of the asset API (definition + sync loading) to pull
forward in parallel with threading, vs. keeping the whole asset phase last.~~
Resolved: planset-5 pulled the whole synchronous slice (+ bindless) forward
*before* threading, as the real client; threading now adds async on top.

## Cross-cutting concerns (weigh when opening each phase)

Not areas of their own — considerations that span the work above and are cheaper
to decide early than to retrofit.

- **Design infrastructure against a real client.** Threading (2) risks being
  designed speculatively. Consider pulling a deliberately thin, *synchronous*
  asset-loading slice (1) forward — just enough to be a real consumer — so it
  surfaces the requirements that shape the threading API, rather than reworking
  it after the fact. Infrastructure built against an actual caller tends to be
  right. *(affects ordering of 1 / 2; de-global (3) is done — planset-4.)*
- **Higher-level descriptor management + a bindless system** in the asset/material
  phase — not an open question but a requirement. Materials (many textures/
  parameters, per-material sets multiplying across a scene) need descriptor
  management above today's per-set `DescriptorSet`/`DescriptorSetLayout` layer:
  name-based binding driven by shader reflection, and bindless / descriptor-
  indexing (the modern default) for texture tables and per-draw resource access.
  It deeply shapes the descriptor/layout/material-binding APIs and is painful to
  retrofit, so design it here. planset-2 explicitly deferred bindless;
  [planset-2/06](../planset-2/06-descriptor-update-policy.md) (done) corrected
  the *flag-policy altitude* of the existing layer — static by default,
  bindless an explicit per-binding opt-in via a single type/feature/pool table.
  The real bindless subsystem (large arrays, per-frame streaming, possibly
  descriptor buffers) — sketched in
  [bindless-descriptors.md](bindless-descriptors.md) — **shipped in planset-5
  (plan 05)** as the `BindlessRegistry` set-0 subsystem. *(area 1, delivered.)*
- ~~**Structured error type for the asset/import pipeline.**~~ **Delivered by
  planset-5** as `AssetLoadError` (`AssetError::Kind` ∈ NotFound / WrongType /
  Corrupt / VersionMismatch / MissingDependency / LoadFailed); `AssetManager`
  returns `AssetResult<T>` = `std::expected<T, AssetLoadError>`. *(area 1,
  resolved.)*
- ~~**CI with a software Vulkan ICD** (lavapipe / SwiftShader) as part of the
  testing work, not after.~~ Explicitly descoped by planset-4 (plan 06): veng has
  no hosted pipeline and none is planned. The GPU/headless suite stays
  local-dev-only (skips with no ICD); the validation gate is local too.
  *(area 5, resolved.)*
- **The editor is the demanding second consumer.** hello-triangle (one pipeline,
  one push constant) won't surface multi-material/mesh/scene friction; the
  node-based editor will. Develop the editor and the engine API together so it
  exercises the asset/material surface as it's built — it doubles as the richer
  sample. Now a detailed area of its own — see [area 6](#6-editor-application)
  ([editor.md](editor.md), [game-module.md](game-module.md)). *(area 1 → area 6.)*
- **Pipeline caching.** Persist `VkPipelineCache` to disk once materials multiply
  — load-time win, naturally part of the asset/material phase. *(area 1.)*
- **Content hashes in the vengpack archives.** Carry a content hash per cooked
  blob (and/or a whole-archive digest) in the pack/archive format. Buys three
  things that compound as the asset count grows: **integrity verification**
  (detect a truncated/corrupt blob), **incremental cooking** (skip re-cooking a
  source whose inputs hash unchanged), and **deduplication** (identical cooked
  blobs share storage). Decide the hash's scope (per-blob vs. whole-archive),
  algorithm, and where it sits in the header before the archive format is widely
  depended on — adding it later is a format-version bump.
  **The loader does not verify.** Hashing every blob at load would be slow and
  the hashes are there for tooling, not the hot path — the runtime trusts its
  packs. Write the hashes in the cooker and expose verification as a separate
  **`vengc verify`** tool that re-hashes an archive's blobs and reports
  mismatches on demand. Touches `assetformat` (the on-disk layout in
  `CookedBlobs.h` / `AssetPack.h`) and the cooker (compute on write + the verify
  tool); the engine loader is deliberately untouched. *(area 1.)*
- **Process discipline.** Keep planset-1's cadence — small, sample-verified,
  per-plan increments. planset-4 followed this for de-global (3), which is now
  done; the same discipline applies to threading (2), where a big-bang sweep
  would be just as tempting and dangerous.

## Status

Vision only beyond what's noted done above. Areas 3 and 5 are complete
(planset-3, planset-4) and **area 1's synchronous slice + bindless is complete**
(planset-5); area 1's **async** half (folded into area 2), **area 2** (threading),
**area 4** (events/input), **area 6** (editor — [editor.md](editor.md) /
[game-module.md](game-module.md)), and **area 7** (scene/entity model) remain
undetailed/unscheduled. Each becomes its own planset (area 6, several) when taken
up.
