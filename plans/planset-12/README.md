# planset-12 — the `SceneRenderer` (a deferred über-pipeline on `RenderGraph`)

**Phase goal:** deliver **`SceneRenderer`** — a long-lived, configurable render
pipeline that owns an offscreen target, renders a `Scene` from a `Camera` through
an **internal compiled `RenderGraph`** composed of **reusable, self-contained pass
units**, and hands back a sampleable result. On that shell, bring the pipeline from
hello-triangle's hand-rolled forward draw to a **minimal deferred chain** —
**g-buffer → directional-light pass → tonemap → output** — and add a directional
**`Light`** component to the scene model. This is **future area 8**
([future/README.md](../future/README.md#8-scene-renderer--render-pipeline-architecture),
[future/scene-renderer.md](../future/scene-renderer.md)).

Its prerequisites are in place: the **compiled `RenderGraph`**
([planset-8](../planset-8/README.md)) gives the builder/`Compile()`/replay lifecycle
the API is built around, the runtime **`Scene`/`Camera`**
([planset-10](../planset-10/README.md)) gives the per-frame input, and
[planset-11](../planset-11/README.md) shipped **`vengc generate-type-id`** (the
`TypeId` minter the new `Light` builtin uses, the analogue of `generate-id` for
asset ids).

## Terminology — "resolve" means one thing here

`RenderGraph` already owns the verb **resolve**: a pass callback *resolves* a
declared transient/import to its concrete `ImageView` this frame
(`PassContext::Resolved(id)`). To avoid overloading it, this planset does **not**
call the deferred-lighting stage a "resolve." The fullscreen pass that consumes the
g-buffer and applies the light is the **deferred lighting pass**
(`DeferredLightingScenePass`); "resolve"/`Resolved` is reserved for the graph's
view-resolution mechanic, and there is no MSAA resolve anywhere in v1.

## Ordering note — taken up before the editor

The roadmap sequences the scene renderer (area 8) **after** the editor (area 6),
because the editor's N viewport panels are its first and hardest consumer (one
`Scene` through N `SceneRenderer`s). planset-12 takes it up **first**, with
hello-triangle's single main view as the only consumer. The design supports a
single consumer unchanged — so the multi-renderer surface is **designed for and
unit-tested, not editor-wired**: the API admits N independent instances (each owning
its target, its own barrier domain), and a test constructs two over one `Scene`, but
the sample wires one. When the editor lands it consumes the same object N ways with
no API change.

## What this delivers, and what it deliberately holds back

A `SceneRenderer` constructed once with an **output format** + initial extent +
settings, owning the GPU resources its passes need and an internal `CompiledGraph`.
Its surface is the **lifetime split** (state separated by how often it changes):

| Phase | Rate | Job |
|---|---|---|
| `Create(info)` | once | Allocate persistent resources; build + compile the graph. |
| `Resize(extent)` | often | Recreate extent-sized images via the retire path; re-register them into bindless; recompile. |
| `Configure(settings)` | often | Recreate only affected resources; recompile topology. |
| `Execute(cmd, view)` | every frame | Replay the graph; pass callbacks record against this frame's `SceneView`. **Never** reallocates or recompiles. |
| `GetOutput()` | per consumer | Return the `Ref<ImageView>` of the owned result. |

The pipeline it wires is a **minimal deferred über-pipeline**: a g-buffer geometry
pass (material shaders write albedo + world-normal into an MRT g-buffer), a
fullscreen **deferred lighting pass** applying the scene's directional light into an
HDR target, and a **tonemap** pass mapping HDR → the output format. The passes are
**reusable, self-contained units** (`ScenePass`) the renderer wires in fixed order;
each knows only how to size its own resources, declare its reads/writes, and record
— it does not know what feeds it. A **`DebugView`** setting re-wires the pass set
(Final / Albedo / Normal / Depth), which is also the live proof that *settings drive
recompile* — a real topology change. (The Final / debug split is the no-throwaway
demonstrator; plan 02's albedo blit is a different thing — a deliberate verifiable
intermediate, superseded by plan 03's lighting pass.)

hello-triangle migrates its **main view** to render through one `SceneRenderer`,
dropping its hand-built scene graph + offscreen image; it keeps its composite
(now sampling `GetOutput()`) and ImGui exactly as before.

**Known cost (stated, not hidden):** the primary platform is macOS/MoltenVK, a
tile-based deferred GPU. The deferred chain stores a full-screen MRT g-buffer and
runs two-to-three additional fullscreen passes (lighting, tonemap, debug blits),
each a store-then-sample of a full-res target — strictly more bandwidth than the
forward draw it replaces for a one-cube scene. v1 trades that bandwidth for the
architecture; on-tile/subpass-fused deferred (the Apple-friendly form) is a future
optimization behind the same `ScenePass` mechanism, not a v1 goal.

What it **holds back** (named, not silently dropped):

- **The rest of the über-pipeline's "batteries."** Shadows, SSAO, bloom, MSAA, a
  transparent/forward pass, post stack — each is its own later increment behind the
  same `ScenePass` + `Configure`-recompile mechanism this planset establishes. v1 is
  the architecture + the **g-buffer → light → tonemap** spine, not the full feature
  set [scene-renderer.md](../future/scene-renderer.md) sketches.
- **Multiple / typed lights.** v1 adds a **single directional** `Light`; point/spot
  lights, multiple lights, and clustered/tiled light culling are future. The lighting
  pass reads **one** directional light from the `SceneView`.
- **Multi-viewport / N-renderer editor wiring.** Designed for and unit-tested
  (decision 7), not wired into the sample — the editor (area 6) is its consumer.
- **Frames-in-flight > 1.** v1 is **single-in-flight** (decision 6): single-copy
  renderer-owned images, the output not ring-buffered. The ring-buffer-when-FIF>1
  decision is recorded as a compile-time allocation note **and guarded by a
  `Create`-time assert**, not built — it has no consumer while the render thread is
  single.
- **Parallel pass recording** into secondary command buffers (area 2's seam): the
  `SceneView`-rides-the-record-context choice (decision 3) is made *for* it, but no
  parallel recording is built.
- **A material/shader hot-reload or editor-authored pipeline.** The über-pipeline is
  batteries-included, **not an extension mechanism** — authoring a bespoke pass graph
  still means dropping to `RenderGraph` directly (the hello-triangle pattern, which
  the sample's composite retains).

## Decisions

1. **`SceneRenderer` owns its target; it takes an output *format*, not a caller-owned
   target.** Sized to the consumer, recreated on `Resize` through the retire path,
   handed back as a sampleable `Ref<ImageView>` from `GetOutput()`. The game's
   swapchain path composites `GetOutput()` exactly as an editor panel will sample it —
   one handoff model. Passing in a caller-owned target re-introduces the
   resize-ownership tangle the lifetime split exists to avoid.
   `SceneRenderer` is **`Unique`** (single owner — nothing holds a `Ref` to one), per
   [docs/ownership.md](../../docs/ownership.md).
   - **`GetOutput()`'s `Ref<ImageView>` is invalidated by `Resize`/`Configure`** (the
     old image retires; a new one is created). Any consumer caching a bindless
     `TextureHandle` or ImGui texture built from it must re-fetch and re-register
     after those calls. The sample never resizes its `SceneRenderer` (fixed internal
     extent), so its one-time registration is valid; the editor (the N consumer) must
     re-register per resize. This is a contract, not an incidental.

2. **`Configure`/`Resize` are the recompile seams; `Execute` never recompiles.** A
   knob that changes **topology** (a pass on/off, `DebugView`) is a `Settings` field
   (→ `Configure` → recompile). A knob that changes **sizing** (extent) is `Resize`
   (→ recompile + recreate extent-sized resources). A value that varies **per frame**
   is a `SceneView` field (→ never recompiles). The corollary for empty work:
   settings-driven topology is compile-time, but **data-driven emptiness** ("no meshes
   this frame") is runtime — keep the pass compiled-in and record **zero draws**
   rather than recompiling. This matches `CompiledGraph`'s structural-change recompile
   model exactly: `Configure`/`Resize` rebuild the `RenderGraph` and re-`Compile()`;
   `Execute` replays.

3. **The `SceneView` rides the record-time context; `RenderGraph` stays
   scene-agnostic.** State the *what* and the *how* separately. *What:* in a compiled
   graph a pass callback **cannot capture its attachment views** (an aliased transient
   has no fixed backing) — it resolves them through `PassContext` at record time, and
   the per-frame `SceneView` (camera, scene, light, delta) rides that same
   record-time channel rather than a second one. *How:* `RenderGraph`/`PassContext`
   must **not** depend on the `Scene` layer (`include_hygiene`), so the channel is an
   **opaque per-`Execute` user pointer** on `PassContext` (forwarded through
   `CompiledGraph::Execute(cmd, imports, void* userData)`), which `SceneRenderer` sets
   to `&view` and its pass units read back through a typed `ScenePassContext` wrapper
   (`Cmd()`, `View()`, `Resolved(id)`).
   - **Checked, not conventional:** `ScenePassContext::View()` asserts the pointer is
     non-null before the reinterpret (`VE_ASSERT`), and `SceneRenderer` sets it on
     every `Execute`. A `void*` is less type-safe than the rejected options, so the
     "always set, always the right type" invariant is enforced at the wrapper, loud
     rather than UB.
   - **Rejected — `View()` directly on `RenderGraph::PassContext`:** couples
     `RenderGraph` to `Scene`, breaking the layering and `include_hygiene`.
   - **Rejected — stash `m_CurrentView` on the `SceneRenderer`:** the design's stated
     reason is **threading** — the stash idiom encourages per-frame scratch on the
     renderer, which is exactly what makes parallel pass recording unsafe. The user
     pointer is per-call data (no renderer scratch). Pick the parallel-friendly shape
     now, while recording is serial and the choice is free.

4. **Reusable pass units — `ScenePass`, not `IRenderPass`.** Each pipeline stage is a
   self-contained `ScenePass` (`Configure(settings)`, `Resize(extent)`,
   `Declare(RenderGraph&, const PassIO&)` — contribute its `RenderGraph` pass(es) +
   record callback into the renderer's **single internal graph**). The renderer owns
   the **wiring** (which pass reads whose g-buffer); each pass owns **itself**. The
   design sketch's `IRenderPass` is renamed `ScenePass` — the **`I` interface prefix
   is a kind-tag forbidden by the naming rule** (CLAUDE.md). The name is also distinct
   from `RenderGraph::Pass` deliberately: a `ScenePass` *contributes* `RenderGraph`
   passes, it is not one.
   - **`PassIO` is the wiring contract, and it is a named-slot struct, not a flat
     in/out pair.** The renderer hands each pass the specific resources it wired in
     (by role: the g-buffer ids/handles it reads) and the targets it must write (the
     output/HDR id). It carries resources flowing **both directions** (a pass may
     produce a resource a later pass consumes), and a `ScenePass` may declare
     **multiple** `RenderGraph` passes (so a future shadow pass = N settings-driven
     sub-passes + a produced shadow-map id; SSAO = a produced AO id the lighting pass
     consumes). `PassIO`'s shape is *defined* in plan 01 (the forward case is
     degenerate — one pass, the output id) and *first exercised with real wiring* in
     plan 02 (g-buffer ids threaded into the lighting pass); plan 01 freezes
     `Declare`'s signature against it, so the shape is named-slot from the start, not
     positional.

5. **Self-contained graph per renderer** (each `SceneRenderer` owns one internal
   `RenderGraph`), **not** contribute-to-a-caller's-shared-graph. Simpler, correct for
   N independent offscreen RTs (each its own barrier domain), and the shared-graph
   upside needs cross-pass reordering veng's linear no-reorder graph cannot exploit.
   "Reusable passes" is about *who authors the pass*, not *whose graph it lands in*.

6. **The renderer's pipeline images are renderer-owned and imported; v1 is
   single-in-flight.** The g-buffer (albedo, world-normal), the depth target, the HDR
   target, and the output are **renderer-owned `Image`/`ImageView`s**, allocated by
   `SceneRenderer` and **`Import`ed** into the internal graph (the graph does not
   allocate them). This is forced by the sampling path: a fullscreen pass samples an
   upstream target through the **bindless** set-0 array, which needs a `Ref<ImageView>`
   to `Register` — a graph-owned transient exposes only a per-frame `ImageView&`
   (`Resolved`), no `Ref`, so it cannot be registered. Renderer-owned images are
   registered into bindless **once** at `Create` (re-registered on `Resize`), and the
   pass that samples one gets its `TextureHandle` through `PassIO`. This is exactly the
   sample's existing cross-graph sampling pattern (own an image, register it once,
   import it). One `Execute` resolves and completes before the next begins; within a
   frame, written-then-read images (g-buffer, HDR) are correctly ordered by the graph's
   derived barriers, and the retire path covers destruction safety on
   resize/reconfigure. The **frames-in-flight > 1** hazard is the **output** image (a
   compositor sampling frame N while the renderer writes N+1) and any image read across
   an `Execute` boundary; the fix — ring-buffer those per frame-in-flight, a
   **compile-time allocation decision** — is **recorded and guarded**, not built: a
   `VE_ASSERT(Context::GetMaxFramesInFlight() == 1, …)` at `Create` makes a future FIF
   bump fail loudly at the exact place the ring buffer must be added, since the render
   thread is single and nothing drives FIF > 1 yet.

7. **Designed for N renderers, one wired.** The API admits N independent
   `SceneRenderer`s over one `const Scene&`. The `SceneView` is **not owned** by the
   renderer — `{ const Scene& World; const Camera& Camera; Light Light; f32 Delta; }`
   — where `Scene`/`Camera` are borrowed and the `Light` is a **per-frame value the
   renderer computes** (the selected directional light, or the default; see decision
   9). A **unit/gpu test constructs two** renderers over a single scene with two
   cameras, `Execute`s them **interleaved** (A, B, A in one command stream) to exercise
   each renderer's independent barrier domain (decision 5), and asserts their
   `GetOutput()`s are independent (each sized to its own extent, distinguishable
   content). hello-triangle wires **one** (its main view); the editor (area 6) is the
   real N consumer.

8. **The deferred material contract: a material's fragment shader writes the
   g-buffer — and this is the *minimum* g-buffer, not the final one.** Going deferred
   changes what an author's fragment shader outputs — from final color to **g-buffer
   channels** (albedo + world-space normal; depth is the depth attachment). The
   g-buffer layout is a fixed, documented contract the geometry pass's `RenderingInfo`
   and every material pipeline agree on (MRT color attachments + depth).
   hello-triangle's brick material shader is updated to write it. Two forward-looking
   constraints are stated now so later batteries do not force a silent breaking change:
   - **The v1 g-buffer is the minimum set, designed to grow.** PBR shading needs
     roughness/metallic/AO — a future **G2** target every material then also writes.
     The contract is documented as "the v1 channels," and the material's g-buffer write
     is funnelled through a single engine-provided `GBufferOutput` struct so adding a
     target is one place, not a per-material edit. Reserve the budget mentally now;
     don't pretend two targets is the final shape.
   - **The deferred material contract is not the forward/transparent contract.** A
     future transparent/forward pass needs materials whose fragment shader outputs
     *final color*, not g-buffer channels — that is a **second** material entry, not a
     breaking change to this one. v1 picks the deferred output as the *opaque* material
     contract, not the universal one.

   Set-0 bindless, `MaterialData`, and texture handles are unchanged — only the
   fragment shader's **outputs** move from swapchain color to the g-buffer.

9. **A directional `Light` builtin component, registered like every other builtin.**
   `Light { vec3 Direction; vec3 Color; f32 Intensity; }` joins
   `Name`/`Transform`/`Parent`/`CameraComponent`/`MeshRenderer` in `BuiltinTypes`,
   `VE_REFLECT`-described with a minted `TypeId` (`vengc generate-type-id`),
   pre-registered by `RegisterBuiltinTypes` (GPU-free, unchanged contract).
   `SceneRenderer::Execute` selects the scene's directional light into the `SceneView`
   **by value**: the first `Light` entity, or a documented default when none — a
   zero-intensity directional light plus the lighting pass's small ambient term, so a
   scene with no light renders flat-ambient (never pure black, never asserts). The
   lighting pass reads it through `ScenePassContext::View()`.

10. **The golden capture is regenerated where the look stabilizes, and the GPU tests
    are the automated oracle for the deferred plans.** Going deferred + lighting
    changes the rendered pixels. Plan 01 (forward shell) keeps the capture
    **byte-identical** — `smoke_golden` is the proof the migration changed nothing.
    Plans 02–04 change the pixels; each regenerates `hello_triangle_scene.png` per the
    CLAUDE.md procedure, so for those plans `smoke_golden` only re-asserts
    reproducibility (it compares against a golden the same change just produced — a
    weak oracle). The **real** per-plan correctness gate is the GPU test, which asserts
    **several spatially-spread sample points** (a lit face, an ambient/shadowed face, a
    known-normal texel, the background) **plus a cheap whole-frame invariant** (mean
    luminance in an expected range), so a global error — a flipped normal channel, a
    gamma slip, a wrong tonemap curve — is caught automatically, not left to a human
    eyeballing the regenerated PPM. The smoke pose stays fixed (`SmokeAngle`), so the
    capture stays reproducible.

## Scope of this phase

| In scope | Out of scope (later / other phases) |
|---|---|
| `SceneRenderer` (`Create`/`Resize`/`Configure`/`Execute`/`GetOutput`), `SceneRendererInfo`/`SceneRendererSettings`/`SceneView`; owns its offscreen output image + internal `CompiledGraph`; `Unique`, single-owner | A caller-owned target; multiple output attachments; an extension/plugin pass API (it is batteries-included, not extensible — drop to `RenderGraph` for bespoke graphs) |
| The `ScenePass` reusable-pass-unit abstraction (`Configure`/`Resize`/`Declare`) + the renderer's fixed wiring; the named-slot `PassIO`; `ScenePassContext` (`Cmd`/`View`/`Resolved`) | Parallel pass recording into secondary command buffers (area 2); cross-renderer / shared-graph scheduling |
| The opaque user-pointer record-time channel on `PassContext` + `CompiledGraph::Execute(..., void* userData)` (the one `RenderGraph` change), with a non-null assert at the `ScenePassContext` wrapper | Any `Scene`-layer dependency in `RenderGraph`/`PassContext`; a `Ref<ImageView>` accessor for transients (renderer-owned imported images sidestep it) |
| The minimal deferred chain: g-buffer geometry pass (MRT albedo+normal, depth), deferred directional-light pass (→ HDR), tonemap (HDR → output); the **deferred opaque material g-buffer contract** + the updated brick shader | Shadows, SSAO, bloom, MSAA, transparent/forward pass (a second material contract), a post stack — later batteries behind the same mechanism; a G2 PBR-params target |
| A directional `Light` builtin component (reflected, `RegisterBuiltinTypes`-registered, serializer-covered); `SceneView` light selection by value | Point/spot/area lights, multiple lights, light culling (clustered/tiled), shadow maps |
| `SceneRendererSettings` incl. a `DebugView` selector driving `Configure` recompile (Final/Albedo/Normal/Depth) — the settings-drive-recompile proof | A general post-processing settings surface; per-pass settings beyond what the v1 passes need |
| hello-triangle migrated: main view through one `SceneRenderer`, composite samples `GetOutput()`, ImGui unchanged; a 2-renderer interleaved unit/gpu test (design-for-N proof) | Multi-viewport editor wiring; a second renderer in the sample UI; frames-in-flight > 1 / ring-buffered output (recorded + asserted, not built) |

## Plans

| # | Plan | Summary | Status |
|---|---|---|---|
| 01a | [The `RenderGraph` record-time channel](01-scenerenderer-shell.md#01a) | Add the opaque `void* userData` to `PassContext`/`CompiledGraph::Execute` (defaulted, no new include, no `Scene` dep). Update existing callers (composite, graph tests) — they pass nothing. Independently committable; the graph tests stay green. | done |
| 01b | [The `SceneRenderer` shell — lifetime split + `ScenePass` framework](01-scenerenderer-shell.md#01b) | The `SceneRenderer` class + `Info`/`Settings`/`SceneView`; owned **imported** output image + internal `CompiledGraph`; the `ScenePass` abstraction + the named-slot `PassIO` + `ScenePassContext` (with the non-null `View()` assert). Wire **one forward pass unit** = today's draw, migrate hello-triangle's main view. **Pixels unchanged** (output image created at `GetOutputFormat()`; `smoke_golden` proves it). | done |
| 02 | [The deferred g-buffer pass + the material g-buffer contract](02-deferred-gbuffer.md) | Convert the geometry pass to write an **MRT g-buffer** (albedo + world-normal + depth) into renderer-owned **imported** images; define + document the **deferred opaque material fragment-shader contract** (the minimum g-buffer, via `GBufferOutput`); update hello-triangle's brick shader. An `AlbedoBlitScenePass` keeps it renderable in isolation. Golden regenerated (unlit deferred); GPU oracle asserts the albedo + whole-frame invariant. | done |
| 03 | [The directional `Light` + the deferred lighting pass](03-light-deferred-resolve.md) | Add the `Light` builtin (reflected, `RegisterBuiltinTypes`, serializer round-trip); `SceneView` light selection by value; a fullscreen **deferred lighting pass** (g-buffer + light → HDR) replacing 02's albedo blit, plus an explicit `HdrBlitScenePass` (HDR → output) as the stand-in tail. Golden regenerated (properly lit); GPU oracle asserts the lit/ambient texels. | done |
| 04 | [The tonemap pass, `Settings`/`DebugView` recompile, the design-for-N test](04-tonemap-settings.md) | A **tonemap** pass (HDR → output) swapping 03's `HdrBlitScenePass`; `SceneRendererSettings` incl. the `DebugView` selector that re-wires passes through `Configure` (recompile proven by output-pixel diff, not pass-count introspection); the single-in-flight contract + the `Create`-time FIF assert; a **two-renderer interleaved** unit/gpu test. Golden regenerated (final tonemapped). | proposed |
| 05 | [Docs + roadmap re-cut](05-docs-roadmap.md) | `plans/README.md` (planset-12 line); `future/README.md` (area 8 **DONE**; remaining batteries/lights/FIF named future); `future/scene-renderer.md` (delivered vs. still-future); `CLAUDE.md` (a `SceneRenderer` section + the deferred opaque material contract + the `Light` builtin). | proposed |

## Dependencies & dispatching

One ordered chain — each plan builds on the last:

```
01a (RenderGraph void* userData channel; callers updated; tests green)
   ──► 01b (shell + ScenePass + PassIO, forward, golden unchanged)
       ──► 02 (deferred g-buffer + material contract + albedo blit)
           ──► 03 (Light + deferred lighting pass + HDR blit tail)
               ──► 04 (tonemap swaps the blit + Settings/DebugView recompile + N-test)
                   ──► 05 (docs)
```

- **Recommended single-threaded order:** `01a → 01b → 02 → 03 → 04 → 05`, the house
  "one plan per session" cadence. 01a + 01b are two commits in one session if it
  flows, or two sessions if 01b runs long — 01a lands and verifies on its own.
- **Keep on the main thread:** the contract-setting plans — **01a/01b** (the
  record-time channel, the lifetime split, the `ScenePass`/`PassIO` abstraction),
  **02** (the deferred material g-buffer contract + the MRT layout), **03** (the
  `Light` builtin + the lighting pass), and **04** (the `Settings`/recompile seam +
  the single-in-flight contract) — plus **05** (docs).
- **Good `model: sonnet` delegation** once those contracts are fixed: **02**'s brick
  shader edit + the MRT pipeline plumbing (given the documented g-buffer layout +
  formats), **03**'s `VE_REFLECT`/serializer wiring for `Light` and its round-trip
  test (given the struct + `TypeId`), and **04**'s `DebugView` pass re-wiring + the
  two-renderer test (given the `Settings` shape). Keep the lifetime split, the
  record-time channel, the `PassIO` shape, the material contract, and the recompile
  seam on the main thread.

## Process & conventions

Same cadence as every planset: implement → migrate `examples/hello-triangle` in the
same pass → verify (clean build, `ctest` green, smoke binary writes a correct-sized
1280×720 RGB PPM ≈ 2,764,816 bytes) → update this table → one commit per plan,
`Plan NN: <summary>` with a `Co-Authored-By` trailer (`planset-12:` for the docs
plan).

- **Public headers stay backend-free.** The new public headers (`SceneRenderer`,
  `ScenePass`/`ScenePassContext`, `SceneView`, the `Light` component) are pure CPU —
  no `vk::`/VMA/GLFW — and are **hand-added to `tests/include_hygiene.cpp`**. The one
  `RenderGraph` change (the opaque `void* userData` on `PassContext`/`Execute`)
  introduces **no** new include. Every plan keeps `include_hygiene` green.
- **`RenderGraph` gains no `Scene` dependency.** The `SceneView` reaches pass units
  only through the opaque user pointer; `SceneRenderer` (the `Scene`-aware layer) owns
  the wrapper that types it (decision 3).
- **Sampling an upstream target goes through bindless.** A fullscreen pass reads the
  g-buffer/HDR through set-0 bindless, indexed by a `TextureHandle` the renderer
  registered once for its owned imported image and threaded in via `PassIO` (decision
  6). No per-frame bindless churn, no new `RenderGraph` API.
- **Verify the depth-as-texture barrier.** The g-buffer depth is **both** a depth
  attachment (geometry pass) and a sampled input (lighting pass) — the first depth
  read-as-texture in the engine. Plans 02–03 confirm the graph derives the
  depth-attachment → shader-read transition (validation gate green); if it does not
  yet, that derivation is a hidden prerequisite to land first.
- **The deferred material contract is documented where it is defined.** The g-buffer
  layout (channels, formats, which attachment is which) is stated as a present-tense
  fact next to the geometry pass and the material pipeline (no plan citations in code
  comments — CLAUDE.md).
- **GPU plans pass the validation gate.** Plans 02–04 draw and must pass
  `ctest --test-dir build-debug -L validation`; the deferred path adds MRT + new
  fullscreen passes, so each plan **verifies the allowlist stays empty** (no new
  validation errors). Plan 01 is a pure migration — same pixels, same barriers.
- **The smoke PPM is non-deterministic in bytes — verify size + exit 0.** The smoke
  pose is fixed, so `smoke_golden` applies: plan 01 keeps it **unchanged** (the
  migration's proof); plans 02–04 **regenerate** it (decision 10), with the GPU tests'
  multi-point + whole-frame assertions as the real automated oracle for those plans.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and
> approved; `done` = landed and verified.

## On completion

veng has a long-lived, configurable `SceneRenderer`: constructed with an output
format + settings, owning an offscreen target and an internal compiled graph of
reusable `ScenePass` units, it renders a `Scene` from a `Camera` through a deferred
**g-buffer → directional-light → tonemap** chain and hands back a sampleable result.
hello-triangle renders its main view through one instance; a test proves N instances
over one scene; the editor (area 6) inherits the multi-viewport consumer solved. Mark
**area 8 delivered** in [future/README.md](../future/README.md) and re-cut
[scene-renderer.md](../future/scene-renderer.md) (shell + minimal-deferred spine
delivered; the remaining batteries — shadows/SSAO/bloom/transparent/post, multiple &
typed lights, a G2 PBR g-buffer target, frames-in-flight > 1 with ring-buffered
output, and parallel pass recording — named as the natural next increments behind the
same mechanism). The **editor** (area 6) and **events/input** (area 4) remain the open
plansets.
