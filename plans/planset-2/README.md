# planset-2 — shader-driven pipeline API & surface cleanup

**Phase goal:** make the shader the single source of truth for pipeline
interface, so consumers stop hand-duplicating descriptor/push-constant/vertex
layouts — and trim the rendering API surface that planset-1 left behind. This
sets up the future asset system (editor + material tools) by producing the
shader-interface metadata at *import* time, not at runtime.

## Guiding decisions

- **Reflection is offline, not at load.** SPIR-V reflection runs at build/import
  time and emits a serializable `ShaderInterface` stored beside the SPIR-V. The
  runtime loads the description — no reflection library in the runtime or public
  surface, no per-launch reparse, deterministic, and editor-readable without a
  live device. This supersedes planset-1's plan 12 (runtime reflection).
- **Asset-ready from day one.** The `ShaderInterface` type is the same thing the
  future asset importer will produce and the material editor will consume. The
  build-time sidecar is a bridge; when the importer lands, runtime code is
  unchanged.
- **Derive, don't restate.** Pipelines build their `PipelineLayout` (descriptor
  set layouts + push-constant ranges) and validate vertex inputs from the
  `ShaderInterface`. Hand-authoring stays available for advanced cases (bindless
  tables, update-after-bind).
- **Shrink the public surface.** Anything the render graph + dynamic rendering
  made vestigial (legacy render-pass pipeline path) leaves the public API.

## Duplication this phase removes (today's call sites)

1. Push-constant range declared in `PipelineLayoutInfo` *and* restated in every
   `cmd.PushConstants(...)`.
2. `struct Vertex` *and* a hand-written `VertexBufferLayout` restating its fields.
3. `DescriptorSetLayout` bindings hand-authored to match the shader; writes by
   raw binding number.
4. Pipeline `ColorAttachments` formats restated to match the render target.
5. `PipelineShaderStageInfo::Stage` restated though the shader knows its stage.
6. `GraphicsPipeline`/`RenderPass`/`Framebuffer` public but used only by the
   internal ImGui path.

## Plans

| # | Plan | Status |
|---|------|--------|
| 01 | [Shader interface description + offline reflection](01-shader-interface.md) | draft |
| 02 | [Derived descriptor & pipeline layouts + name-based writes](02-derived-layouts.md) | draft |
| 03 | [Push-constant blocks](03-push-constant-blocks.md) | draft |
| 04 | [Vertex layout from the vertex type](04-vertex-layout.md) | draft |
| 05 | [Pipeline attachment formats from render targets](05-attachment-formats.md) | draft |
| 06 | [Retire the legacy render-pass pipeline surface](06-retire-render-pass.md) | draft |
| 07 | [Asset/material format groundwork (design only)](07-asset-groundwork.md) | draft |

## Dependency sketch

```
01 (shader interface)
 ├─► 02 (derived layouts) ──► 03 (push-constant blocks)
 ├─► 04 (vertex layout)
 └─► 07 (asset groundwork, design)
05 (attachment formats)  ── depends on planset-1/08 (render graph, done)
06 (retire render-pass)  ── depends on planset-1/08 + 09 (done)
```

01 is the linchpin; 05 and 06 are independent cleanups that can land in any
order. 07 is a design note that anchors the asset-system phase, not code.

> Status legend: `draft` = proposed here, not yet detailed/approved. Plans get
> fleshed out (and ordered firmly) before implementation, planset-1 style.
