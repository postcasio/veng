# Plan 05 — Bindless descriptor subsystem (`BindlessRegistry`, set 0)

**Goal:** the engine-side bindless foundation that materials are built on. A
`BindlessRegistry` (owned by `Context`) holds a few large global arrays — sampled
images, samplers, storage images — bound **once per frame** as **set 0**, with a
free-list slot allocator handing back typed `u32` handles. Every `PipelineLayout`
reserves set 0 for the registry (a fixed engine root signature). This is the
promotion of the descriptor-indexing features veng already enables into a real
subsystem, done **before** materials so a material can be the thin "handles + a
parameter entry" it's meant to be rather than a bundle of descriptor sets.

Full design: [bindless-descriptors.md](../future/bindless-descriptors.md). This plan
implements its core; the per-material/per-object SSBO data layout lands with the
material (plan 09).

## Why this is its own plan, and before materials

The material *is defined in terms of* bindless handles — a texture becomes a `u32`,
binding is "write the material's index", and `Bind` stops swapping descriptor sets
per draw. Building the registry first means plan 09's material is the end-state
model, not a per-set stopgap that gets rewritten when bindless lands. It's also a
self-contained engine subsystem, verifiable on its own by routing the sample's
existing texture sampling through set 0.

## Prerequisite: the descriptor-policy addendum

[planset-2/06](../planset-2/06-descriptor-update-policy.md) (descriptor binding
flags & update policy) is the foundation this builds on: static-by-default flags
with a per-binding **bindless opt-in**, and a single source of truth mapping
descriptor **type → {pool size, required device feature}**. That addendum also
closes the documented validation gap (storage-image/sampled-image pool sizes +
the `UPDATE_AFTER_BIND` feature mismatch — CLAUDE.md). **It is currently
`proposed`; land it first** (either as the tail of planset-2 or as the opening step
of this plan). Bindless's `partiallyBound` + `UPDATE_AFTER_BIND` arrays are exactly
where those opt-in flags legitimately live.

## What's already in place (this is a promotion, not a green field)

Per the design doc: device creation (`Context.cpp`) already enables
`runtimeDescriptorArray`, the `shader*ArrayNonUniformIndexing` family,
`descriptorBindingPartiallyBound`, the `…UpdateAfterBind` features, and
`bufferDeviceAddress`. `DescriptorSet::WriteArray` already writes many views into
one arrayed binding. ImGui's texture registration is a working mini-bindless
pattern. The features are on and mostly unused.

## API

```cpp
namespace Veng
{
    struct TextureHandle { u32 Index = k_Invalid; [[nodiscard]] bool IsValid() const; };
    struct SamplerHandle { u32 Index = k_Invalid; [[nodiscard]] bool IsValid() const; };
    struct StorageImageHandle { u32 Index = k_Invalid; };

    class BindlessRegistry   // owned by Context (de-globalized — planset-4)
    {
    public:
        TextureHandle      Register(const Ref<ImageView>& sampled);
        SamplerHandle      Register(const Ref<Sampler>& sampler);
        StorageImageHandle RegisterStorage(const Ref<ImageView>& storage);
        void Release(TextureHandle);          // deferred free (see lifetime)
        void Release(SamplerHandle);
        void Bind(CommandBuffer& cmd) const;  // bind set 0, once per frame

        [[nodiscard]] Ref<DescriptorSetLayout> GetSet0Layout() const;  // the root signature
    };
}
```

Internally: one `DescriptorSetLayout` with a few large, `partiallyBound`,
`UPDATE_AFTER_BIND`, arrayed bindings (sampled images, samplers, storage images);
a free-list per array; writes into the set as resources register; and a `Ref` kept
to each registered resource so a live slot can't dangle (the centralized version of
`DescriptorSet::m_BoundResources`). Array capacities sized against the target Metal
GPUs' `maxDescriptorSetUpdateAfterBind*` limits (MoltenVK reality — global arrays in
a set, **not** `VK_EXT_descriptor_buffer`).

## Set 0 as a fixed root signature

Every `PipelineLayout` reserves **set 0** for the registry across all pipelines;
per-frame / per-pass / per-material data goes in higher sets. This is what lets set
0 be bound once per frame and never touched again. The reflection-driven layout
builder (plan 08) recognizes set 0 as engine-provided and excludes it from
author-declared bindings — the two plans agree on this contract.

## Lifetime & sync (the hard part)

- **Slot reuse is deferred.** `Release` must not reclaim a slot an in-flight frame
  still samples — defer the reclaim past the frames-in-flight window through the
  **existing per-frame retire queue** (planset-1/04), not a second mechanism (same
  rule as resource destruction and asset eviction, plan 04).
- **`partiallyBound` + `UPDATE_AFTER_BIND`** are what make a sparse,
  mutated-while-bound global array legal — the descriptor-policy addendum's
  bindless opt-in is where these flags belong.
- **Barriers are unchanged.** Bindless only changes how a shader *names* a texture,
  not its image state — a bindless texture sampled in a pass still declares
  `.Sample(view)` on the render graph, which derives the barrier as today. Bindless
  and the render graph are orthogonal.

## Work

1. **Land planset-2/06** (if not already) — the descriptor-policy single source of
   truth + bindless opt-in flags; closes the validation gap.
2. **`BindlessRegistry`** — the set-0 layout, the arrayed bindings, the slot
   allocators, `Register`/`Release`/`Bind`, the kept-`Ref` table, deferred slot
   release via the retire queue. Owned by `Context`; created at context init.
3. **Set 0 reservation** — `PipelineLayout` always includes the registry's set-0
   layout as set 0; per-pass/material sets shift to ≥ 1.
4. **Shader side** — the bindless GLSL/Slang preamble (separate `texture2D[]` +
   `sampler[]`, `nonuniformEXT`); migrate the sample's existing texture-sampling
   shader + pass to bind set 0 once and index a registered texture, proving the
   path end to end before any asset type depends on it.
5. **Tests** — extend the GPU suite: register N image views + a sampler, bind set
   0, sample a registered texture in a draw, assert correct pixels; a
   register→release→re-register cycle proves deferred slot reuse doesn't alias; a
   `partiallyBound` sparse-slot sample is validation-clean.

## Dependencies

Plan 04 only loosely (shares the retire-queue deferral pattern); fundamentally an
engine subsystem independent of the cooker/manager. **Prerequisite:** planset-2/06.
**Blocks** the texture asset (plan 06, which registers into it) and the material
(plan 09, which is defined in terms of handles).

## Acceptance

- Clean build, `ctest` green incl. the bindless GPU tests.
- The sample renders its texture through set 0 (bound once per frame), not a
  per-draw descriptor set.
- **Validation-clean** under `VE_DEBUG` — and this plan should *narrow* the known
  descriptor gap (via the planset-2/06 fix), so update/remove the validation-gate
  allowlist entry (CLAUDE.md / `cmake/ValidationGate.cmake`) accordingly.

## Notes

- **Auto-register vs. explicit** (the doc's open call): lean **explicit-but-
  ergonomic** — a resource gets a slot when something registers it (the texture
  loader, plan 06), not automatically on every `ImageView::Create`.
- **Buffers via BDA vs. arrayed SSBO descriptors**: the per-material/per-object
  SSBO arrays land with the material (plan 09); decide BDA-vs-descriptor there. This
  plan covers the image/sampler arrays, which are unambiguously array-in-set.
- **One mega-set vs. typed sets**: v1 one set 0 with a few arrayed bindings
  (textures / samplers / storage), per the doc.
- Keep `DescriptorSet`/`DescriptorSetLayout` intact — ImGui and simple passes keep
  using the per-set API; bindless is additive, set 0 specifically.
