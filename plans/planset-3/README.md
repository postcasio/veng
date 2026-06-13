# planset-3 — future work (DRAFT / vision)

> Draft vision doc, not scheduled — planset-2 comes first. This is a **holding
> area** for the larger phases beyond the surface cleanup. It will be **split
> into several dedicated plansets** (3, 4, …) as each area is taken up and
> detailed planset-1 style; the sections below are direction, not plans.

Captured now so the earlier phases stay coherent with where veng is going.

## Areas

### 1. Material system (the headline next phase)

Make the **material** the primary rendering interface, not the shader. A material
bundles a shader (binary) with the uniform/texture data it needs; you bind and
draw a material rather than juggling pipelines, descriptor sets, push constants
and shader layouts by hand.

Two ways to get one:

1. **Loaded (editor-authored).** A node-based material editor produces a material
   asset carrying the shader binary plus required uniform values and texture
   references. The runtime loads it ready to bind — no manual layout wiring.
2. **Constructed (programmatic).** Reference a shader and explicitly supply the
   uniform/texture info; the engine validates it against what the shader needs
   and builds the descriptor sets / push-constant data.

This phase **absorbs all the deferred shader work** (the reason planset-1/12 and
the shader parts of planset-2 were dropped):

- **Offline shader reflection → serializable `ShaderInterface`** (descriptor
  bindings, push-constant blocks, vertex inputs), produced by the importer at
  cook time — not at runtime; editor and runtime both read it.
- Descriptor/pipeline layouts derived from the interface; name-based binding.
- Vertex layout derived from / validated against the shader's vertex inputs.

Likely sub-plans: shader-interface + reflection step; `Material`/`MaterialInstance`
runtime + pipeline caching; material asset format + serialization; the node-based
editor (its own tooling area); hot-reload and shader variants.

### 2. De-globalize the rendering context

`Context::Instance()` is a global singleton reached by every resource
constructor (`Buffer::Create` etc. secretly grab it). It's the biggest
"not-modern" smell: it blocks more than one device, hides the dependency, and
couples tests to global state. planset-1 kept it deliberately; this phase removes
it.

Direction: thread an explicit device/context into resource creation
(`device.CreateBuffer(info)` or `Buffer::Create(context, info)`), or a scoped
"current device" for ergonomics. Large, mechanical-but-pervasive change touching
every `Create` call site — its own planset. Best done before/with the asset
system, which will want device-free editor code paths anyway.

### 3. Event & input systems

Both are thin and partially stubbed today:

- **Events:** `EventType` declares `WindowFocus/LostFocus/Moved` with no event
  classes; only resize/close exist. The `EVENT` macro + virtual dispatch is heavy
  for two events.
- **Input:** lives ad-hoc on `Window` (`KeyPressed`, `GetMousePosition`);
  `MouseButton` is defined but unused; no actions/bindings/devices abstraction.

Revisit when gameplay drives the requirements — design a real input abstraction
(polling + actions, multiple devices) and decide whether the event system grows
or is replaced. Its own planset.

### 4. Unit testing / test infrastructure (firm up soon)

Today's `tests/` are two CTest smoke/compile guards (`include_hygiene`,
`headless_smoke`) — there's no unit-test framework and no coverage of pure logic.
Stand up a real test setup:

- **Framework:** a lightweight header-only one (doctest or Catch2). Note the
  constraint: `veng` builds `-fno-exceptions` (PRIVATE), but test executables are
  separate targets that link `veng::veng` and *don't* inherit that flag, so a
  throwing framework is fine in test TUs.
- **Pure-logic coverage** (no GPU): `Result`/error paths, typed-buffer size math
  and `VertexBufferLayout` stride, `ToVk`/`FromVk` vocabulary round-trips, render
  graph barrier-decision logic (extract the diff rule so it's testable without a
  device).
- **Fatal-assert (death) tests:** `VE_ASSERT` aborts, so test these as separate
  processes via exit code (CTest `WILL_FAIL` / a small fork harness) — e.g.
  uploading u16 indices into a U32 `IndexBuffer`, descriptor type mismatch.
- **GPU-backed tests** via the headless context (extend the `headless_smoke`
  pattern): render-graph correctness, upload/download round-trips.
- **CI:** run the suite; the headless/GPU tests need a Vulkan ICD
  (MoltenVK / lavapipe / SwiftShader) — gate or skip gracefully where absent.

## Status

Vision only. No plans detailed, nothing scheduled. Each area above becomes its
own planset when taken up. Revisit after planset-2.
