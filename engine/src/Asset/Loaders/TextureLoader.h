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

        /// @brief Returns true: the decode, image creation and upload submit run on a worker.
        [[nodiscard]] bool ParsesOffThread() const override { return true; }

        /// @brief Decodes the cooked texture blob into a texture whose Finalize registers it.
        [[nodiscard]] AssetResult<Detail::ParsedAsset>
        Parse(const AssetParseContext& context, AssetId id,
              std::span<const u8> cooked) const override;

        /// @brief Decodes a cooked texture blob and builds its texture, uploading per @p parse.Async.
        ///
        /// What Parse does, for a loader embedding a texture in its own blob (a flipbook's atlas).
        /// @param parse   The parse context the embedding loader was handed.
        /// @param id      The asset being loaded, for naming and errors.
        /// @param cooked  The cooked texture blob.
        /// @return The texture's job — its Finalize registers it — or the decode error.
        [[nodiscard]] static AssetResult<Detail::LoadJob>
        PrepareTexture(const AssetParseContext& parse, AssetId id, std::span<const u8> cooked);
    };
}
