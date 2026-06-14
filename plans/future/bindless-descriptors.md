# Bindless descriptors — design sketch (future)

> **Vision / design sketch, not scheduled.** Detail for the descriptor-strategy
> cross-cutting concern in [README.md](README.md) (asset/material phase, area 1).
> Direction and decisions, not a firm plan — it becomes part of a planset when the
> asset/material work is taken up. Builds on the
> [planset-2/06 addendum](../planset-2/06-descriptor-update-policy.md), which fixes
> the *altitude* of today's per-set descriptor layer; this sketch is the real
> subsystem that layer hands off to.

## Why

veng is **bind-by-set** today: each `DescriptorSet` holds a few resources, you
`BindDescriptorSets` the right one before a draw, and a material would carry its
own set. That is `O(draws)` rebinds and one layout/set per material variation —
fine for hello-triangle, a wall once materials and a scene multiply.

Bindless flips it: a **few large global arrays** of every live resource, **bound
once per frame**, and draws carry **`u32` indices** into those arrays instead of
swapping sets. A texture stops being "a descriptor you bind" and becomes a number.

```
        register on Create/upload                 bound ONCE per frame
 Image/View ───────────────────► BindlessRegistry ──────────────► set 0 (global)
 Sampler                          (slot allocator)                  textures[]   ← ~16k
 Buffer                                │                            samplers[]
                                       │ hands back                 storageImages[]
                                       ▼
                                  TextureHandle{u32}
                                       │
   draw: cmd.PushConstants(DrawData{ objectIndex })   // tiny selector only
                                       │
                                       ▼
   shader: texture(textures[nonuniformEXT(material.Albedo)], uv)
```

## What veng already has (the foundation is laid)

This is a promotion of existing pieces, not a from-scratch subsystem:

- **The device is already configured for it.** `Context.cpp` (device creation)
  already enables `runtimeDescriptorArray`, the `shader*ArrayNonUniformIndexing`
  family, `descriptorBindingPartiallyBound`, the `…UpdateAfterBind` features, and
  `bufferDeviceAddress` — exactly the descriptor-indexing features bindless needs,
  on today and mostly unused.
- **A bindless-shaped write already exists:** `DescriptorSet::WriteArray` writes
  many image views into one arrayed binding.
- **Prior art in-repo:** ImGui's `ImGui_ImplVulkan_AddTexture` is a mini-bindless
  registry — every texture gets a persistent descriptor slot. Same pattern,
  engine-wide.
- **`UPDATE_AFTER_BIND` is available as a per-binding opt-in** (`Bindless = true`,
  [planset-2/06](../planset-2/06-descriptor-update-policy.md)) — precisely what a
  mutated-while-bound global set needs; the registry's arrayed bindings would set it.

## The model

**1. Typed handles** (keep veng's type-safety idiom — not raw `u32`):
```cpp
struct TextureHandle { u32 Index = k_Invalid; };
struct SamplerHandle { u32 Index = k_Invalid; };
struct BufferHandle  { u32 Index = k_Invalid; };   // or vk::DeviceAddress via BDA
```

**2. A `BindlessRegistry`** (owned by the device/context — see de-global, area 3):
one big set + a free-list slot allocator.
```cpp
TextureHandle Register(const Ref<ImageView>& view);  // writes slot, returns index
void          Release(TextureHandle);                 // deferred free (see sync)
void          Bind(CommandBuffer&);                   // bind set 0, once per frame
```
Internally owns a `DescriptorSetLayout` with a few large, `partiallyBound`,
arrayed bindings (sampled images, samplers, storage images), writes into them as
resources register, and keeps a `Ref` to each registered resource so a live slot
can't dangle — the ownership rule `DescriptorSet::m_BoundPerBinding` already
enforces, centralized.

**3. A shared "global" set layout in every `PipelineLayout`.** Reserve **set 0**
for the registry across all pipelines (a fixed engine root signature); per-frame /
per-pass data goes in higher sets. This is what lets set 0 be bound once and never
touched again.

**4. Per-draw indices via push constants** — veng's existing engine-owned channel
([plan 01](../planset-2/01-push-constants.md)). See the data-layout section: push
constants carry a *selector*, not the resources.

## Per-draw data layout (push constants vs buffers) — for a deferred renderer

The recurring question: where do transforms and texture indices live? Resolved
for veng's constraints and a deferred pipeline:

- **Push constants are capped at 128 bytes** in veng (plan 01's
  `static_assert(sizeof(T) <= 128)`, the portable guaranteed minimum). A `mat4`
  model (64B) + normal matrix already fills it — there is no room for matrices
  *and* indices. One of them must live in a buffer regardless.
- **Cardinality differs:** transforms are **per-object**; texture indices are
  **per-material** (shared across many objects). Per-material data in push
  constants would re-emit identical bytes every draw — wrong frequency.
- **Deferred specifics:** material textures are sampled only in the
  **G-buffer/geometry pass** (many small draws — where per-draw overhead matters
  most). The **lighting pass** reads the G-buffer (a fixed handful of
  attachments), so it needs little or no bindless.

**Recommended layout:**

| Data | Lives in | Indexed by | Frequency |
|---|---|---|---|
| camera / view-proj | UBO (per-frame set) | — | per frame |
| `ObjectData { mat4 Model; mat4 Normal; u32 Material; }` | **SSBO array** | `objectIndex` | per object |
| `MaterialData { u32 Albedo, Normal, Orm; vec4 Factors; }` | **SSBO array** | `obj.Material` | per material |
| textures / samplers | bindless global set (set 0) | handle (`u32`) | global |
| `DrawData { u32 objectIndex; }` | **push constant** (~4–8B) | — | per draw |

```glsl
layout(push_constant) uniform PC { uint objectIndex; };
ObjectData   o = objects[objectIndex];
MaterialData m = materials[o.Material];
vec3 albedo = texture(sampler2D(textures[nonuniformEXT(m.Albedo)], samplers[s]), uv).rgb;
```

So: **texture indices belong in a buffer (a material SSBO array), not push
constants** — push constants carry the index that *selects* the buffer entry.
Prefer one indexed SSBO array over a per-material UBO bound each draw: a per-draw
UBO reintroduces per-draw binding (the thing bindless removes) and UBOs have a
16KB guaranteed cap that's awkward for "all materials"; UBOs stay the right tool
for per-frame globals.

**Matrices:** model + normal in push constants is a fine *simple-start* shortcut
for non-instanced forward draws, but for deferred (many objects, any instancing)
move them into the per-object SSBO indexed by `objectIndex` / `gl_InstanceIndex`,
and store the normal matrix **precomputed** rather than `transpose(inverse(model))`
per vertex. Push constants then shrink to the selector.

## Lifetime & sync (the hard part)

- **Slot reuse must be deferred.** Freeing slot 42 and reusing it immediately can
  alias a resource an in-flight frame still samples. `Release` must defer the
  actual reclaim past the frames-in-flight window — reuse the existing
  deferred-destruction / per-frame retire queue (planset-1/04), not a second one.
- **`partiallyBound` + `UPDATE_AFTER_BIND`** are what make a sparse,
  mutated-while-bound global array legal — already enabled; the 06 addendum's
  "bindless opt-in" is where these flags legitimately live (vs. on by default).
- **Barriers do not change.** Bindless only changes how a shader *names* a
  texture, not its image state. A bindless texture sampled in a pass still needs
  its layout transition, so the **render graph still declares `.Sample(view)`** and
  derives the barrier as it does now. Bindless and the render graph are orthogonal.

## Shader side

Fixed bindless set, unbounded arrays, non-uniform indexing, **separate**
`texture2D[]` + `sampler[]` (the bindless-friendly split — thousands of images, a
handful of samplers):
```glsl
#extension GL_EXT_nonuniform_qualifier : require
layout(set = 0, binding = 0) uniform texture2D textures[];
layout(set = 0, binding = 1) uniform sampler   samplers[];
```
When offline shader reflection lands (asset phase), set 0 is recognized as
**engine-provided** — like push constants, the material author never declares it.

## MoltenVK reality

veng targets Metal via MoltenVK. The **descriptor-indexing / `runtimeDescriptorArray`**
model above is supported there (veng already enables and runs with those
features). **Avoid `VK_EXT_descriptor_buffer`** — the newer "bind a pointer instead
of a set" style is poorly supported on MoltenVK. So: global-arrays-in-a-set, not
descriptor buffers. Size the arrays against the target Metal GPUs'
`maxPerStageDescriptorUpdateAfterBindSampledImages` / `maxDescriptorSetUpdateAfterBind*`
limits.

## Layering

```
Material (asset) = shader handle + param block + { TextureHandle albedo, normal, ... }
       │
Renderer         = bind global set 0 once; per draw push { objectIndex }
       │
BindlessRegistry = global set, slot allocator, deferred free
       │
Render graph     = still declares Sample/Storage → still derives barriers
```
The material system ([area 1](README.md)) becomes thin: a material is *handles + a
param entry*, not a bundle of descriptor sets.

## Migration / sequencing

1. **06 addendum first — done.** Static-by-default + the type/feature/pool single
   source of truth landed in
   [planset-2/06](../planset-2/06-descriptor-update-policy.md): the existing
   per-set layer is coherent and bindless flags are now an explicit per-binding
   opt-in (`DescriptorBinding::Bindless`) rather than a global default. The gaps
   planset-3 pinned (`descriptor_write_paths` under `VE_DEBUG`) are closed — the
   Primary Pool sizes every `DescriptorType` via `GetDescriptorTypeInfo`
   (`TypeMapping.h`), and `descriptorBindingStorageImageUpdateAfterBind` is
   enabled, so a static `StorageImage` binding (the common case) needs neither.
2. **De-globalize `Context` (area 3)** — the registry wants an explicit device, not
   a `Context::Instance()` grab.
3. **Registry + handles, alongside the per-set API** — don't rip out `DescriptorSet`
   day one (ImGui and simple passes keep it). Migrate textures → material/object
   buffers → materials incrementally, geometry pass first (it's the deferred
   bindless-heavy pass).

## Open decisions

- **Auto-register vs. explicit.** Every `ImageView::Create` gets a slot
  automatically (simple, but everything becomes bindless) vs. explicit
  `registry.Register` (more control). Lean explicit-but-ergonomic.
- **One mega-set vs. a few typed sets** (textures / storage / buffers split across
  bindings vs. sets).
- **Buffers: arrayed SSBO descriptors vs. buffer-device-address.** BDA is already
  enabled and is cleaner for per-material/per-object blocks (push a 64-bit pointer,
  skip the descriptor) — but a sharper tool.
- **Array sizing / growth.** Fixed large capacity (simple) vs. growable (re-create
  the set when full).
- **G-buffer access in the lighting pass:** fixed descriptor set vs. routing the
  G-buffer attachments through the registry as handles too.
