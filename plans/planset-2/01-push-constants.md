# 01 — Push-constant range & typed push

**Goal:** declare a push-constant range *once* (on the pipeline layout) and push
typed data with `cmd.PushConstants(value)` — no stage/offset/size restated at the
call site, no untyped `void*`. Explicit declaration, no shader reflection.

**Dependencies:** none (planset-1 only). Independent of the other planset-2 plans.

## Framing: push constants are an engine-fixed channel

Push constants in veng are, and will remain, an **engine-controlled** channel:
they carry render transforms (matrices) supplied by the renderer. They are *not*
part of the material's authoring surface — no material discovers, names, or
overrides them. The eventual scene renderer will own a single fixed push-constant
struct.

That has two consequences for this plan:

- **No reflection for push constants — ever.** Reflection only earns its keep
  when an external author (a material) must discover and write named fields by
  introspecting the shader. Push constants have no external author: the renderer
  knows the struct at compile time because it owns it. Reflection belongs to the
  *shader-uniform* side (descriptor-bound UBOs/SSBOs, samplers), drafted in
  [future/](../future/README.md) — not here.
- **No CPU-side staging object.** The earlier draft proposed a
  `PushConstantBuffer` for assembling a block from multiple writes and a
  `PushConstantLayout` class to carry shape. Both only existed to serve features
  that turn out to be material/reflection concerns (incremental multi-source
  writes, name-based `Set`). With push constants fixed and engine-owned, there is
  nothing left for them to do. A plain C++ struct *is* the layout (offsets are
  member offsets) and *is* the data.

## Current state

```cpp
// pipeline layout creation:
.PushConstantRanges = {{ .Stages = ShaderStage::Vertex, .Offset = 0, .Size = sizeof(mat4) }}
// every draw — stage/offset/size restated, data is an untyped void*+size:
cmd.PushConstants({ .PipelineLayout = *layout, .StageFlags = ShaderStage::Vertex,
                    .Offset = 0, .Size = sizeof(mat4), .Data = &transform });
```

The range is the source of truth, but the draw restates all of it and discards
the type. The command buffer already tracks the bound pipeline layout
(`m_LastBoundPipelineLayout`, used for descriptor sets) — so everything the push
restates is already recoverable from the bound pipeline.

## Design

Two pieces: a **range** on the pipeline layout (shape, declared once) and a
**typed push** that reads everything else from the bound pipeline.

```cpp
// Veng/Renderer/PipelineLayout.h

// The shape of one range. Stays general (per-range stages/offset) at no cost.
struct PushConstantRange
{
    ShaderStage Stages{};
    u32 Offset = 0;
    u32 Size{};

    // Typed helper so the size is never hand-written:
    template <typename T>
    static PushConstantRange Of(ShaderStage stages, u32 offset = 0)
    {
        return {.Stages = stages, .Offset = offset, .Size = sizeof(T)};
    }
};

// PipelineLayoutInfo carries a vector<PushConstantRange> (replacing the inline
// PipelineLayoutPushConstantRangeInfo).
```

```cpp
// Veng/Renderer/CommandBuffer.h

// Typed push. Layout handle, stages, and size come from the bound pipeline;
// only the value (and an optional offset) come from the call site.
template <typename T>
void PushConstants(const T& value, u32 offset = 0) const;
```

**`PushConstants<T>` behaviour:**

1. Take the bound pipeline layout (`m_LastBoundPipelineLayout`) and its ranges.
2. Find the declared range that fully contains `[offset, offset + sizeof(T))`.
   In debug, assert exactly one such range exists.
3. Record `pushConstants(layout, range.Stages, offset, sizeof(T), &value)`.

The call site names no layout, stage, or size — and no byte size to keep in sync
with the type. `static_assert(sizeof(T) <= 128)` guards the guaranteed minimum
block size.

**Why `offset` is a parameter, not a buffer.** A single offset gives the three
things a staging buffer would have:

- *Whole-block push* (the common case): `offset = 0`, `sizeof(T) == range.Size`.
- *Partial update* — push one field without re-sending the block:
  `cmd.PushConstants(model, offsetof(DrawPushConstants, Model))`. `offsetof` is
  the natural idiom because the struct is engine-owned and compile-time known.
- *Multi-range, unambiguously* — if vertex sees bytes 0–64 and fragment sees
  64–80, each push lands inside exactly one range and picks up that range's
  stages automatically. The offset *is* the disambiguator, so there is no "which
  range?" question.

The one case offsets don't cover is a *single* push straddling two ranges with
different stages — genuinely exotic. The raw `PushConstants(PushConstantsInfo)`
overload stays for it (and any other dynamic/exotic use).

### Struct-layout responsibility

The C++ struct's member offsets must mirror the shader's `push_constant` block
(std430-style rules — `vec3` padded to 16, etc.). This is already true of the
current `&transform`/`sizeof` code, so it is **not a regression** — but the typed
struct makes the struct the documented contract, which is strictly better than an
untyped `void*`. The plan adds the `static_assert` on total size; member
alignment remains the author's responsibility (a note in the header, not a
mechanism).

## Migration

- Replace `PipelineLayoutPushConstantRangeInfo` with `PushConstantRange`; update
  `PipelineLayoutInfo`, `PipelineLayout` storage, and the getter.
- Add the templated `CommandBuffer::PushConstants<T>`; keep the raw
  `PushConstants(PushConstantsInfo)` overload.
- Sample's triangle: declare the range as
  `PushConstantRange::Of<mat4>(ShaderStage::Vertex)` on the pipeline layout, and
  each frame `cmd.PushConstants(transform)` — no layout, stage, offset, or size
  at the call site.

## Acceptance

- The push-constant range is declared once (on the pipeline layout) and the
  per-draw push restates none of it: `cmd.PushConstants(value)` names no pipeline
  layout, stage, offset, or byte size.
- A size/stage change is a one-line edit on the range declaration.
- The data is typed: `cmd.PushConstants(value)` with `sizeof(value)` not fitting
  any single declared range is a fatal assert; `sizeof > 128` fails to compile.
- `cmd.PushConstants(value, offset)` supports partial and multi-range pushes,
  selecting stages from the range that contains the bytes.
- No `PushConstantBuffer` / `PushConstantLayout` types introduced; reflection is
  not in scope for push constants.
- Sample converted; output unchanged.
