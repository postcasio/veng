#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetType.h>

namespace Veng
{
    /// @brief Cooked blob bytes verbatim, with no GPU resources.
    ///
    /// A passthrough asset type so AssetManager's mount/resolve/load/cache/GC path
    /// is testable end-to-end without a render Context.
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
