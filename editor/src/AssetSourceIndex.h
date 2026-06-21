#pragma once

#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Veng.h>

/// @brief Maps an AssetId to its per-asset JSON source file by parsing the pack
/// manifest (.vengpack.json). The archive TOC carries no source path; this index
/// is the only bridge from a mounted AssetId back to the file an asset editor edits.

namespace VengEditor
{
    /// @brief AssetId-to-source-file index built from the pack manifest.
    class AssetSourceIndex
    {
    public:
        /// @brief One manifest entry: asset type and source paths.
        struct Entry
        {
            /// @brief Asset type of this entry.
            Veng::AssetType Type{};
            /// @brief Absolute path to the per-asset JSON source (e.g. brick.tex.json).
            Veng::path Source;
            /// @brief Source path as written in the manifest, relative to its directory.
            ///
            /// The folder structure the asset browser builds its tree from
            /// (e.g. textures/brick_basecolor.tex.json).
            Veng::path RelativeSource;
        };

        /// @brief Parses the manifest at manifestPath and returns the resulting index.
        ///
        /// Source paths in the manifest are relative to the manifest's directory and
        /// are resolved to absolute paths here. An unreadable or malformed manifest
        /// yields an empty index (logged via Log::Error).
        /// @param manifestPath Path to the .vengpack.json manifest.
        static AssetSourceIndex Parse(const Veng::path& manifestPath);

        /// @brief Returns the source entry for an id, or nullptr when not in the manifest.
        [[nodiscard]] const Entry* Find(Veng::AssetId id) const;

        /// @brief Returns all manifest ids of a given asset type, in unspecified order.
        ///
        /// Returned by value so the caller can sort/filter without holding the index's storage.
        /// Used by the inspector's AssetHandle picker to enumerate candidates.
        [[nodiscard]] Veng::vector<Veng::AssetId> EntriesOfType(Veng::AssetType type) const;

        /// @brief Invokes @p fn for every manifest entry, in unspecified order.
        ///
        /// The asset browser enumerates the manifest to build its source-path folder tree.
        /// @param fn  Visitor called with each entry's id and its Entry record.
        void ForEachEntry(const Veng::function<void(Veng::AssetId, const Entry&)>& fn) const;

    private:
        Veng::unordered_map<Veng::u64, Entry> m_Entries;
    };
}
