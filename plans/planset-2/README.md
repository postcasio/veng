# planset-2 — rendering API surface cleanup

**Phase goal:** trim the rendering API surface that planset-1 left behind —
remove "say it twice" duplication at call sites and retire vestigial public
types — *without* touching shaders. Shader-facing work (reflection, derived
descriptor layouts, name-based writes, vertex layout from the shader) is
deliberately deferred: it belongs to the asset/material phase, drafted in
[future/](../future/README.md).

## Scope decision (2026-06-12)

Shader changes are pushed to a later phase again. The material (part of a broader
asset system) — not the shader — becomes the primary authoring interface (see
[future/](../future/README.md)), so the shader-interface/reflection work is
designed there rather than here. planset-2 is limited to cleanups that stand on
their own.

## Plans

| # | Plan | Status |
|---|------|--------|
| 01 | [Push-constant layout & buffer](01-push-constants.md) | ready |
| 02 | [Pipeline attachment formats from render targets](02-attachment-formats.md) | ready |
| 03 | [Retire the legacy render-pass pipeline surface](03-retire-render-pass.md) | ready |
| 04 | [Minor API cleanups & consistency](04-minor-cleanups.md) | ready |
| 05 | [Compute dispatch](05-compute-dispatch.md) | ready |

All independent — they can land in any order (04's pipeline rename is sequenced
after 03).

## Duplication / surface this phase removes

1. Push-constant range declared at pipeline-layout creation *and* restated in
   every `cmd.PushConstants(...)` → declare the range once on the pipeline layout
   and push typed data with `cmd.PushConstants(value)`, which recovers
   layout/stage/size from the bound pipeline. No buffer/layout wrapper types;
   push constants are an engine-fixed channel, so no reflection.
2. Pipeline `ColorAttachments` formats restated to match the render target →
   derive/validate against the render-graph pass.
3. `GraphicsPipeline`/`RenderPass`/`Framebuffer` public but used only by the
   internal ImGui path → port ImGui to dynamic rendering and delete them (also
   removes the last public `ImageLayout`).
4. Assorted small ergonomics & consistency (frame API, `VertexBufferLayout`
   vocabulary, CommandBuffer const-correctness, `Ref<>` vs raw-ref params,
   `Create` return-type rule, dead code) — from the 2026-06-12 surface audit.
5. Compute is bindable but **not dispatchable** (`cmd.Dispatch` is missing) —
   close that gap so `RenderGraph::AddComputePass` actually does something.

## Explicitly deferred to a later (shader/material) phase

- Offline shader reflection → serializable `ShaderInterface`.
- Descriptor/pipeline layouts derived from the shader; name-based descriptor
  writes.
- Vertex layout derived from / validated against the shader.
- `PipelineShaderStageInfo::Stage` (stage comes from the shader interface).

> Status legend: `ready` = reviewed and approved for implementation; `done` =
> landed and verified. Plans were fleshed out and ordered firmly before
> implementation, planset-1 style.
