#pragma once

#include <Veng/Veng.h>

/// @brief The engine-side mirror of the core material.slang `MaterialParams` block.
///
/// The metallic-roughness parameter set every Surface material's fragment shader loads
/// from set-0 binding 4 at index * MaterialParamStride. This is the documented layout
/// the cooker patches by reflected offset and the loader writes handle indices into.
///
/// This struct is not memcpy'd into the GPU buffer directly (a material's block is
/// assembled by reflection at cook time, not from this type). It exists so its
/// sizeof/offsetof can be static_assert-guarded against the documented layout, turning
/// a drift between this header and the byte-identical Slang copies into a build error
/// rather than a silent offset-patch misread.
namespace Veng::Renderer
{
    /// @brief C++ mirror of the `MaterialParams` Slang block, used for layout static_asserts.
    struct MaterialParams
    {
        /// @brief Bindless sampled-image index (base-color texture).
        u32 BaseColor;
        /// @brief Bindless sampler index.
        u32 BaseColorSampler;
        /// @brief Bindless sampled-image index (occlusion-roughness-metallic texture).
        u32 ORM;
        /// @brief Bindless sampler index.
        u32 ORMSampler;
        /// @brief Bindless sampled-image index (tangent-space normal map; 0 = unbound).
        u32 Normal;
        /// @brief Bindless sampler index.
        u32 NormalSampler;
        /// @brief Padding to maintain std430 alignment.
        u32 Pad0;
        /// @brief Padding to maintain std430 alignment.
        u32 Pad1;
        /// @brief Tint applied to the sampled base color.
        vec4 BaseColorFactor;
        /// @brief rgb reserved; a = emissive strength.
        vec4 EmissiveFactor;
        /// @brief Metallic factor.
        f32 MetallicFactor;
        /// @brief Roughness factor.
        f32 RoughnessFactor;
        /// @brief Occlusion strength scalar.
        f32 OcclusionStrength;
        /// @brief Padding to maintain std430 alignment.
        f32 Pad2;
    };

    // std430 layout guards — a drift between this header and the Slang `MaterialParams`
    // block is a build error.
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
