# Declaring shader resources for every GPU backend

A veng shader is one Slang source that cooks to SPIR-V for the Vulkan backend and to a Metal library
for the Metal backend. The two targets reach a shader's resources differently, and the declaration
form below is the one that reaches both. This guide covers the form, the binding model the cook and
the Metal backend share, the data-layout rule a constant block or structured buffer must keep, and
what the cook checks.

## Which targets a shader cooks to

`VE_SHADER_TARGETS` (a cache string beside `VE_GPU_BACKEND`) decides:

- **`Native`** (the default) cooks the target of the tree's backend: SPIR-V in a Vulkan tree. A
  Vulkan tree's cook never runs the Metal toolchain, so a shader that Metal would reject cannot break
  a Vulkan build.
- **`All`** cooks SPIR-V and Metal, for a pack that must carry both.

The exported package records the setting as `VENG_SHADER_TARGETS`, and `veng_add_asset_pack` and
`veng_add_project` pass it to every cook, so a consumer's packs cook for the backend of the engine
tree it builds against. `vengc cook` and `cook-project` take `--shader-targets` (`Native`, `All`,
or a list of `spirv` and `metal`) to override it.

A cooked shader carries one section per target. The runtime loads the section for the backend it was
built with, and refuses a shader without one, naming the asset.

Whenever Metal is cooked, the cook also holds the shader to the binding model below and checks its
Metal layout against its SPIR-V layout, so a shader passes on one target only if it binds alike on
both. In a `VE_GPU_BACKEND=Metal` tree a missing Metal toolchain (`xcrun metal`) is a configure-time
error; under `All` in a Vulkan tree the cook drops the Metal section and says so once per pack. The
toolchain's version is part of every shader's cook-cache key, so updating Xcode recooks them.

## The declaration form

Declare each descriptor set as one `ParameterBlock`, through the macros in `Veng/binding.slang`:

```slang
#include "Veng/bindless.slang" // sets 0-2, and Veng/binding.slang

struct BlurSet
{
    Texture2D<float4> Source;
    SamplerState Sampler;
    [format("rgba16f")] RWTexture2D<float4> Dest;
};
VE_DESCRIPTOR_SET(3, BlurSet, g_Blur);

struct BlurPush
{
    float2 Direction;
    uint Radius;
};
VE_PUSH_CONSTANTS(BlurPush, g_PC);

// ... g_Blur.Source.SampleLevel(g_Blur.Sampler, uv, 0.0) ...
```

- **`VE_DESCRIPTOR_SET(set, Type, name)`** declares `[[vk::binding(0, set)]] ParameterBlock<Type>
  name : register(b<set>)`. On SPIR-V the struct's fields take the set's bindings 0, 1, 2, … in field
  order, so the SPIR-V bindings are the ones loose `[[vk::binding]]` globals would have had. On Metal
  the block is one argument buffer, pinned at the buffer slot its set index names.
- **Every binding of the set is a field, in binding order.** A block must sit at binding 0 of its set,
  and a `[[vk::binding]]` on one of its fields is silently ignored, so a field cannot be moved to
  another binding; a set whose bindings are shared by several stages declares the ones a stage does
  not use too, so the fields after them keep their bindings. A block holds resources only — plain
  data belongs in a `ConstantBuffer<T>` field.
- **`VE_DYNAMIC_UNIFORM_BUFFER(binding, set, ordinal, Type, name)`** declares a uniform buffer bound
  with a dynamic offset. It stays outside the block, bound by address, so its binding comes after
  every block field in its set. `ordinal` is its position among the pipeline layout's dynamic buffers,
  counted in set then binding order.
- **`VE_PUSH_CONSTANTS(Type, name)`** declares the push-constant block. Per-stage ranges are fields at
  their absolute offsets within the one block.
- **The bindless registries** (set 0 the 2D textures, samplers, storage images and buffers and the
  per-frame buffers; set 1 the volumes; set 2 the cubes) are declared once, in `Veng/bindless.slang`,
  with each array sized by its registry capacity — a Metal argument buffer holds no unsized array.
  The `g_Textures`, `g_Samplers`, `g_Volumes` … names every engine header has always exposed stay
  available as aliases of its fields.
- **`NonUniformIndex(i)`** marks an index that may differ between invocations. SPIR-V needs the
  `NonUniform` decoration; Metal has no `NonUniformResourceIndex` at all. The wrapper is
  `[ForceInline]`, and must stay so: behind a function call the SPIR-V decoration is silently lost.
- **`Veng/portable.slang`** (included by `Veng/binding.slang`) spells the few intrinsics one target
  lacks: `DdxFine`/`DdyFine` and `WavePrefixCount`. A vertex shader reads the raw vertex index through
  `SV_VulkanVertexID` and the instance index through `SV_VulkanInstanceID`; both reach Metal as its
  `vertex_id` and `instance_id`, which already include the draw's base vertex and base instance.

The loose `[[vk::binding]]` globals form keeps compiling for SPIR-V, so a shader still written that
way cooks in a Vulkan tree. It cannot reach Metal: Slang flattens loose globals into individual
entry-point parameters with nothing grouping them by set, and an unsized array in them is rejected by
the Metal compiler. The cook names any such binding when Metal is cooked.

## The binding model

The Metal backend translates the engine's binding vocabulary onto Metal 4's argument tables; the cook
records what the translation needs. The buffer slots are constants in
`Veng/Cook/MetalBindingSlots.h`, shared by the cook and the backend, and mirrored in
`Veng/binding.slang`.

| What | On Metal | Slot |
|---|---|---|
| Descriptor set N | One argument buffer | N (0–7) |
| A binding | One element of its set's argument buffer; the cooked interface records the first element | — |
| A combined image sampler | Two elements, the texture then the sampler | — |
| An arrayed binding | One element per array entry, from its first element | — |
| A dynamic uniform buffer | Bound by address (base plus dynamic offset), outside its set's argument buffer, so a rebind never rewrites memory the GPU may be reading | 8 + ordinal (8–11) |
| Push constants | Copied into a per-frame ring and bound by address | 12 |
| Vertex buffer binding N | Bound by address | 16 + N (16–30) |

Each argument-buffer element is 8 bytes: a resource ID or a GPU address. The cooked interface's
`CookedDescriptorBinding::MetalBinding` says whether a binding is an argument-buffer element or a
buffer slot, and `MetalIndex` gives the element or the slot.

What Slang emits for each shape of binding, compiled to a `.metallib` and compared with the SPIR-V
reflection:

| Shape | Metal | Agrees with SPIR-V |
|---|---|---|
| Bindless sets 0/1/2 | One argument buffer each, `constant S*` at `[[buffer(n)]]`; each array sized, at its base plus stride 8 | Yes: the fields take consecutive bindings |
| Typed set: comparison sampler, storage image, 3D texture | One element each | Yes |
| Combined image sampler (`Sampler2D`) | Two elements, the texture at +0 and the sampler at +8 | Yes (one SPIR-V binding) |
| Arrayed binding | A base element plus its count, stride 8 | Yes |
| Dynamic uniform buffer | `constant T*` at its own pinned `[[buffer(n)]]` | Yes, with its binding after every block field |
| Push constants | `constant T*` at the pinned push slot | Yes |
| Vertex input | `[[vk::location(n)]]` becomes `[[attribute(n)]]` under `[[stage_in]]` | Yes |
| `Wave*` operations | `simd_sum`, `simd_prefix_exclusive_sum`, `simd_broadcast_first`, `simd_ballot`, `simd_any`, `simd_max` | — |
| `SV_VulkanInstanceID` | `[[instance_id]]`, which on Metal already includes the base instance | — |

`precise` is not kept on either target: Slang's Metal target drops the qualifier, and its SPIR-V
target emits no `NoContraction` for it in the cook's default floating-point mode. Both targets compile
with fast math, so a computation whose accuracy rests on per-operation rounding has to be structured
to survive contraction.

## Data layout agrees across targets

Where the bindings agree, the layout of the data behind them can still differ:

- **In a constant or push block, a Metal `float3` is 16 bytes**, so a scalar after it sits 4 bytes
  later than on SPIR-V (`float3 Direction; float Scale;` puts `Scale` at 16 on Metal against 12).
- **In a structured-buffer element, Metal packs a `float3` to 12 bytes**, so a `float3` after a
  `float2` sits at 8 against std430's 16, and the element's size (its array stride) can differ too.
- **`ByteAddressBuffer.Load<T>`** reads at 4-byte scalar offsets on both targets, and agrees.

No Slang layout flag moves the Metal side, so the cook compares every member offset (and every array
stride, and the size of a structured-buffer element) of every constant block, push block and
structured-buffer element between the two layouts, and fails naming the shader, the struct and the
member. Rewrite such a struct so both layouts agree **without moving any SPIR-V offset**, so the C++
mirror stays as it is:

- **Pack a `float3` and the scalar after it into a `float4`**, and keep the old names as properties,
  which take no space in the layout:

  ```slang
  struct Light
  {
      float4 DirectionScale;
      property float3 Direction { get { return DirectionScale.xyz; } }
      property float Scale { get { return DirectionScale.w; } }
  };
  ```

- **Pad** a structured-buffer element so a `float3` starts on a 16-byte boundary and the element is a
  whole multiple of 16 bytes on both targets.
