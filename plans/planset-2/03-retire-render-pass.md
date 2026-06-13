# 03 — Retire the legacy render-pass pipeline surface

**Goal:** remove `GraphicsPipeline`, `RenderPass` and `Framebuffer` from the
public API. After planset-1/08 the render graph is dynamic-rendering only, and
after 09 the *only* remaining user of the render-pass path is the internal ImGui
layer. They're public dead weight.

**Dependencies:** planset-1/08 (render graph) and 09 (ImGui module) — both done.

## Current state

- `GraphicsPipeline` (render-pass based) + `RenderPass` + `Framebuffer` are
  public headers with full Native/escape surface.
- The render graph and the sample use only `DynamicGraphicsPipeline` + dynamic
  rendering.
- `ImGuiLayer` (planset-1/09) still uses `RenderPass`/`Framebuffer` for its draw
  pass, and `CommandBuffer::BeginRenderPass/EndRenderPass`.
- `ImageLayout` survives in the public `RenderPass` attachment descriptor — the
  one public spot still naming layouts after plan 08.

## Design

- Move `GraphicsPipeline`, `RenderPass`, `Framebuffer` and the
  `BeginRenderPass/EndRenderPass` command-buffer methods **into the backend**
  (or into the ImGui module, since it's the sole consumer). Public pipeline
  surface becomes dynamic-rendering only.
- Decide the ImGui home: either (a) ImGui keeps a backend-internal render-pass
  path, or (b) port the ImGui draw to dynamic rendering too and delete the
  render-pass path entirely. Recommendation: (b) if `imgui_impl_vulkan` supports
  dynamic rendering in our version (it does via `UseDynamicRendering`), which
  lets us delete `RenderPass`/`Framebuffer` outright and removes the last public
  `ImageLayout` use — finishing plan 08's acceptance to the letter.

## Acceptance

- No `GraphicsPipeline`/`RenderPass`/`Framebuffer` in `include/Veng/Renderer/`;
  the include-hygiene test still passes and no public header names `ImageLayout`
  except `Types.h`'s enum definition.
- ImGui renders unchanged (windowed sample), validation-clean.
- If (b): the render-pass path is gone from the codebase, not just hidden.
