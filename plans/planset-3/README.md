# planset-3 — material system (DRAFT / vision)

> Draft vision doc. Not scheduled — planset-2 comes first. Captured now so the
> earlier phases stay coherent with where the rendering API is going.

**Phase goal:** make the **material** the primary rendering interface, not the
shader. A material bundles a shader (binary) with the uniform/texture data it
needs; you bind and draw a material rather than juggling pipelines, descriptor
sets, push constants and shader layouts by hand. This is where the deferred
shader-facing work (reflection, derived layouts, name-based binding) finally
lands — in service of materials.

## The two ways to get a material

1. **Loaded (editor-authored).** A node-based material editor produces a material
   asset containing the shader binary plus the required uniform values and
   texture references. The runtime loads it and it's ready to bind — no manual
   layout/descriptor wiring.
2. **Constructed (programmatic).** Reference a shader and explicitly supply the
   uniform/texture info. The engine validates the supplied data against what the
   shader actually needs and builds the descriptor sets / push-constant data for
   you.

Both paths converge on the same runtime `Material` and the same descriptor /
push-constant machinery; only the source of the parameter data differs.

## What this phase absorbs (deferred from planset-1/2)

- **Offline shader reflection → serializable `ShaderInterface`** (descriptor
  bindings, push-constant blocks, vertex inputs). Produced by the material/asset
  importer at cook time, *not* at runtime; the editor and the runtime both read
  it. This is the old planset-1 plan 12, reframed as material-system groundwork.
- **Derived descriptor & pipeline layouts** from the shader interface; a
  material's parameters bind by **name**.
- **Vertex layout** derived from / validated against the shader's vertex inputs.
- `PipelineShaderStageInfo::Stage` etc. — stage and layout come from the
  interface, not restated.

## Likely sub-areas (to become real plans later)

- Shader interface description + offline reflection tool / importer step.
- `Material` / `MaterialInstance` runtime types (shared shader+pipeline, per-
  instance parameter data); pipeline caching/identity.
- Material asset format (shader ref + named uniform values + texture/sampler refs
  + render state: blend/cull/depth) and its on-disk serialization.
- Node-based material editor (tooling; almost certainly its own phase/repo area).
- Hot-reload and shader variant/permutation handling.

## Open questions

- Division between veng-core material runtime and editor-only authoring types.
- How render state (blend/cull/depth, attachment formats) is authored: in the
  material vs. by the render graph pass that draws it.
- Pipeline permutations (defines/specialization constants) and their caching.
- Relationship to the asset system generally (this phase may be folded into a
  broader asset-pipeline phase).

## Status

Vision only. No plans detailed, nothing scheduled. Revisit after planset-2.
