# Plan 07 — docs + roadmap re-cut

**Goal:** record what planset-18 delivered. Docs only — no code. Lands last, after 00–06
are green.

## What lands

### `CLAUDE.md`

- **Shaders & materials section** — replace the two-parallel-SSBO `MaterialData` + authored
  `MaterialParams` description with the **unified material block**: a material's bindless
  handle slots and authored params share one reflection-sized, ring-buffered per-material
  block (set 0 binding 4), patched by offset+kind, with an arbitrary shader-defined handle
  count (no fixed engine struct). Note that per-frame `SetParam`/`SetTexture` is safe and
  stall-free (N-buffered, host-mapped, dynamic-offset).
- **Shaders & materials section** — add the **material domain** concept: a `Material`
  carries a `MaterialDomain` (`Surface`/`PostProcess`) selecting its output contract,
  pipeline shape, standard vertex shader, and invocation site, with the parameter
  schema / bindless / authoring / inspector shared across domains. Note the lowercase
  `"domain"` `.vmat.json` key (default `surface`) and the cook-time fragment-output contract
  check.
- **SceneRenderer section** — note the **PostProcess fullscreen-material path**: a
  `PostProcessScenePass` builds a pipeline from a postprocess material's fragment shader
  against one color target, samples upstream targets via set-0 bindless, and drives the
  material's authored params; tonemap is the first such material. State the surviving line —
  fixed plumbing composites (`SwapChainCompositePass`, the debug blits) stay hardcoded engine
  passes; a postprocess material is for tunable effects.
- **Core pack / standard shaders** — the engine ships the standard vertex shader per domain
  in the core pack (`surface.vert`, `fullscreen.vert`); a game references the engine asset
  rather than shipping its own surface vertex stage.
- **Editor / material node editor** — the node catalog is **domain-aware**;
  `MaterialOutput`'s pins follow the domain's output contract.

### `plans/README.md`

Add the planset-18 entry after planset-17: the unified ring-buffered material parameter
block, material domains (Surface + PostProcess), the standard engine vertex shaders, the
PostProcess fullscreen-material path, tonemap-as-material, and the domain-aware node catalog —
area 13's prioritized first slice; node→Slang codegen remains the named follow-on.

### `plans/future/README.md`

- **Area 13** — mark the **material-domains slice delivered** (the unified ring-buffered
  parameter block, Surface + PostProcess, the PostProcess fullscreen-material path, the
  standard vertex shaders, tonemap-as-material, the domain-aware output node) by planset-18;
  keep **node→Slang codegen** as the still-future follow-on (every node an emitter, `Param`
  const-vs-exposed, compile → generated Slang).
- **Area 8** — note that the authorable post stack now has its mechanism (PostProcess
  materials); the remaining batteries (grade/bloom/SSAO/shadows as further passes or
  materials) stay future.
- **Ordering & dependencies / Status** — re-point "prioritized next" from the domains slice
  to **node→Slang codegen** (area 13's follow-on) and the **scene editor** (area 6 sub-D),
  whichever the next planset takes up.

### `plans/future/material-codegen.md`

- Note the **domains slice is delivered** (planset-18); the doc's section 1 (material
  domains) is realized, section 2 (codegen + the node reshape) remains the direction.
- Record that the **output-node inversion has begun** — `MaterialOutput` is domain-driven —
  so the codegen follow-on inherits a domain-correct sink and reshapes the **input** side
  (emitter nodes, const-vs-exposed params, generated-Slang compile target).

### `plans/planset-18/README.md`

Flip the status table to `done` and confirm the **On completion** paragraph matches what
landed.

## Verification

- Docs build/read cleanly; links resolve.
- No code, no test change. `git grep` confirms no lingering reference to a removed symbol
  (`TonemapScenePass`, `TonemapPushConstants`, `Renderer::MaterialData`, the example
  `brick.vert`) in prose that should now describe the unified-block / material path.
