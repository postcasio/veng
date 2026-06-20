#pragma once

#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Cook/Types.h>
#include <Veng/Reflection/TypeRegistry.h>

namespace Veng::Cook
{
    /// @brief Result of resolving an AssetId to its uncooked source file.
    ///
    /// The absolute path is `pack.Dir / entry.Source`; Type is carried for validation
    /// (e.g. the ShaderImporter checks that a vertex_layout reference is AssetType::VertexLayout).
    struct ResolvedSource
    {
        /// @brief Absolute path to the uncooked source file (`pack.Dir / entry.Source`).
        path AbsolutePath;
        /// @brief Asset type of the resolved entry.
        AssetType Type{};
    };

    /// @brief Cook context shared across all entries cooked from one pack.
    ///
    /// PackDir is the pack file's directory; entry `"source"` paths are relative to it so
    /// packs stay relocatable. Resolve looks up any AssetId across the pack being cooked
    /// and any reference packs, returning nullopt if unknown.
    struct CookContext
    {
        /// @brief Directory of the pack JSON file; entry source paths are relative to it.
        path PackDir;
        /// @brief Resolves an AssetId to its uncooked source path and type, or nullopt if unknown.
        function<optional<ResolvedSource>(AssetId)> Resolve;
        /// @brief Reflected module type descriptors; non-null only on a `--module` cook.
        ///
        /// The prefab importer uses this to validate component schemas; other importers ignore it.
        const TypeRegistry* Types = nullptr;
        /// @brief Records a source file the cook read, for build dependency tracking.
        ///
        /// The cooker records the pack JSON and resolved cross-asset references centrally.
        /// Importers call this for binary payloads not named in the manifest — a texture's image
        /// file, a mesh model, a shader's .slang source and its includes. Always set by the cooker.
        function<void(const path&)> RecordDependency;
    };

    /// @brief Offline, Vulkan-free importer interface: one implementation per AssetType.
    ///
    /// Registered into a Cooker and dispatched per pack entry. Cook() turns a pack entry's
    /// JSON (plus any files it references on disk) into cooked blob bytes for ArchiveWriter::Add.
    class AssetImporter
    {
    public:
        /// @brief Virtual destructor.
        virtual ~AssetImporter() = default;

        /// @brief Returns the AssetType this importer handles.
        [[nodiscard]] virtual AssetType Type() const = 0;

        /// @brief Cooks one pack entry's JSON into cooked blob bytes.
        /// @param context  Cook context providing the pack directory, asset resolver, and dependency recorder.
        /// @param entry    The pack entry JSON object for this asset.
        /// @return Cooked blob bytes, or an error string.
        [[nodiscard]] virtual Result<vector<u8>> Cook(const CookContext& context,
                                                      const json& entry) const = 0;
    };
}
