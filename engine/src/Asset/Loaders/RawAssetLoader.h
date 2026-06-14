#pragma once

#include <Veng/Asset/AssetLoader.h>
#include <Veng/Asset/RawAsset.h>

// The built-in AssetType::Raw loader: cooked blob bytes -> RawAsset, verbatim.
// Registered by AssetManager's constructor so the mount/resolve/load/cache/GC
// path is exercisable before 06-09 register real (GPU-backed) loaders.

namespace Veng
{
    class RawAssetLoader final : public AssetLoader
    {
    public:
        [[nodiscard]] AssetType Type() const override { return AssetType::Raw; }

        [[nodiscard]] AssetResult<Detail::RefAny> Load(
            AssetManager& manager, Renderer::Context& context,
            AssetId id, std::span<const u8> cooked) const override;
    };
}
