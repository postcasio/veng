#pragma once

#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Asset/Types.h>

#include <span>

namespace Veng::Cook
{
    // A parsed asset pack registry: id -> (type, source). Pure data, populated
    // by ParseAssetPack (which owns JSON parsing) — assetformat stays JSON-free.
    // Used for cross-pack id resolution during cooking and for AssetId minting.
    struct AssetPackEntry
    {
        AssetId Id;
        AssetType Type{};
        string Source; // source path as written in the pack JSON, relative to the pack dir
    };

    struct AssetPack
    {
        path Dir;                       // directory the pack JSON lives in (source paths are relative to it)
        vector<AssetPackEntry> Entries;

        [[nodiscard]] const AssetPackEntry* FindById(AssetId id) const;
    };

    // Mints a random non-zero u64 AssetId that collides with no id in any of the
    // provided packs. Regenerates on the astronomically-unlikely collision. The
    // caller owns loading/parsing the packs.
    [[nodiscard]] AssetId GenerateAssetId(std::span<const AssetPack* const> packs);
}
