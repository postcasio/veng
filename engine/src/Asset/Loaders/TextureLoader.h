#pragma once

#include <Veng/Asset/AssetLoader.h>
#include <Veng/Renderer/Texture.h>

// The AssetType::Texture loader: cooked
// CookedTextureHeader + RGBA8 pixels -> Renderer::Texture, registered into the
// bindless registry by Texture::Create.

namespace Veng
{
    class TextureLoader final : public AssetLoader
    {
    public:
        [[nodiscard]] AssetType Type() const override { return AssetType::Texture; }

        [[nodiscard]] AssetResult<Detail::RefAny> Load(
            AssetManager& manager, Renderer::Context& context,
            AssetId id, std::span<const u8> cooked) const override;
    };
}
