#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetType.h>

// RawAsset (planset-5 plan 04): the cooked blob's bytes, verbatim. The only
// concrete asset type this plan loads — exists so AssetManager's
// mount/resolve/load/cache/GC path is testable end-to-end before 06-09 add
// real (GPU-backed) types.

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
