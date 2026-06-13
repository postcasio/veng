# 05 — Descriptor set typed writers

**Goal:** replace `DescriptorSetWriteInfo` + variant with typed `Write` methods
that infer the descriptor type from the layout. Unsupported/mismatched writes
become fatal asserts instead of silent no-ops. (Bind groups deliberately out of
scope — future direction.)

**Dependencies:** 04 (the keep-alive question changes shape once the retire
queue exists). Plan 12 (reflection) layers name-based writes on top of this.

## Current state

`include/Veng/Renderer/Backend/DescriptorSet.h`:

- `DescriptorSetWriteInfo { vk::DescriptorType Type; u32 Binding; u32
  ArrayElement; variant<vector<DescriptorImageInfo>,
  vector<DescriptorBufferInfo>> Data; }` — caller restates the type the layout
  already knows; only two descriptor types supported; `default: break` drops
  everything else silently (interim assert added by plan 01).
- `m_BoundResources` sized by binding count but indexed by binding number —
  out-of-bounds for sparse layouts (interim assert added by plan 01).
- `DescriptorSet` already holds `Ref<DescriptorSetLayout> m_Layout`
  (`DescriptorSet.h:77`) — everything needed for inference is present.

## Design

### Layout lookup

Add to `DescriptorSetLayout` (`DescriptorSetLayout.h`):
`[[nodiscard]] vk::DescriptorType GetBindingType(u32 binding) const` and
`[[nodiscard]] u32 GetBindingCount(u32 binding) const` (descriptor count, for
arrays), backed by a `map<u32, BindingDesc>` keyed by binding *number* —
sparse-safe by construction. Asserts (fatal, per plan 03) on unknown binding.

### Typed writer API

```cpp
class DescriptorSet {
public:
    // sampled image / combined image sampler
    void Write(u32 binding, const Ref<ImageView>& view, const Ref<Sampler>& sampler);
    // storage image
    void Write(u32 binding, const Ref<ImageView>& view);
    // uniform or storage buffer — type disambiguated by the layout
    void Write(u32 binding, const Ref<Buffer>& buffer);
    void Write(u32 binding, const Ref<Buffer>& buffer, u64 offset, u64 range);
    // bindless-style arrays
    void WriteArray(u32 binding, std::span<const Ref<ImageView>> views,
                    const Ref<Sampler>& sampler, u32 firstElement = 0);
};
```

- Each `Write` looks up the binding's `vk::DescriptorType` from the layout,
  asserts the payload kind matches (image payload for image types, buffer
  payload for buffer types), builds the `vk::WriteDescriptorSet`, and calls
  `updateDescriptorSets` immediately. Image layout for the write is derived
  from the descriptor type (`eShaderReadOnlyOptimal` for sampled,
  `eGeneral` for storage) — removing the `vk::ImageLayout` knob from
  `DescriptorImageInfo` is part of the insulation goal; revisit if a real
  exception appears.
- Supported types now: `eCombinedImageSampler`, `eSampledImage`,
  `eStorageImage`, `eUniformBuffer`, `eStorageBuffer`. The assert on anything
  else names the type — adding one is a small, obvious extension.
- If profiling ever shows per-write `updateDescriptorSets` mattering, add a
  `DescriptorWriter` batcher then — not now.

### Keep-alive / ownership

- Replace `m_BoundResources` (`vector<Ref<void>>` indexed by `binding++`) with
  `map<u32, vector<Ref<void>>> m_BoundPerBinding` keyed by binding number —
  re-writing a binding releases exactly the resources it replaces, arrays hold
  all elements. This is the *ownership* list (dangling-descriptor prevention),
  distinct from the deleted frame-tracking lists of plan 04.

### Deletions

- `DescriptorSetWriteInfo`, `DescriptorSetUpdateInfo`, `DescriptorImageInfo`,
  `DescriptorBufferInfo`, `DescriptorSetImageBinding` (the latter already looks
  vestigial — confirm no users), and `UpdateDescriptorSet` itself (the
  `Update` rename from Part 1 is subsumed: the method disappears).
- The interim asserts from plan 01 go with it.

## Migration

Call sites are in `examples/hello-triangle` and possibly `Context.cpp` /
ImGui-adjacent code — grep `UpdateDescriptorSet` and convert as part of this
plan. Each variant write becomes one `Write` call; the diff at call sites
should be strictly negative in lines.

## Acceptance

- `DescriptorSetWriteInfo`/`UpdateDescriptorSet` no longer exist; everything
  goes through typed `Write`/`WriteArray`.
- Writing a storage buffer works (was silently dropped before).
- Writing the wrong payload kind, or to a nonexistent binding, fails a fatal
  assert naming binding and type.
- A sparse layout (bindings 0, 2, 5) creates, writes, and binds correctly.
