# 01 — Push-constant layout & buffer

**Goal:** define a push-constant layout *once* and reuse it for both pipeline
layout creation and the per-draw push, instead of restating stage/offset/size at
every `cmd.PushConstants(...)`. Explicit layout — no shader reflection (that's a
later, shader/material phase).

**Dependencies:** none (planset-1 only). Independent of the other planset-2 plans.

## Current state

```cpp
// pipeline layout creation:
.PushConstantRanges = {{ .Stages = ShaderStage::Vertex, .Offset = 0, .Size = sizeof(mat4) }}
// every draw — stage/offset/size restated, data is an untyped void*+size:
cmd.PushConstants({ .PipelineLayout = *layout, .StageFlags = ShaderStage::Vertex,
                    .Offset = 0, .Size = sizeof(mat4), .Data = &transform });
```

## Design

Three pieces: a **layout** (shape, defined once), a **buffer** (this draw's
data), and a command that takes the buffer.

```cpp
// Veng/Renderer/PushConstants.h

// The shape — one or more ranges (stage + offset + size). Defined once and
// shared with the pipeline layout so the two cannot disagree.
struct PushConstantRange { ShaderStage Stages; u32 Offset; u32 Size; };

class PushConstantLayout {
public:
    static PushConstantLayout Create(std::initializer_list<PushConstantRange>);
    // Common single-block helper, typed:
    template <typename T>
    static PushConstantLayout Block(ShaderStage stages, u32 offset = 0); // Size = sizeof(T)
    [[nodiscard]] u32 TotalSize() const;
    [[nodiscard]] const vector<PushConstantRange>& Ranges() const;
};

// The data for one push. Sized from the layout; holds CPU bytes + the layout.
class PushConstantBuffer {
public:
    explicit PushConstantBuffer(PushConstantLayout layout);
    template <typename T> void Set(const T& value, u32 offset = 0);   // bounds-checked
    void SetData(std::span<const u8> bytes, u32 offset = 0);
    [[nodiscard]] const PushConstantLayout& Layout() const;
};
```

- **Pipeline layout** takes a `PushConstantLayout` (replacing the inline
  `PushConstantRangeInfo` vector); the VkPipelineLayout's ranges come from it.
- **`CommandBuffer::PushConstants(const PushConstantBuffer&)`** records the push:
  the pipeline-layout handle comes from the **last bound pipeline** (the command
  buffer already tracks `m_LastBoundPipelineLayout` for descriptor sets), and the
  stages/offsets/bytes come from the buffer. No `PipelineLayout`, stage, offset
  or size at the call site. In debug it can assert the buffer's layout is
  compatible with the bound pipeline's.
- The raw `PushConstants(PushConstantsInfo)` overload stays for dynamic/exotic
  cases.

So the source of truth is the `PushConstantLayout`, declared next to the data
struct and handed to both the pipeline layout and the buffer.

## Migration

Sample's triangle: build a `PushConstantLayout::Block<mat4>(ShaderStage::Vertex)`
once, feed it to the triangle pipeline layout, and each frame fill a
`PushConstantBuffer` with the transform and `cmd.PushConstants(buffer)`.

## Acceptance

- The push-constant range is declared once and shared by the pipeline layout and
  the per-draw push — a size/stage change is a one-line edit.
- `cmd.PushConstants(buffer)` names no pipeline layout, stage, offset or byte
  size; `buffer.Set(wrongType)` past the layout size is a fatal assert.
- Sample converted; output unchanged.
