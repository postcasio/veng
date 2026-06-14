# Bindless descriptors — design (shipped in planset-5)

> **Shipped — delivered by [planset-5](../planset-5/README.md) (plan 05)** as the
> `BindlessRegistry` set-0 subsystem (`Veng/Renderer/BindlessRegistry.h`):
> arrayed, `partiallyBound` + `updateAfterBind` bindings for sampled images,
> samplers, storage images, and a per-material `MaterialData` SSBO, all in **set 0**
> (reserved by every `PipelineLayout`), bound once per pipeline bind, with typed
> `u32` handles and deferred slot release through the per-frame retire window. The
> model below describes what was built; the **Open decisions** at the end record
> what was chosen and what genuinely remains for a later (deferred-renderer /
> growth) pass. Builds on the
> [planset-2/06 addendum](../planset-2/06-descriptor-update-policy.md), which fixed
> the *altitude* of the per-set descriptor layer this subsystem hands off from.

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
2. **De-globalize `Context` (area 3) — done (planset-4).** The registry takes an
   explicit `Context&`, not a `Context::Instance()` grab.
3. **Registry + handles, alongside the per-set API — done (planset-5, plan 05).**
   `DescriptorSet` was not ripped out (ImGui and simple passes keep it); textures
   and materials register into set 0 while per-frame/per-pass data stays in the
   per-set layer. The deferred geometry-pass migration waits on a deferred
   renderer.

## Open decisions

Resolved by planset-5 (plan 05):

- ~~**Auto-register vs. explicit.**~~ **Explicit** — `BindlessRegistry::Register`
  (and `RegisterStorage` / `RegisterMaterial`) hands back a typed handle; nothing
  becomes bindless implicitly on `Create`.
- ~~**One mega-set vs. a few typed sets.**~~ **One set (set 0)** with typed
  bindings (`TextureBinding` 0, `SamplerBinding` 1, `StorageImageBinding` 2,
  `MaterialBinding` 3), not a set per type.

Still open (a deferred-renderer / scaling pass):

- **Buffers beyond `MaterialData`: arrayed SSBO descriptors vs.
  buffer-device-address.** The per-material `MaterialData` ships as one arrayed
  SSBO (binding 3). A general per-object buffer-handle path — BDA (already enabled,
  cleaner for per-object blocks) vs. more arrayed SSBO descriptors — is not yet
  chosen; no per-object SSBO exists until the renderer needs one.
- **Array sizing / growth.** Fixed capacities shipped (`MaxTextures` 1024,
  `MaxSamplers` 128, `MaxStorageImages` 512, `MaxMaterials` 256). Growable arrays
  (re-create the set when full) are still future.
- **G-buffer access in the lighting pass:** fixed descriptor set vs. routing the
  G-buffer attachments through the registry as handles too — undecided; veng has
  no deferred path yet.
