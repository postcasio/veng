#pragma once

#include <Veng/Asset/Path.h>

#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Cook/Types.h>
#include <Veng/Project/BuildConfiguration.h>
#include <Veng/Reflection/TypeRegistry.h>

namespace Veng
{
    class SystemRegistry;
}

namespace Veng::Cook
{
    /// @brief Result of resolving an AssetId to its uncooked source file.
    ///
    /// The absolute path is `pack.Dir / entry.Source`; Type is carried for validation
    /// (e.g. the ShaderImporter checks that a vertex_layout reference is AssetTypes::VertexLayout).
    struct ResolvedSource
    {
        /// @brief Absolute path to the uncooked source file (`pack.Dir / entry.Source`).
        path AbsolutePath;
        /// @brief Asset type of the resolved entry.
        AssetTypeId Type{};
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
        /// @brief The asset-type identities this cook recognizes; always set by the cooker.
        ///
        /// An importer reads it to name a type in a diagnostic; type *dispatch* keys on the id
        /// value and never consults it.
        const AssetTypeRegistry* AssetTypes = nullptr;
        /// @brief Reflected module type descriptors; non-null only on a `--module` cook.
        ///
        /// The prefab importer uses this to validate component schemas; other importers ignore it.
        const TypeRegistry* Types = nullptr;
        /// @brief Reflected module system catalog; non-null only on a `--module` cook.
        ///
        /// The level importer resolves a level's system ids against it; other importers ignore it.
        const SystemRegistry* Systems = nullptr;
        /// @brief The active build configuration driving this cook, or null for a zero-config cook.
        ///
        /// The texture importer resolves a texture's compression role to a concrete format
        /// through this configuration's role table. Null means no project settings: the
        /// importer falls back to its hardcoded ASTC default, the zero-config behavior.
        const BuildConfiguration* Config = nullptr;
        /// @brief Engine core shader directory added to every Slang session's search path.
        ///
        /// Lets a consumer `.slang` resolve `#include "Veng/surface.slang"` against the engine
        /// core shader dir. Added after the source file's own directory, so a local file shadows
        /// a same-named engine file. Empty for a cook that ships no engine shader header (the core
        /// pack itself, whose shaders reach the engine header through their own source dir).
        path ShaderIncludeDir;
        /// @brief Records a source file the cook read, for build dependency tracking.
        ///
        /// The cooker records the pack JSON and resolved cross-asset references centrally.
        /// Importers call this for binary payloads not named in the manifest — a texture's image
        /// file, a mesh model, a shader's .slang source and its includes. Always set by the cooker.
        function<void(const path&)> RecordDependency;
        /// @brief Threads this Cook call may spawn for parallelism of its own. Never below 1.
        ///
        /// The cook has **one** concurrency budget, shared by the driver's per-asset workers and
        /// any threading inside an importer, so an importer that parallelizes internally does not
        /// stack a second pool on top of the driver's. An importer that spawns no threads ignores
        /// this; one that does must not exceed it.
        u32 ThreadBudget = 1;
    };

    /// @brief Whether an importer's Cook may run concurrently with other importers' Cook calls.
    ///
    /// The cook driver runs a pack's assets across a worker pool. An importer declares which
    /// scheduling it tolerates; @ref ImporterConcurrency::Serialized is the default because a cook
    /// that is fast and occasionally wrong is worse than one that is slow.
    enum class ImporterConcurrency : u8
    {
        /// @brief Runs under the cook's serialization lock: never concurrent with another importer.
        ///
        /// The safe default. An importer driving a library with process-wide state, or one whose
        /// reentrancy has not been established, stays here at the cost of the cook's parallelism.
        Serialized,
        /// @brief Reentrant: Cook runs on any worker thread, concurrently with any other importer.
        ///
        /// Declaring this asserts that Cook touches no mutable state outside its own frame and
        /// drives no library carrying process-wide state that another concurrent call could reach.
        Parallel,
    };

    /// @brief Whether a loaded module image can change what an importer emits.
    ///
    /// The cook loads module images — a runtime module whose reflected types and systems some
    /// importers validate against, and a cook module supplying importers of its own — and their
    /// identity is a genuine input to whatever consults them. It is an input to nothing else: a
    /// shader, a texture, a mesh or a material is decided entirely by its own sources and the
    /// cooker's own code, and keying it on an image it never reads throws its cached result away
    /// every time that image is rebuilt.
    enum class ImporterModuleDependence : u8
    {
        /// @brief Emits from its own sources alone; no module image can change the result.
        Independent,
        /// @brief Consults a module image — its reflected registries, or code living inside it.
        DependsOnModule,
    };

    /// @brief Offline, Vulkan-free importer interface: one implementation per AssetTypeId.
    ///
    /// Registered into a Cooker and dispatched per pack entry. Cook() turns a pack entry's
    /// JSON (plus any files it references on disk) into cooked blob bytes for ArchiveWriter::Add.
    class AssetImporter
    {
    public:
        /// @brief Virtual destructor.
        virtual ~AssetImporter() = default;

        /// @brief Returns the AssetTypeId this importer handles.
        [[nodiscard]] virtual AssetTypeId Type() const = 0;

        /// @brief Declares how the cook driver may schedule this importer's Cook.
        ///
        /// Defaults to @ref ImporterConcurrency::Serialized, so an importer that says nothing is
        /// never run concurrently. Override with @ref ImporterConcurrency::Parallel only once
        /// Cook — and every library it drives — is known reentrant.
        /// @return This importer's concurrency class.
        [[nodiscard]] virtual ImporterConcurrency Concurrency() const
        {
            return ImporterConcurrency::Serialized;
        }

        /// @brief Declares whether a loaded module image can change what this importer emits.
        ///
        /// Defaults to @ref ImporterModuleDependence::Independent, which is the truth for an
        /// importer that reads only its own sources. Override with
        /// @ref ImporterModuleDependence::DependsOnModule when Cook reads CookContext::Types or
        /// CookContext::Systems, so an entry it produced is invalidated by the module rebuild that
        /// can change it.
        ///
        /// A host never asks this of an importer a cook module supplied: that importer's code lives
        /// in the module image, so its output depends on the image whatever it declares here.
        /// @return This importer's module dependence.
        [[nodiscard]] virtual ImporterModuleDependence ModuleDependence() const
        {
            return ImporterModuleDependence::Independent;
        }

        /// @brief Cooks one pack entry's JSON into cooked blob bytes.
        /// @param context  Cook context providing the pack directory, asset resolver, and dependency recorder.
        /// @param entry    The pack entry JSON object for this asset.
        /// @return Cooked blob bytes, or an error string.
        [[nodiscard]] virtual Result<vector<u8>> Cook(const CookContext& context,
                                                      const json& entry) const = 0;
    };
}
