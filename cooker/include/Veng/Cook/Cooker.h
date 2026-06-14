#pragma once

#include <Veng/Asset/Archive.h>
#include <Veng/Cook/AssetPack.h>
#include <Veng/Cook/Importer.h>

#include <set>
#include <span>
#include <unordered_map>

// The cooker: owns the importer table and turns a JSON
// asset pack into a .vengpack archive.
//
// Pack JSON schema:
//   { "version": 1, "assets": [ { "id": <u64>, "type": "<name>", ... }, ... ] }
// - "id" is the author-owned AssetId (any non-zero u64); 0 and duplicates
//   within a pack are cook errors.
// - "type" selects the registered AssetImporter ("raw", "texture", "mesh",
//   "shader", "material", "vertex_layout"); an unknown type, or one with no
//   registered importer, is a cook error naming the entry.
// - Per-type fields (e.g. "source") are interpreted by that type's importer.

namespace Veng::Cook
{
    // Parses a pack JSON file into an AssetPack (id/type/source registry).
    // Used for cross-pack resolution and AssetId generation.
    // Errors are located: "pack '<path>': <reason>".
    [[nodiscard]] Result<AssetPack> ParseAssetPack(const path& packJson);

    class Cooker
    {
    public:
        void Register(Unique<AssetImporter> importer);

        // Parses packJson, cooks every entry through its registered importer,
        // and writes the resulting archive to outArchive. referencePacks lists
        // additional uncooked pack JSON files whose assets can be resolved by
        // AssetId during cooking (e.g. a core layout pack). Errors are located:
        // "pack '<path>': asset[<n>]: <reason>".
        [[nodiscard]] VoidResult CookPack(const path& packJson, const path& outArchive,
            std::span<const path> referencePacks = {}) const;

    private:
        [[nodiscard]] VoidResult CookEntry(const CookContext& context, const json& entry,
            std::set<u64>& seenIds, ArchiveWriter& writer) const;

        std::unordered_map<AssetType, Unique<AssetImporter>> m_Importers;
    };
}
