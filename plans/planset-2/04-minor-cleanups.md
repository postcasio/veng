# 04 — Minor API cleanups & consistency

**Goal:** small, independent rendering-API tidy-ups that don't warrant their own
plan and don't touch shaders. From the 2026-06-12 surface audit. Each is opt-in;
we land the ones worth landing, and none change behaviour.

**Dependencies:** none. Shader-interface-dependent cleanups (deriving vertex
layouts, dropping `PipelineShaderStageInfo::Stage`) are excluded — shader/material
phase.

## Consistency

### CommandBuffer const-correctness

Recording methods are randomly `const`/non-`const` (`Draw`/`PushConstants`/
`Set*`/`Copy*`/`Blit` are `const`; `BindPipeline`/`BindVertexBuffer`/
`BindDescriptorSets`/`BeginRendering`/`BeginRenderPass` are not). Recording always
mutates GPU state — make them uniformly non-`const`.

### Parameter style: `Ref<X>` vs raw `X&`

Unify how resources are passed. `CopyBufferToImage(const Buffer&, const Image&)`,
`BlitImageInfo{ Image& … }` use raw refs; `BindVertexBuffer(const Ref<Buffer>&)`
uses `Ref`. Pick one convention (lean `const Ref<X>&` for owned GPU resources).

### `Create()` return-type rule

`Create` returns `Ref` for most types but `Unique` for `Fence`/`Semaphore`/
`Shader`. Document the rule (shared GPU resource → `Ref`; single-owner →
`Unique`) and apply it. Revisit `Shader` (likely `Ref` — shaders get shared/
cached) when the shader phase lands; leave for now if contentious.

### References in info structs

`PipelineShaderStageInfo{ const Shader& Module }` stores a reference in a
copyable struct — lifetime-fragile and inconsistent with `Ref<>` elsewhere.
Change to `Ref<Shader>`. (The `PushConstantsInfo{ PipelineLayout& }` case is
removed by plan 01.)

### `GetNative() const` returns mutable `Native&`

A logical-const violation on every resource (the escape hatch). Keep the
behaviour but document the doctrine in `Native.h` so it reads as deliberate, not
accidental.

## Simplifications

### Dead code: `VertexBufferLayout::Create`

`VertexBufferLayout::Create(initializer_list)` (returns `Unique`) is unused — the
type is used by value. Delete it.

### `VertexBufferLayout` vocabulary

Pre-planset-1/06 style: `const char*`/`std::string`, a tiny
`VertexElementDataType { Float, Float2, Float3 }`, no engine `Format`. Bring to
engine vocabulary (`Veng::string`, element type as `Format`). Deriving the layout
from a vertex struct / validating against a shader stays a later (shader) phase.

### Frame API redundancy

`Application::Frame` calls `Command::BeginFrame()` (returns `CommandBuffer&`) but
ignores it; the app re-fetches via `Context::GetCurrentCommandBuffer()`. Fold the
frame driver onto `Context` (`Context::BeginFrame()/EndFrame()`), and have one
clear way to get the frame's command buffer. (Replaces the static `Command`
class.)

### `DynamicGraphicsPipeline` → `GraphicsPipeline`

Once plan 03 retires the legacy render-pass `GraphicsPipeline`, the "Dynamic"
qualifier is pure disambiguation noise — rename the survivor to
`GraphicsPipeline`. (Sequenced after plan 03.)

### Extent/offset signedness

`Window` stores `ivec2` but returns `uvec2`; `SetScissor(ivec2, uvec2)` mixes
both. Pick a convention for extents (`uvec2`) and offsets (`ivec2`) and apply.

## Acceptance

- Each landed cleanup keeps the sample compiling and the headless smoke output
  unchanged — surface/ergonomics only, no behavioural change.

## Out of scope (deferred to planset-3 future-work draft)

The audit's bigger items are captured in
[../planset-3/README.md](../planset-3/README.md), not here: de-globalizing the
`Context::Instance()` singleton, and the thin/partially-stubbed event & input
systems. Both are their own future plansets.
