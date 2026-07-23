#pragma once

#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Veng.h>
#include <Veng/Path.h>

#include <span>

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
            Veng::AssetTypeId Type{};
            /// @brief Absolute path to the per-asset JSON source (e.g. brick.tex.json).
            Veng::path Source;
            /// @brief Source path as written in the manifest, relative to its directory.
            ///
            /// The folder structure the asset browser builds its tree from
            /// (e.g. textures/brick_basecolor.tex.json).
            Veng::path RelativeSource;
            /// @brief True for an entry the index synthesized rather than read from a manifest.
            ///
            /// A material's companion default MaterialInstance is emitted by the cook, not authored,
            /// so it has no editable source of its own (Source is empty). The index registers it to
            /// give the browser a name and folder; an asset editor must refuse to open it.
            bool Synthesized = false;
        };

        /// @brief Parses the manifest at manifestPath and returns the resulting index.
        ///
        /// Source paths in the manifest are relative to the manifest's directory and
        /// are resolved to absolute paths here. An unreadable or malformed manifest
        /// yields an empty index (logged via Log::Error).
        /// @param manifestPath Path to the .vengpack.json manifest.
        /// @param types        Registry each entry's "type" name resolves through; an entry
        ///                     naming an unregistered type is skipped.
        static AssetSourceIndex Parse(const Veng::path& manifestPath,
                                      const Veng::AssetTypeRegistry& types);

        /// @brief Parses several manifests and returns their merged index.
        ///
        /// The project's packs are parsed in order and their entries unioned (a later pack's
        /// entry for the same id wins). Used by the editor host to index every pack the project
        /// owns from one AssetId→source map.
        /// @param manifestPaths The pack manifests to merge.
        /// @param types         Registry each entry's "type" name resolves through.
        static AssetSourceIndex ParsePacks(std::span<const Veng::path> manifestPaths,
                                           const Veng::AssetTypeRegistry& types);

        /// @brief Returns the source entry for an id, or nullptr when not in the manifest.
        [[nodiscard]] const Entry* Find(Veng::AssetId id) const;

        /// @brief Returns all manifest ids of a given asset type, in unspecified order.
        ///
        /// Returned by value so the caller can sort/filter without holding the index's storage.
        /// Used by the inspector's AssetHandle picker to enumerate candidates.
        [[nodiscard]] Veng::vector<Veng::AssetId> EntriesOfType(Veng::AssetTypeId type) const;

        /// @brief Invokes @p fn for every manifest entry, in unspecified order.
        ///
        /// The asset browser enumerates the manifest to build its source-path folder tree.
        /// @param fn  Visitor called with each entry's id and its Entry record.
        void ForEachEntry(const Veng::function<void(Veng::AssetId, const Entry&)>& fn) const;

        /// @brief Returns the asset-type registry the index was built against.
        ///
        /// The index is the display surface's carrier for it: every site that renders a type
        /// already holds the index the type came from.
        [[nodiscard]] const Veng::AssetTypeRegistry& GetAssetTypes() const { return *m_Types; }

    private:
        Veng::unordered_map<Veng::u64, Entry> m_Entries;
        /// @brief The registry the entries' types were resolved through; never null after a Parse.
        const Veng::AssetTypeRegistry* m_Types = nullptr;
    };
}
