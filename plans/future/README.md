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
  validated against what the shader needs).
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

Today's `tests/` are two CTest smoke/compile guards (`include_hygiene`,
`headless_smoke`) — no unit-test framework, no pure-logic coverage. Stand up a
real setup:

- **Framework:** lightweight header-only (doctest/Catch2). Constraint: `veng`
  builds `-fno-exceptions` (PRIVATE), but test executables link `veng::veng` and
  don't inherit it, so a throwing framework is fine in test TUs.
- **Pure-logic coverage** (no GPU): `Result`/error paths, typed-buffer size math
  and `VertexBufferLayout` stride, `ToVk`/`FromVk` round-trips, render-graph
  barrier-decision logic (extract the diff rule so it's device-testable).
- **Death tests:** `VE_ASSERT` aborts → test as separate processes via exit code
  (CTest `WILL_FAIL` / fork harness) — e.g. u16-into-U32 index buffer, descriptor
  type mismatch.
- **GPU-backed tests** via the headless context (extend `headless_smoke`).
- **CI:** GPU tests need a Vulkan ICD (MoltenVK / lavapipe / SwiftShader) — gate
  or skip gracefully where absent.

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

## Status

Vision only. Nothing detailed or scheduled. Each area becomes its own planset
when taken up. Revisit after planset-2.
