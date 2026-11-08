#pragma once

// The definition of ImGuiTexture::Native and the ImTextureID encoding, private to the ImGui layer.
// A registration is two bindless slots, so it names no graphics API and every backend shares it.

#include <Veng/ImGui/ImGuiTexture.h>
#include <Veng/Renderer/BindlessRegistry.h>

namespace Veng
{
    struct ImGuiTexture::Native
    {
        /// @brief The sampled-image slot the registered view occupies; released on destruction.
        Renderer::TextureHandle Texture;
        /// @brief The registry-shared sampler slot matching the registered sampler's description.
        Renderer::SamplerHandle Sampler;
    };

    /// @brief The two bindless slots an ImTextureID names.
    struct ImGuiTextureSlots
    {
        /// @brief Slot in the sampled-image array.
        u32 Texture = 0;
        /// @brief Slot in the sampler array.
        u32 Sampler = 0;
    };

    /// @brief The bit every packed id carries, so no registration packs to zero, which ImGui
    ///        reserves as the invalid id. Sampler slots are far fewer than 2^31, so it is free.
    inline constexpr u64 ImGuiTextureIdTag = u64{1} << 63;

    /// @brief Packs a texture slot (low 32 bits) and a sampler slot (the bits above) into an id.
    [[nodiscard]] constexpr u64 PackImGuiTextureId(const Renderer::TextureHandle texture,
                                                   const Renderer::SamplerHandle sampler)
    {
        return ImGuiTextureIdTag | (u64{sampler.Index} << 32) | u64{texture.Index};
    }

    /// @brief Recovers the two slots PackImGuiTextureId packed.
    [[nodiscard]] constexpr ImGuiTextureSlots UnpackImGuiTextureId(const u64 id)
    {
        return {
            .Texture = static_cast<u32>(id & 0xFFFFFFFFu),
            .Sampler = static_cast<u32>((id & ~ImGuiTextureIdTag) >> 32),
        };
    }
}
