#pragma once

#include <Veng/Asset/AssetType.h>
#include <Veng/Cook/Types.h>

// The cooker-side importer interface (planset-5 plan 03): one AssetImporter
// per AssetType, registered into a Cooker and dispatched per pack entry.
// Cook() is offline and Vulkan-free — it turns a pack entry's JSON, plus
// whatever it references on disk, into cooked blob bytes ready for
// ArchiveWriter::Add.

namespace Veng::Cook
{
    // Shared across all entries cooked from one pack. PackDir is the pack
    // file's directory — entry "source" paths are relative to it, so packs
    // stay relocatable.
    struct CookContext
    {
        path PackDir;
    };

    class AssetImporter
    {
    public:
        virtual ~AssetImporter() = default;

        [[nodiscard]] virtual AssetType Type() const = 0;

        // Cooks one pack entry's JSON into cooked blob bytes for its type.
        [[nodiscard]] virtual Result<vector<u8>> Cook(const CookContext& context, const json& entry) const = 0;
    };
}
