# 03 — Push-constant blocks

**Goal:** define a push-constant layout once and reuse it everywhere — pipeline
layout creation *and* the `cmd.PushConstants(...)` call — instead of restating
stage/offset/size at every push. This is the motivating example for the phase.

**Dependencies:** 01 (interface carries the push-constant block), 02 (derived
layouts).

## Current state

```cpp
// At layout creation:
.PushConstantRanges = {{ .Stages = ShaderStage::Vertex, .Offset = 0, .Size = sizeof(mat4) }}
// At every push — restated:
cmd.PushConstants({ .PipelineLayout = *layout, .StageFlags = ShaderStage::Vertex,
                    .Offset = 0, .Size = sizeof(mat4), .Data = &transform });
```

Stage, offset and size are duplicated; `Data`/`Size` are an untyped void*/byte
count the caller computes.

## Design

A `PushConstantBlock<T>` handle that owns the (stage, offset, size) once and is
type-safe over the payload `T`:

```cpp
template <typename T>
struct PushConstantBlock {            // static_assert trivially copyable
    ShaderStage Stages; u32 Offset = 0;
    static constexpr u32 Size = sizeof(T);
};

// Source of truth options (decide in impl):
//  a) from the shader interface:  layout->GetPushConstantBlock<T>("PushConstants")
//  b) declared once next to the shared C++/GLSL struct.
cmd.PushConstants(block, value);      // T& value; no offset/size/stage restated
```

- The pipeline layout's push-constant range comes from the same block (or, via
  plan 02, straight from the interface), so the two can't disagree.
- `cmd.PushConstants(const PushConstantBlock<T>&, const T&)` replaces the
  `PushConstantsInfo` struct at call sites (the raw overload stays for dynamic
  cases). It can assert `Offset+Size` and `Stages` against the bound pipeline's
  interface in debug.
- Document the shared-struct discipline (the C++ `T` must match the GLSL
  `layout(push_constant)` block); plan 01's reflected member list lets a debug
  build validate field offsets.

## Acceptance

- Pushing constants names neither stage, offset nor byte size at the call site,
  and passing the wrong payload type is a compile error.
- The pipeline layout's push-constant range and the push call derive from one
  definition — a deliberate size/stage change is made in exactly one place.
- Sample's triangle transform push is converted; output unchanged.
