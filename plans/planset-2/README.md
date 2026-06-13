# planset-2 — rendering API surface cleanup

**Phase goal:** trim the rendering API surface that planset-1 left behind —
remove "say it twice" duplication at call sites and retire vestigial public
types — *without* touching shaders. Shader-facing work (reflection, derived
descriptor layouts, name-based writes, vertex layout from the shader) is
deliberately deferred: it belongs to the material system, drafted in
[planset-3](../planset-3/README.md).

## Scope decision (2026-06-12)

Shader changes are pushed to a later phase again. The material system — not the
shader — becomes the primary authoring interface (see planset-3), so the
shader-interface/reflection work is designed there rather than here. planset-2 is
limited to cleanups that stand on their own.

## Plans

| # | Plan | Status |
|---|------|--------|
| 01 | [Push-constant layout & buffer](01-push-constants.md) | draft |
| 02 | [Pipeline attachment formats from render targets](02-attachment-formats.md) | draft |
| 03 | [Retire the legacy render-pass pipeline surface](03-retire-render-pass.md) | draft |
| 04 | [Minor API cleanups](04-minor-cleanups.md) | draft |

All independent — they can land in any order.

## Duplication / surface this phase removes

1. Push-constant range declared at pipeline-layout creation *and* restated in
   every `cmd.PushConstants(...)` → one `PushConstantLayout`, a
   `PushConstantBuffer` for the data, `cmd.PushConstants(buffer)`.
2. Pipeline `ColorAttachments` formats restated to match the render target →
   derive/validate against the render-graph pass.
3. `GraphicsPipeline`/`RenderPass`/`Framebuffer` public but used only by the
   internal ImGui path → port ImGui to dynamic rendering and delete them (also
   removes the last public `ImageLayout`).
4. Assorted small ergonomics (frame API, `VertexBufferLayout` vocabulary).

## Explicitly deferred to a later (shader/material) phase

- Offline shader reflection → serializable `ShaderInterface`.
- Descriptor/pipeline layouts derived from the shader; name-based descriptor
  writes.
- Vertex layout derived from / validated against the shader.
- `PipelineShaderStageInfo::Stage` (stage comes from the shader interface).

> Status legend: `draft` = proposed, not yet detailed/approved. Plans get fleshed
> out and ordered firmly before implementation, planset-1 style.
