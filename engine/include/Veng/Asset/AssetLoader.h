#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetError.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetType.h>

#include <span>

namespace Veng::Renderer
{
    class Context;
}

namespace Veng
{
    class AssetManager;
    class TaskSystem;
    class TypeRegistry;

    namespace Detail
    {
        /// @brief Two-phase load result returned by AssetLoader::Load.
        ///
        /// The worker phase (create + record upload + resolve dependencies) has run;
        /// the main-thread finalize phase (bindless registration, index patching,
        /// pipeline build) is deferred into Finalize so the async path can run it
        /// on the render-thread continuation.
        struct LoadJob
        {
            /// @brief The created-but-unregistered resource (type-erased; AssetHandle<T> downcasts it).
            ///
            /// Swapped into the cache entry once Finalize completes.
            RefAny Resource;

            /// @brief Dependency cache entries this asset's Finalize requires resident and finalized first.
            ///
            /// Empty for leaf assets (Raw/Mesh/Shader/VertexLayout). Keeps dependencies alive until Finalize runs.
            vector<Ref<AssetCacheEntry>> Dependencies;

            /// @brief Main-thread finalize callback: register into the bindless registry, patch indices, build pipelines.
            ///
            /// Null when the asset needs no finalize step. Runs once every Dependencies entry is resident.
            /// Returns VoidResult so a deferred failure surfaces as an AssetLoadError on the sync path.
            function<VoidResult()> Finalize;
        };
    }

    /// @brief Per-type cooked-blob loader, registered into AssetManager and dispatched from Load/LoadSync.
    class AssetLoader
    {
    public:
        virtual ~AssetLoader() = default;

        /// @brief Returns the AssetType this loader handles.
        [[nodiscard]] virtual AssetType Type() const = 0;

        /// @brief Decodes a cooked blob into a LoadJob containing the unregistered resource and its finalize step.
        ///
        /// When async is true the loader records GPU uploads through the task system (no device wait) and
        /// resolves dependencies via manager.Load; when false it uses the blocking UploadSync path and
        /// manager.LoadSync. A MissingDependency surfaces as an AssetLoadError, not a crash.
        ///
        /// @param manager  The owning AssetManager (dependency resolution).
        /// @param context  The render context (GPU resource creation).
        /// @param tasks    The task system (async upload recording).
        /// @param types    The engine TypeRegistry; used by the prefab loader to surface embedded AssetHandle dependencies.
        /// @param id       The asset being loaded.
        /// @param cooked   The cooked blob bytes from the archive.
        /// @param async    True to record uploads asynchronously; false for the blocking sync path.
        [[nodiscard]] virtual AssetResult<Detail::LoadJob>
        Load(AssetManager& manager, Renderer::Context& context, TaskSystem& tasks,
             TypeRegistry& types, AssetId id, std::span<const u8> cooked, bool async) const = 0;
    };
}
