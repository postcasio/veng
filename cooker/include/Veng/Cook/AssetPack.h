#pragma once

#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Asset/Types.h>
#include <Veng/Asset/Path.h>

#include <span>

namespace Veng::Cook
{
    /// @brief One entry from a parsed pack JSON: the asset's id, type, and source path.
    struct AssetPackEntry
    {
        /// @brief The asset's unique identifier.
        AssetId Id;
        /// @brief The asset type (texture, mesh, shader, …).
        AssetTypeId Type{};
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

    /// @brief Mints a random non-zero AssetTypeId that collides with nothing in `existing`.
    ///
    /// The AssetTypeId analogue of GenerateAssetId, and the in-process form of
    /// `vengc generate-asset-type`. Pass a registry pre-filled with the engine builtins (and, for
    /// a host that has loaded one, a module's registrations) so the minted id avoids every type
    /// already claimed.
    /// @param existing  The registry the minted id must not collide with.
    /// @return A fresh, collision-free AssetTypeId.
    [[nodiscard]] AssetTypeId GenerateAssetTypeId(const AssetTypeRegistry& existing);
}
