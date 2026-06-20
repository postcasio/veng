#pragma once

#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Asset/Types.h>

#include <span>

namespace Veng::Cook
{
    /// @brief One entry from a parsed pack JSON: the asset's id, type, and source path.
    struct AssetPackEntry
    {
        /// @brief The asset's unique identifier.
        AssetId Id;
        /// @brief The asset type (texture, mesh, shader, …).
        AssetType Type{};
        /// @brief Source path as written in the pack JSON, relative to the pack directory.
        string Source;
    };

    /// @brief Parsed asset pack registry mapping ids to their type and source path.
    ///
    /// Pure data, populated by ParseAssetPack. Used for cross-pack id resolution
    /// during cooking and for AssetId minting.
    struct AssetPack
    {
        /// @brief Directory the pack JSON lives in; entry source paths are relative to it.
        path Dir;
        /// @brief All entries parsed from the pack JSON.
        vector<AssetPackEntry> Entries;

        /// @brief Returns the entry with the given id, or nullptr if not found.
        [[nodiscard]] const AssetPackEntry* FindById(AssetId id) const;
    };

    /// @brief Mints a random non-zero AssetId that collides with no id in any of the provided packs.
    ///
    /// Regenerates on collision (astronomically unlikely). The caller owns loading/parsing the packs.
    /// @param packs  Packs to check for collisions.
    /// @return A fresh, collision-free AssetId.
    [[nodiscard]] AssetId GenerateAssetId(std::span<const AssetPack* const> packs);
}
