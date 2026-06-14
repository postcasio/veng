#pragma once

#include <Veng/Asset/AssetLoader.h>
#include <Veng/Asset/RawAsset.h>

// The built-in AssetType::Raw loader: cooked blob bytes -> RawAsset, verbatim.
// Registered by AssetManager's constructor so the mount/resolve/load/cache/GC
// path is exercisable without requiring GPU-backed loaders.

namespace Veng
{
    class RawAssetLoader final : public AssetLoader
    {
    public:
        [[nodiscard]] AssetType Type() const override { return AssetType::Raw; }

        [[nodiscard]] AssetResult<Detail::LoadJob> Load(
            AssetManager& manager, Renderer::Context& context, TaskSystem& tasks,
            AssetId id, std::span<const u8> cooked, bool async) const override;
    };
}
