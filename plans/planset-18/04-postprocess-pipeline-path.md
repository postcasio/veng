# Plan 04 — PostProcess fullscreen-material pipeline path

**Goal:** stand up the runtime mechanism the PostProcess domain needs — a `ScenePass` that
builds its graphics pipeline from a PostProcess **material's** fragment shader against a
single color target, binds set-0 bindless, samples an upstream target through a runtime-bound
material handle field, and drives the material's authored params. This is the reusable path;
plan 05 makes tonemap its first real consumer.

## The shape today, and the gap

[SceneRenderer.cpp](../../engine/src/Renderer/SceneRenderer.cpp) already has the fullscreen
pattern, but always over a **hardcoded core fragment shader** with a **bespoke
push-constant block**: `TonemapScenePass` builds `m_TonemapPipeline` from the core
`tonemap.frag` and pushes a `TonemapPushConstants { hdr slots, exposure }`; the
`FullscreenBlitScenePass` family does the same per debug view. Each effect is C++ — a
pipeline, a push struct, a `ScenePass` subclass. A material cannot supply a fullscreen
effect because nothing builds a fullscreen pipeline from a *material's* shader or routes a
material's params/handles into it.

The Surface domain already proves the material→pipeline path: `Material` builds a
`GraphicsPipeline` from its reflected vertex+fragment shaders with set 0 reserved for the
bindless registry, and the per-draw material index (pushed at the selector offset) selects
the material's unified parameter-block entry. PostProcess reuses **all** of that — the
only differences are the **pipeline shape** (fullscreen triangle, one color target, no
vertex inputs / the screenspace layout) and the **invocation** (recorded by a post-chain
`ScenePass`, not a per-submesh draw).

## What lands

### `PostProcessScenePass`

A new reusable `ScenePass` (new files under `engine/src/Renderer/`, or a nested class in
`SceneRenderer.cpp` alongside the existing passes — match the existing organisation, which
keeps the scene passes private to `SceneRenderer.cpp`). It is constructed with:

- the PostProcess **`AssetHandle<Material>`** to invoke (a handle, not a `Ref` — the renderer
  holds it across `Configure`/`Resize`, and it stays stable behind a hot-reload remount, the
  cook-on-demand path the material preview relies on),
- the named input slot(s) it samples (an upstream `Import`ed target — e.g. the HDR lighting
  result), supplied through the renderer's `PassIO` wiring as a `TextureHandle`, exactly as
  the lighting pass receives the g-buffer handles,
- the output color target, its **format** (the pass builds its pipeline against this — see
  "Building the fullscreen pipeline"), and extent.

Its `Declare` adds one graph pass that:

- writes the single output color attachment (`.Color(output)`),
- reads the upstream input(s) (`.Sample(...)`) so the graph derives the barrier,
- in `Record`: binds set 0 (`registry.Bind(cmd)`), binds the pass's fullscreen pipeline,
  writes the sampled upstream target's bindless index into the material's runtime-bound input
  **handle field** (so the fragment shader reads it through `g_Textures[...]` like any
  material texture), pushes the material's per-draw selector index, and
  `cmd.DrawFullscreenTriangle()`.

The postprocess fragment shader reads its handles (the input texture/sampler) and its
authored params (the exposed knobs, e.g. exposure) from the **one unified material block**
(plan 00) through the standard set-0 path. The per-frame handle write lands in the
ring-buffered block (plan 01) safely. The only push is the selector index at the domain's
selector offset (see decision 2).

### Building the fullscreen pipeline from the material

A PostProcess material's pipeline must be the **fullscreen shape** (screenspace VS / no
vertex inputs, one color target, no depth), and its color target **format is the renderer's**
— the pass's output (the renderer's output format) or an intermediate HDR format — **not a
constant the loader can know**. So the build splits by where the information lives:

- The `MaterialLoader` reflects the material and builds its **pipeline layout** (set-0
  reservation, the selector push range) for both domains, and for **Surface** builds the
  `GraphicsPipeline` against the fixed g-buffer formats as today.
- For **PostProcess** the loader does **not** build the `GraphicsPipeline` (it has no target
  format). The `PostProcessScenePass` builds the fullscreen `GraphicsPipeline` from the
  material's reflected shaders + pipeline layout against **its own color format** (passed in
  at construction), exactly as the existing hardcoded fullscreen passes build theirs from
  `m_OutputFormat`. The pass binds that pipeline; the material supplies the layout, the
  set-0 block, and the selector index.

This keeps format ownership with the renderer (where the format is known) and still builds
the pipeline from the *material's* shaders — the planset's "material pipeline, not a new
shader path" decision holds; only the `GraphicsPipeline::Create` call (which needs the
format) moves from the loader to the pass for this domain.

### The input-target handle binding

A postprocess effect samples an upstream target that is **renderer-owned**, addressed by a
bindless index the renderer assigns. The material's input is a **runtime-bound handle field**
(plan 00 makes a handle field with no cooked asset a first-class case): the cooker emits it
as a handle field with no resolved id, the loader leaves its slot zero (loads no texture
dependency), and the pass writes the live upstream index into it each frame via
`Material::SetTexture`-by-name. That per-frame write lands in the ring-buffered block (plan
01) — the current frame's region, no stall, no hazard. The renderer transitions the sampled
target for the read via the graph's derived barrier (the `.Sample` declaration), as the
lighting pass does for the g-buffer.

### A placeholder consumer to wire it

To land and test the mechanism independently of plan 05, the `gpu` test drives **one trivial
identity PostProcess material** — a fragment shader that samples its one runtime-bound input
and writes it unchanged — as a **test-only fixture** (cooked by the test, not added to the
core or example pack), proving: build-from-material, set-0 bind, upstream sample, param read,
fullscreen draw, output written. Nothing is wired into the shipping `Final` chain in this
plan, so `smoke_golden` cannot move; plan 05 is where tonemap joins the real chain.

## Decisions

1. **The domain branches the pipeline *layout* at load; the PostProcess *pipeline* builds in
   the pass.** The loader reflects both domains and builds the pipeline layout for each;
   Surface also builds the `GraphicsPipeline` (fixed g-buffer formats), as today. PostProcess
   defers `GraphicsPipeline::Create` to the `PostProcessScenePass`, which alone knows the
   color format. The renderer does not rebuild a *surface* pipeline; it builds the
   *postprocess* one from the material's shaders against its own target format — the only
   clean place that format is known.

2. **The selector push offset is domain-keyed.** `Material` already carries a per-material
   `m_SelectorOffset` (pushed in `Material::Bind`); it is hardcoded to 64 at load because a
   surface push block is `MVP + NormalMatrix` (64 bytes) then the `u32` index. A PostProcess
   shader has no geometry block, so its selector sits at **offset 0** — a 4-byte push range,
   no dead MVP padding. The loader sets `SelectorOffset` from the domain (Surface → 64,
   PostProcess → 0) and the `selectorCovered` guard
   ([MaterialLoader.cpp:95](../../engine/src/Asset/Loaders/MaterialLoader.cpp)) checks the
   *domain's* offset. Forcing a fullscreen shader to declare a 64-byte MVP-shaped push block
   just to land the index at 64 would be 60 dead bytes per shader — the branch is both less
   code and the honest contract.

3. **Inputs flow through the material's bindless handle field, not a new push block.** A
   postprocess effect's scene-texture input is a material **handle field** whose bindless
   index the pass rewrites per frame (runtime-bound, plan 00). This keeps the postprocess
   fragment shader a plain bindless material shader reading the unified block, and means the
   editor inspector and the node catalog (plan 06) see a postprocess material's inputs and
   params through the same `GetFields()` they already use. The bespoke per-effect push struct
   (`TonemapPushConstants`) is retired in plan 05.

4. **`PostProcessScenePass` is reusable; the renderer owns the wiring.** Per the
   `SceneRenderer` contract, a `ScenePass` knows how to record, never what feeds it — the
   renderer's `PassIO` names which upstream target the postprocess pass samples. So one
   `PostProcessScenePass` type drives any postprocess material; the chain (tonemap, then a
   future grade/bloom) is a list of them the renderer wires, the named post-stack seam.

5. **Fixed plumbing stays hardcoded (planset decision 5).** This pass is for *materials*
   (tunable effects). `SwapChainCompositePass` and the `DebugView` blits are not materials
   and are untouched — they remain hardcoded engine passes.

## Files

| File | Change |
|---|---|
| `engine/src/Renderer/SceneRenderer.cpp` | New `PostProcessScenePass` (builds its fullscreen pipeline from a material's shaders against its color format; binds set 0; runtime-binds the input handle; selector push; fullscreen draw). |
| `engine/src/Asset/Loaders/MaterialLoader.cpp` | Build the pipeline *layout* for both domains; build the `GraphicsPipeline` only for Surface; set `SelectorOffset` from the domain and guard `selectorCovered` against the domain's offset; accept a runtime-bound (id-less) handle field. |
| `engine/include/Veng/Renderer/ScenePass.h` | If `PostProcessScenePass` is exposed (vs nested), declare it; otherwise unchanged. |
| `tests/gpu/*` + a test-only identity postprocess material fixture | A GPU test cooking + driving an identity PostProcess material fullscreen and asserting it samples an upstream target and writes it through unchanged (extends the existing `SceneRenderer`/two-renderer GPU coverage). |

## Verification

- Clean build; the test-only identity postprocess material loads (layout at load, pipeline
  built by the pass against its format) and the test chain executes.
- `gpu` band: the postprocess pass samples its upstream input through the runtime-bound
  handle and writes it; `validation_gate` (under `build-debug`) is clean — the derived
  barrier transitions the sampled target correctly, and the domain-keyed selector push range
  is valid (no unallowlisted validation error).
- Smoke PPM correct size + exit 0; **`smoke_golden` unchanged** — nothing is wired into the
  shipping `Final` chain in this plan (the identity material is test-only). Plan 05 is where
  tonemap joins the real chain and governs the golden.
