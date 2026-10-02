#pragma once

#include <Veng/Asset/AssetLoader.h>
#include <Veng/Asset/RawAsset.h>

namespace Veng
{
    /// @brief AssetTypes::Raw loader: copies the cooked blob bytes verbatim into a RawAsset.
    ///
    /// Registered unconditionally by AssetManager so the mount/resolve/load/cache/GC
    /// path is exercisable without GPU-backed loaders.
    class RawAssetLoader final : public AssetLoader
    {
    public:
        /// @brief Returns AssetTypes::Raw.
        [[nodiscard]] AssetTypeId Type() const override { return AssetTypes::Raw; }

        /// @brief Returns true: the decode runs on a worker for an asynchronous load.
        [[nodiscard]] bool ParsesOffThread() const override { return true; }

        /// @brief Copies the cooked blob bytes verbatim into a resident RawAsset.
        [[nodiscard]] AssetResult<Detail::ParsedAsset>
        Parse(const AssetParseContext& context, AssetId id,
              std::span<const u8> cooked) const override;
    };
}
