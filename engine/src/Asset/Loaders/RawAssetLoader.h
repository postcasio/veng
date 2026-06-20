#pragma once

#include <Veng/Asset/AssetLoader.h>
#include <Veng/Asset/RawAsset.h>

namespace Veng
{
    /// @brief AssetType::Raw loader: copies the cooked blob bytes verbatim into a RawAsset.
    ///
    /// Registered unconditionally by AssetManager so the mount/resolve/load/cache/GC
    /// path is exercisable without GPU-backed loaders.
    class RawAssetLoader final : public AssetLoader
    {
    public:
        /// @brief Returns AssetType::Raw.
        [[nodiscard]] AssetType Type() const override { return AssetType::Raw; }

        /// @brief Copies the cooked blob bytes verbatim into a LoadJob producing a resident RawAsset.
        [[nodiscard]] AssetResult<Detail::LoadJob> Load(
            AssetManager& manager, Renderer::Context& context, TaskSystem& tasks,
            TypeRegistry& types, AssetId id, std::span<const u8> cooked, bool async) const override;
    };
}
