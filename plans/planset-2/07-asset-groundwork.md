# 07 — Asset/material format groundwork (design only)

**Goal:** a written design (not code) for the on-disk shader/material asset the
future importer produces and the editor/material tools consume — so the
`ShaderInterface` from plan 01 is asset-ready and planset-2's derivations line up
with where the asset system is going. This anchors the *next* phase.

**Dependencies:** 01 (the interface is the core of the shader asset). Informs but
does not block 02–06.

## Why now

We keep making choices (offline reflection, derived layouts, push-constant
blocks) whose payoff is an editor-driven material workflow. Writing the asset
shape down now keeps those choices coherent and surfaces requirements early —
without committing to building the asset system yet.

## Scope of the design note

- **Cooked shader asset:** SPIR-V + `ShaderInterface` (plan 01) + source hash /
  build metadata. Identity, versioning, hot-reload story.
- **Material asset:** which shaders, parameter values bound by *name* (ties to
  plan 02 name-based writes), texture/sampler references, render state (blend,
  cull, depth) that today lives in `DynamicPipelineInfo`. What's authored in the
  editor vs derived from the shader interface.
- **Importer responsibilities:** where reflection runs, what's baked, how the
  build-time sidecar (plan 01 bridge) migrates into the importer with no runtime
  change.
- **Editor needs:** reading interfaces/materials without a live device; the
  `ShaderInterface` member/type info needed to render a parameter UI.
- **Open questions:** pipeline caching/identity, variant/permutation handling,
  hot-reload invalidation, separation between veng-core and editor-only types.

## Deliverable

A `docs/` design document (and any small `ShaderInterface` adjustments it implies
fed back into plan 01). No runtime/editor code in this plan — it exists to make
the asset-system phase start from an agreed shape.

## Acceptance

- A reviewed design doc exists; plan 01's `ShaderInterface` is confirmed
  sufficient (or amended) to back both the material editor and runtime material
  loading.
