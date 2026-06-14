#pragma once

#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Cook/Types.h>

// The cooker-side importer interface (planset-5 plan 03): one AssetImporter
// per AssetType, registered into a Cooker and dispatched per pack entry.
// Cook() is offline and Vulkan-free — it turns a pack entry's JSON, plus
// whatever it references on disk, into cooked blob bytes ready for
// ArchiveWriter::Add.

namespace Veng::Cook
{
    // Result of resolving an AssetId to its uncooked source file. The absolute
    // path is pack.Dir / entry.Source; the type is carried for validation
    // (e.g. the ShaderImporter checks that a vertex_layout reference is actually
    // AssetType::VertexLayout).
    struct ResolvedSource
    {
        path AbsolutePath; // pack.Dir / entry.Source
        AssetType Type{};
    };

    // Shared across all entries cooked from one pack. PackDir is the pack
    // file's directory — entry "source" paths are relative to it, so packs
    // stay relocatable. Resolve looks up any AssetId across the pack being
    // cooked and any --reference packs (uncooked sources); nullopt if unknown.
    struct CookContext
    {
        path PackDir;
        function<optional<ResolvedSource>(AssetId)> Resolve;
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
