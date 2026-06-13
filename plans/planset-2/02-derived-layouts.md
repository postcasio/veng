# 02 — Derived descriptor & pipeline layouts + name-based writes

**Goal:** pipelines build their `PipelineLayout` (descriptor set layouts +
push-constant ranges) from their shaders' `ShaderInterface`, so consumers stop
hand-authoring `DescriptorSetLayout`/`PipelineLayout` to match shaders by
convention. Descriptor writes become name-based.

**Dependencies:** 01 (shader interface). Builds on planset-1/05 (typed writers)
and 07 (the layout types).

## Current state

`DescriptorSetLayout`, `PipelineLayout` are hand-authored and must agree with the
shaders. `DescriptorSet::Write(0, …)` / `Write(1, …)` use raw binding numbers
(see the sample's composite set). A mismatch is a runtime validation error, not a
compile/load error.

## Design

- **Merged interface:** a free function unions the `ShaderResource`s and
  push-constant blocks across a pipeline's stages — same set/binding with
  differing type is a fatal assert naming both; stage flags OR together.
- **`PipelineLayout::FromShaders(span<const Shader*>)`** builds the descriptor
  set layouts and push-constant ranges from the merged interface. Hand-authored
  `PipelineLayout::Create(info)` stays for bindless / update-after-bind / arrays
  where explicit control matters.
- **Pipeline auto-layout:** when `DynamicPipelineInfo::PipelineLayout` is null,
  derive it from `ShaderStages`. Explicit value still wins. (Also lets the
  pipeline drop the separately-created layout objects in the common case.)
- **Name-based writes:** `DescriptorSet::Write("u_Albedo", view, sampler)` etc.,
  resolving name→(set,binding) from the layout (populated only when derived from
  an interface; name-write on a nameless layout is a fatal assert listing
  available names). Forwards to the planset-1/05 typed writers.

## Acceptance

- A pipeline created from shaders + render-target info alone (no hand-written
  `DescriptorSetLayout`/`PipelineLayout`) renders correctly (composite pass).
- Cross-stage binding type conflict fails layout derivation, message naming
  set/binding/both types.
- `Write("typo", …)` is a fatal assert listing available names.
- The sample's composite set layout + pipeline layout are deleted in favour of
  derivation; output unchanged.
