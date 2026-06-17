#pragma once

#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Veng.h>

// AssetSourceIndex: maps an AssetId to its per-asset JSON source file by parsing
// the pack manifest (the .vengpack.json the cooker reads). The editor needs the
// source path — not just the cooked blob in the archive — to edit and recook an
// asset on demand. The archive TOC carries no source path, so this is the only
// bridge from a mounted AssetId back to the file the texture editor edits.

namespace VengEditor
{
    class AssetSourceIndex
    {
    public:
        struct Entry
        {
            Veng::AssetType Type{};
            // Absolute path to the per-asset JSON source (e.g. brick.tex.json).
            Veng::path Source;
        };

        // Parses the manifest at manifestPath. Source paths in the manifest are
        // relative to the manifest's directory; they are resolved to absolute
        // here. An unreadable or malformed manifest yields an empty index (logged).
        static AssetSourceIndex Parse(const Veng::path& manifestPath);

        // The source entry for an id, or nullptr if the id is not in the manifest.
        [[nodiscard]] const Entry* Find(Veng::AssetId id) const;

        // Every manifest id of a given asset type, in unspecified order — the
        // candidate set the inspector's AssetHandle picker offers. Returned by
        // value so a caller can sort/filter without holding the index's storage.
        [[nodiscard]] Veng::vector<Veng::AssetId> EntriesOfType(Veng::AssetType type) const;

    private:
        Veng::unordered_map<Veng::u64, Entry> m_Entries;
    };
}
