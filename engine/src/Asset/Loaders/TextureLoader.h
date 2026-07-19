#pragma once

#include <Veng/Asset/AssetLoader.h>
#include <Veng/Asset/Texture.h>

namespace Veng
{
    /// @brief AssetTypes::Texture loader.
    ///
    /// Decodes a CookedTextureHeader + pixel data into a Veng::Texture. Image creation
    /// and upload are worker-legal; bindless registration is deferred to the main-thread
    /// Finalize so the handle is assigned on the correct thread.
    class TextureLoader final : public AssetLoader
    {
    public:
        /// @brief Returns AssetTypes::Texture.
        [[nodiscard]] AssetTypeId Type() const override { return AssetTypes::Texture; }

        /// @brief Decodes the cooked texture blob into a LoadJob producing a resident Veng::Texture.
        [[nodiscard]] AssetResult<Detail::LoadJob> Load(AssetManager& manager,
                                                        Renderer::Context& context,
                                                        TaskSystem& tasks, TypeRegistry& types,
                                                        AssetId id, std::span<const u8> cooked,
                                                        bool async) const override;
    };
}
