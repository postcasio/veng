# future — work beyond planset-2 (DRAFT / vision)

> Not a planset and not scheduled — a **holding area** for the larger phases
> after the surface cleanup. Each area below becomes its **own planset** when
> taken up and detailed planset-1 style. Direction, not plans.

Captured now so the earlier phases stay coherent with where veng is going.

## Areas

### 1. Asset system (incl. materials, textures, meshes)

The headline next phase. veng needs a real asset system, and **the work begins by
defining the asset API** — the general abstraction first, concrete asset types
after. **Design overview:** [asset-system.md](asset-system.md). **The synchronous
slice is now detailed as its own planset:**
[planset-5](../planset-5/README.md) (cooker + asset packs + `LoadSync`; Slang +
offline reflection; **includes the bindless subsystem** so materials are thin —
async loading is the one follow-on). Marked done here when planset-5 lands.

- **Asset API (first):** how assets are identified, referenced (handles/refs),
  loaded, cached, lifetime-managed, hot-reloaded, and imported/cooked. This is
  the foundation every asset type plugs into.
- **Asset types:** materials, textures, meshes, shaders — each an importer +
  cooked runtime form on top of the asset API.
- **Materials** specifically: the material becomes the primary rendering
  interface, not the shader. Bundle a shader (binary) with its uniform/texture
  data; bind and draw a material instead of juggling pipelines, descriptor sets,
  push constants and layouts by hand. Two paths: **loaded** (a node-based
  material editor produces an asset carrying shader binary + parameter data) and
  **constructed** (reference a shader + explicitly supplied uniform/texture info,
  validated against what the shader needs). This **requires higher-level
  descriptor management and a bindless system** — see the descriptor-strategy
  cross-cutting concern below.
- This phase **absorbs all deferred shader work** (why planset-1/12 and the
  shader parts of planset-2 were dropped): **offline shader reflection →
  serializable `ShaderInterface`** (descriptor bindings, push-constant blocks,
  vertex inputs) produced by the importer at cook time, not at runtime;
  descriptor/pipeline layouts derived from it; name-based binding; vertex layout
  derived from / validated against the shader.

Depends on area 2 (threading) for non-blocking loads.

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

- **Known descriptor-pool / `UPDATE_AFTER_BIND` validation gap** (storage-image,
  and — surfaced by planset-3's `descriptor_write_paths` — sampled-image pool
  sizes): not a testing task but a real engine gap the tests now pin, and the
  validation gate's one allowlist entry. It belongs to the
  [bindless/descriptor rework](bindless-descriptors.md), not area 5 — when that
  rework closes the gap, remove the allowlist entry so the gate tightens
  automatically.

## Ordering & dependencies

A first cut at sequencing — the order to *take the areas up* (each becomes its own
planset), not a schedule. Refine when each is detailed.

Areas 5a, 3, and 5b (+ the validation gate) are **done** (planset-3, planset-4).
The remaining chain is:

```
2 threading ──► 1 asset system
4 events/input — independent, gameplay-driven (any time)
```

~~1. Test harness + pure-logic tests (area 5, first half).~~ Done — planset-3.

~~2. De-globalize the context (area 3).~~ Done — planset-4 (plans 01–04).

~~3. GPU / integration tests (area 5, second half) + validation gate.~~ Done —
planset-4 (plans 05–06).

1. **Threading / task system (area 2).** Design against the explicit-device API
   (now available — `Context::Instance()` is gone); this is where the
   single-threaded v1 contract is deliberately lifted, and Vulkan-queue
   correctness is the hard part.
2. **Asset system (area 1) — headline, end of the chain.** The general asset API
   (types, handles, import/cook, *synchronous* load) is largely independent and
   could be scoped/started earlier, but the headline payoff — loading without
   stalling the frame — needs threading (step 1 above). Land the API early if
   convenient; async loading after threading.

**Event & input (area 4)** is off the critical path — independent of the
rendering/asset/threading work and driven by gameplay needs, so slot it in
whenever it's wanted.

Open question: how much of the asset API (definition + sync loading) to pull
forward in parallel with threading, vs. keeping the whole asset phase last.

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
  retrofit, so design it here. planset-2 explicitly deferred bindless, and the
  [planset-2/06 addendum](../planset-2/06-descriptor-update-policy.md) found the
  current descriptor layer already strains a single basic use case — it hard-codes
  `UPDATE_AFTER_BIND` on every binding with hand-maintained, drift-prone feature/
  pool prerequisites. That addendum only corrects the *flag-policy altitude* for
  the existing layer; the real bindless subsystem (large arrays, per-frame
  streaming, possibly descriptor buffers) is this phase's job. *(area 1.)*
- **Structured error type for the asset/import pipeline.** `Result<T>` carries a
  `std::string` today; asset loading is where callers will want to branch on error
  *kind* (not-found / corrupt / version-mismatch / missing-dependency). Expect to
  promote the error to a structured type — `Result.h` already flags this. *(area 1.)*
- ~~**CI with a software Vulkan ICD** (lavapipe / SwiftShader) as part of the
  testing work, not after.~~ Explicitly descoped by planset-4 (plan 06): veng has
  no hosted pipeline and none is planned. The GPU/headless suite stays
  local-dev-only (skips with no ICD); the validation gate is local too.
  *(area 5, resolved.)*
- **The editor is the demanding second consumer.** hello-triangle (one pipeline,
  one push constant) won't surface multi-material/mesh/scene friction; the
  node-based editor will. Develop the editor and the engine API together so it
  exercises the asset/material surface as it's built — it doubles as the richer
  sample. *(area 1.)*
- **Pipeline caching.** Persist `VkPipelineCache` to disk once materials multiply
  — load-time win, naturally part of the asset/material phase. *(area 1.)*
- **Process discipline.** Keep planset-1's cadence — small, sample-verified,
  per-plan increments. planset-4 followed this for de-global (3), which is now
  done; the same discipline applies to threading (2), where a big-bang sweep
  would be just as tempting and dangerous.

## Status

Vision only beyond what's noted done above. Areas 3 and 5 are complete
(planset-3, planset-4); areas 1, 2, and 4 remain undetailed/unscheduled. Each
becomes its own planset when taken up.
