#pragma once

#include <Veng/Veng.h>

// The engine-side mirror of the core material.slang `MaterialParams` block — the
// metallic-roughness parameter set every Surface material's fragment shader loads
// from set-0 binding 4 at index * MaterialParamStride. It is the documented layout
// the cooker patches by reflected offset and the loader writes handle indices into.
//
// This struct is not bound or memcpy'd into the GPU buffer (a material's block is
// assembled by reflection at cook time, not from this type). It exists so its
// sizeof/offsetof can be static_assert-guarded against the documented layout,
// turning a drift between this header and the byte-identical Slang copies into a
// build error rather than a silent offset-patch misread.
namespace Veng::Renderer
{
    struct MaterialParams
    {
        u32 BaseColor;          // bindless sampled-image index (base-color texture handle)
        u32 BaseColorSampler;   // bindless sampler index
        u32 ORM;                // bindless sampled-image index (occlusion-roughness-metallic)
        u32 ORMSampler;         // bindless sampler index
        u32 Normal;             // bindless sampled-image index (tangent-space normal map; 0 = unbound)
        u32 NormalSampler;      // bindless sampler index
        u32 Pad0;
        u32 Pad1;
        vec4 BaseColorFactor;   // tint applied to the sampled base color
        vec4 EmissiveFactor;    // rgb reserved; a = emissive strength
        f32 MetallicFactor;
        f32 RoughnessFactor;
        f32 OcclusionStrength;
        f32 Pad2;
    };

    // The std430 layout the Slang `MaterialParams` block reflects to. A drift here
    // is a build error.
    static_assert(sizeof(MaterialParams) == 80, "MaterialParams must be 80 bytes");
    static_assert(offsetof(MaterialParams, BaseColor) == 0);
    static_assert(offsetof(MaterialParams, BaseColorSampler) == 4);
    static_assert(offsetof(MaterialParams, ORM) == 8);
    static_assert(offsetof(MaterialParams, ORMSampler) == 12);
    static_assert(offsetof(MaterialParams, Normal) == 16);
    static_assert(offsetof(MaterialParams, NormalSampler) == 20);
    static_assert(offsetof(MaterialParams, Pad0) == 24);
    static_assert(offsetof(MaterialParams, Pad1) == 28);
    static_assert(offsetof(MaterialParams, BaseColorFactor) == 32);
    static_assert(offsetof(MaterialParams, EmissiveFactor) == 48);
    static_assert(offsetof(MaterialParams, MetallicFactor) == 64);
    static_assert(offsetof(MaterialParams, RoughnessFactor) == 68);
    static_assert(offsetof(MaterialParams, OcclusionStrength) == 72);
    static_assert(offsetof(MaterialParams, Pad2) == 76);
}
