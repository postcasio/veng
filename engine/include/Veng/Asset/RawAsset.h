#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetType.h>

// RawAsset: the cooked blob's bytes, verbatim. A passthrough asset type with no
// GPU resources, so AssetManager's mount/resolve/load/cache/GC path is testable
// end-to-end without a Context.

namespace Veng
{
    struct RawAsset
    {
        vector<u8> Bytes;
    };

    template <>
    struct AssetTypeTrait<RawAsset>
    {
        static constexpr AssetType Type = AssetType::Raw;
    };
}
