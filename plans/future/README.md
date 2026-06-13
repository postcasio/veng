# future — work beyond planset-2 (DRAFT / vision)

> Not a planset and not scheduled — a **holding area** for the larger phases
> after the surface cleanup. Each area below becomes its **own planset** when
> taken up and detailed planset-1 style. Direction, not plans.

Captured now so the earlier phases stay coherent with where veng is going.

## Areas

### 1. Asset system (incl. materials, textures, meshes)

The headline next phase. veng needs a real asset system, and **the work begins by
defining the asset API** — the general abstraction first, concrete asset types
after.

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
contract** (documented in `Veng.h`); this phase revisits it.

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

### 3. De-globalize the rendering context

`Context::Instance()` is a global singleton reached by every resource constructor
(`Buffer::Create` etc. secretly grab it). Biggest "not-modern" smell: blocks more
than one device, hides the dependency, couples tests to global state, and fights
multi-threaded creation. planset-1 kept it deliberately; this phase removes it
(thread an explicit device/context into creation, or a scoped "current device").
Pervasive, mechanical change — best done with/before the asset + threading work,
which want device-explicit, device-free-editor code paths anyway.

### 4. Event & input systems

Thin and partially stubbed: `EventType` declares focus/move events with no
classes; input lives ad-hoc on `Window` (`MouseButton` unused; no actions/
bindings/devices). Revisit when gameplay drives the requirements.

### 5. Unit testing / test infrastructure (firm up soon)

> **Area 5a is DONE — delivered by [planset-3](../planset-3/README.md)**: doctest
> framework + CTest wiring, a death harness (separate-process; traps
> SIGABRT/SIGTRAP/SIGILL and gates on the assert message via
> `PASS_REGULAR_EXPRESSION` — `WILL_FAIL` does *not* invert a signal death, so the
> original sketch below was wrong on that point), and base coverage: pure-logic
> (`Result`, `VertexBufferLayout`), `ToVk`/`FromVk` round-trips, an extracted pure
> `DecideBarrier`/`ScopeFor` rule + tests, death tests, and a consolidated one-exe
> GPU band that skips (not fails) with no ICD. Typed-buffer size math is covered
> end-to-end on the GPU, not extracted.

What remains in area 5 after planset-3:

- **5b — in-process multi-case GPU integration suite**, deferred until after the
  de-globalize change (area 3) so it targets the explicit-device API and gets real
  per-test isolation (stand a `Context` up/down per case). It can't get that
  isolation while `Context::Instance()` is a singleton, and writing it against the
  singleton now would mean rewriting it post-de-global — hence the `5a → 3 → 5b`
  ordering. Its exact shape (fixtures, per-case device lifecycle) is fixed by where
  de-global lands, so it's specified then, not now. The existing one-exe-per-test
  GPU band (each its own process → its own singleton) is the stopgap until then.
- **CI with a software Vulkan ICD** (lavapipe / SwiftShader). planset-3 was
  deliberately local-dev-only: the GPU band skips where no driver is present, but
  there is no hosted pipeline and no automated validation gate. Validation errors
  still don't fail tests (the debug messenger only logs) — a CI gate would need to
  grep stderr for validation ERRORs, or promote them to failures behind a flag.
- **Known descriptor-pool / `UPDATE_AFTER_BIND` validation gap** (storage-image,
  and — surfaced by planset-3's `descriptor_write_paths` — sampled-image pool
  sizes): not a testing task but a real engine gap the tests now pin. It belongs to
  the [bindless/descriptor rework](bindless-descriptors.md), not area 5.

## Ordering & dependencies

A first cut at sequencing — the order to *take the areas up* (each becomes its own
planset), not a schedule. Refine when each is detailed.

Note that *testing* (area 5) isn't one block — its two halves sit at different
points, around the de-global change:

```
5a test harness + pure-logic tests ──► 3 de-globalize ──► 5b GPU/integration tests
                                                  └─► 2 threading ──► 1 asset system
4 events/input — independent, gameplay-driven (any time)
```

1. **Test harness + pure-logic tests (area 5, first half).** Framework + CTest
   wiring + the no-GPU unit tests (Result paths, typed-buffer/stride math,
   `ToVk`/`FromVk` round-trips, render-graph barrier-diff rule). These don't touch
   `Context::Instance()`, so de-globalizing does nothing for them — no reason to
   delay them, and they're the safety net for the sweep that follows. Satisfies
   "firm up testing soon".
2. **De-globalize the context (area 3).** Mechanical sweep across every `Create`
   call site — safest on the current single-threaded base with step 1 (plus the
   existing smoke tests/sample) as a net. Must precede threading: threads on top
   of a global mutable singleton invite races.
3. **GPU / integration tests (area 5, second half).** Written *after* de-global so
   they target the explicit-device API once (not rewritten when the singleton
   goes) and get real per-test isolation — stand up/tear down a context per case
   instead of sharing global state. (Isolation matters most for a unit framework
   running many cases in one process; a one-exe-per-test like `headless_smoke`
   already gets a fresh singleton per process.)
4. **Threading / task system (area 2).** Design against the explicit-device API;
   this is where the single-threaded v1 contract is deliberately lifted, and
   Vulkan-queue correctness is the hard part.
5. **Asset system (area 1) — headline, end of the chain.** The general asset API
   (types, handles, import/cook, *synchronous* load) is largely independent and
   could be scoped/started earlier, but the headline payoff — loading without
   stalling the frame — needs threading (step 4). Land the API early if
   convenient; async loading after threading.

**Event & input (area 4)** is off the critical path — independent of the
rendering/asset/threading work and driven by gameplay needs, so slot it in
whenever it's wanted.

Open question: how much of the asset API (definition + sync loading) to pull
forward in parallel with de-global/threading, vs. keeping the whole asset phase
last.

## Cross-cutting concerns (weigh when opening each phase)

Not areas of their own — considerations that span the work above and are cheaper
to decide early than to retrofit.

- **Design infrastructure against a real client.** De-global (3) and threading
  (2) risk being designed speculatively. Consider pulling a deliberately thin,
  *synchronous* asset-loading slice (1) forward — just enough to be a real
  consumer — so it surfaces the requirements that shape the context/threading
  APIs, rather than reworking them after the fact. Infrastructure built against an
  actual caller tends to be right. *(affects ordering of 1 / 2 / 3.)*
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
- **CI with a software Vulkan ICD** (lavapipe / SwiftShader) as part of the
  testing work, not after — otherwise the GPU/headless suite only ever runs
  locally. *(area 5.)*
- **The editor is the demanding second consumer.** hello-triangle (one pipeline,
  one push constant) won't surface multi-material/mesh/scene friction; the
  node-based editor will. Develop the editor and the engine API together so it
  exercises the asset/material surface as it's built — it doubles as the richer
  sample. *(area 1.)*
- **Pipeline caching.** Persist `VkPipelineCache` to disk once materials multiply
  — load-time win, naturally part of the asset/material phase. *(area 1.)*
- **Process discipline.** Keep planset-1's cadence — small, sample-verified, per-
  plan increments — especially for de-global (3), where a big-bang sweep is most
  tempting and most dangerous.

## Status

Vision only. Nothing detailed or scheduled. Each area becomes its own planset
when taken up. Revisit after planset-2.
