#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetType.h>

namespace Veng
{
    /// @brief Cooked blob bytes verbatim, with no GPU resources.
    ///
    /// The opaque-bytes asset type: a consumer-defined binary payload cooked by
    /// RawImporter, loaded through the ordinary AssetManager path by RawAssetLoader,
    /// and referenced by an AssetHandle<RawAsset> field the reflection inspector draws
    /// as an asset picker and the prefab/level pipeline cooks and resolves like any
    /// other handle. The engine sees only bytes; what they mean is the consumer's own
    /// concern (a lookup table, a data file, a domain-specific catalogue). Carrying no
    /// GPU resource, it also exercises the mount/resolve/load/cache/GC path without a
    /// render Context.
    struct RawAsset
    {
        /// @brief The raw cooked blob bytes.
        vector<u8> Bytes;
    };

    /// @brief AssetTypeTrait specialization mapping RawAsset to AssetType::Raw.
    template <>
    struct AssetTypeTrait<RawAsset>
    {
        /// @brief The asset type tag for RawAsset.
        static constexpr AssetType Type = AssetType::Raw;
    };
}
