#pragma once

#include <Veng/Asset/AssetLoader.h>
#include <Veng/Asset/Texture.h>

// The AssetType::Texture loader: cooked CookedTextureHeader + RGBA8 pixels ->
// Veng::Texture. Creation + upload is the worker-legal half; registration into
// the bindless registry is the deferred main-thread Finalize.

namespace Veng
{
    class TextureLoader final : public AssetLoader
    {
    public:
        [[nodiscard]] AssetType Type() const override { return AssetType::Texture; }

        [[nodiscard]] AssetResult<Detail::LoadJob> Load(
            AssetManager& manager, Renderer::Context& context, TaskSystem& tasks,
            AssetId id, std::span<const u8> cooked, bool async) const override;
    };
}
