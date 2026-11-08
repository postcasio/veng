// The portable resource-declaration form. One source compiles to SPIR-V and to Metal, so a shader
// declares each descriptor set as one ParameterBlock: on SPIR-V the block's fields take the set's
// bindings 0..N in field order, and on Metal the block is one argument buffer whose fields are its
// elements. A set declared as loose [[vk::binding]] globals cannot reach Metal: Slang flattens them
// into individual entry-point parameters with nothing grouping them by set.
//
// The Metal buffer slots below mirror Veng/Cook/MetalBindingSlots.h, and the cook checks every
// shader's reflected slots against that header.
//
//   VE_DESCRIPTOR_SET(set, Type, name)
//       Declares set `set` as ParameterBlock<Type> at binding 0, in Metal buffer slot `set`. A
//       block must sit at binding 0 of its set, and a [[vk::binding]] on one of its fields is
//       ignored, so every field of the set belongs in the struct, in binding order. A combined
//       sampler (Sampler2D) a stage never samples is emitted on Metal as a lone texture and
//       misplaces every later field, which the cook refuses; declare Texture + SamplerState.
//   VE_DYNAMIC_UNIFORM_BUFFER(binding, set, ordinal, Type, name)
//       Declares a uniform buffer bound with a dynamic offset. It stays outside its set's block,
//       bound by address at the reserved slot for the ordinal-th dynamic buffer of the pipeline
//       layout (counted in set, then binding, order), so its binding follows every block field.
//   VE_PUSH_CONSTANTS(Type, name)
//       Declares the push-constant block, bound by address at the reserved push-constant slot.
#pragma once

// The target-portable spellings of the intrinsics one target lacks.
#include "portable.slang"

#define VE_CAT_(a, b) a##b
#define VE_CAT(a, b) VE_CAT_(a, b)

#define VE_METAL_DYNAMIC_UNIFORM_BUFFER_SLOT_0 8
#define VE_METAL_DYNAMIC_UNIFORM_BUFFER_SLOT_1 9
#define VE_METAL_DYNAMIC_UNIFORM_BUFFER_SLOT_2 10
#define VE_METAL_DYNAMIC_UNIFORM_BUFFER_SLOT_3 11
#define VE_METAL_PUSH_CONSTANT_SLOT 12

#define VE_DESCRIPTOR_SET(setIndex, Type, name)                                                    \
    [[vk::binding(0, setIndex)]] ParameterBlock<Type> name : register(VE_CAT(b, setIndex))

#define VE_DYNAMIC_UNIFORM_BUFFER(bindingIndex, setIndex, ordinal, Type, name)                     \
    [[vk::binding(bindingIndex, setIndex)]] ConstantBuffer<Type> name                              \
        : register(VE_CAT(b, VE_CAT(VE_METAL_DYNAMIC_UNIFORM_BUFFER_SLOT_, ordinal)))

#define VE_PUSH_CONSTANTS(Type, name)                                                              \
    [[vk::push_constant]] ConstantBuffer<Type> name : register(VE_CAT(b, VE_METAL_PUSH_CONSTANT_SLOT))

// A resource index that may differ between invocations. SPIR-V needs the NonUniform decoration on
// such an index; Metal has no counterpart and indexes an argument buffer as given. It must inline:
// called through a function boundary, the decoration does not reach the access on SPIR-V.
[ForceInline]
uint NonUniformIndex(uint index)
{
    __target_switch
    {
    case spirv:
        return NonUniformResourceIndex(index);
    default:
        return index;
    }
}
