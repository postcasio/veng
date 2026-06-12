# 12 — Shader reflection

**Goal:** run spirv-reflect at shader load so descriptor-set layouts,
push-constant ranges, and vertex inputs are *derived* from SPIR-V instead of
hand-duplicated, and consumers can write resources by name. Removes the
layout-mismatch bug class.

**Dependencies:** 05 (typed writers are the write path that name-based writes
forward to). Benefits from 03 (`Result` for reflection failures). Last in the
sequence by design — the write/layout APIs it builds on must be settled.

## Current state

- `Shader` (`include/Veng/Renderer/Backend/Shader.h`) loads SPIR-V and keeps
  only module + entry point — the binary's metadata is discarded.
- `DescriptorSetLayout`, `PipelineLayout`, `VertexBufferLayout` are all
  hand-authored by consumers and must agree with the shaders by convention.
- After plan 06 these use engine types (`ShaderStage`, `Format`), which is the
  vocabulary reflection output needs anyway.

## Design

### Dependency

FetchContent `KhronosGroup/SPIRV-Reflect` (two files, pinned to a release tag
per plan 01's policy), linked PRIVATE — reflection types never appear in
public headers.

### Reflection at load

`Shader::Create` (both file and binary paths) runs reflection once and stores:

```cpp
struct ShaderReflection {
    struct Binding { u32 Set; u32 Binding; string Name;
                     DescriptorType Type; u32 Count; ShaderStage Stages; };
    vector<Binding> Bindings;
    struct PushConstants { u32 Offset; u32 Size; ShaderStage Stages; };
    optional<PushConstants> PushConstantRange;
    vector<VertexInput> VertexInputs;   // location, name, Format
};
const ShaderReflection& Shader::GetReflection() const;
```

(`DescriptorType` engine enum — introduced here or in plan 06, whichever lands
first needs it.) Reflection failure is a `Result` error from `Shader::Create`
(plan 03 shape).

### Derived layouts

- `DescriptorSetLayout::Create(const ShaderReflection&, u32 set)` /
  `PipelineLayout` creation from a *merged* reflection of all stages
  (`PipelineLayout::Create(span<const Shader*>)`): union bindings across
  stages, assert on conflicts (same set/binding, different type), merge stage
  flags. Hand-authored creation stays available — bindless tables and
  update-after-bind sets will want explicit control.
- `VertexBufferLayout` from reflected vertex inputs: locations + formats come
  from the shader; *offsets/stride* come from the C++ vertex type, so the
  consumer still provides the struct — `VertexBufferLayout::Create<V>(const
  ShaderReflection&)` matches reflected locations to fields by declaration
  order (or explicit per-field mapping if order proves fragile — decide during
  implementation against the sample app's vertex types).
- Pipeline creation (`GraphicsPipelineInfo`): when `PipelineLayout` /
  `VertexBufferLayout` are omitted, derive them from `ShaderStages`'
  reflection. Explicit values still win.

### Name-based writes

```cpp
// DescriptorSet addition (set knows its layout; layout now knows names)
set->Write("u_Albedo", imageView, sampler);   // looks up binding by name,
set->Write("u_Camera", uniformBuffer);        // forwards to plan-05 writers
```

Name→binding map lives on `DescriptorSetLayout` (populated only when created
via reflection; name-based `Write` on a hand-authored layout without names is
a fatal assert). A higher-level `Material` abstraction stays out of veng for
now — name-based `DescriptorSet::Write` is the primitive consumers can build
materials on.

## Migration

The sample app deletes its hand-written layout definitions pipeline by
pipeline, keeping the explicit path where it intentionally diverges from the
shader. Each deleted layout is a place a mismatch bug can no longer exist.

## Acceptance

- A pipeline created with only shaders + render-target info (no hand-written
  `DescriptorSetLayout`/`PipelineLayout`/`VertexBufferLayout`) renders
  correctly.
- A shader whose bindings disagree across stages (type conflict) fails layout
  derivation with a message naming set/binding/both types.
- `set->Write("name", ...)` with a typo'd name fails a fatal assert listing
  available names.
- Reflection adds no public header dependency (spirv-reflect stays PRIVATE).
