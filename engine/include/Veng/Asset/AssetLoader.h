#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetError.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Task/TaskSystem.h>

#include <span>

namespace Veng::Renderer
{
    class Context;
}

namespace Veng
{
    class AssetManager;
    class AssetTypeRegistry;
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

            /// @brief A worker Task producing the resource, for a loader whose decode is CPU-heavy.
            ///
            /// Set only on a loader's async path. When present, Resource and Finalize are unused: the
            /// resource is produced off the render thread and the entry becomes resident through the
            /// main-thread continuation pump when the Task lands, keeping the asset's own AssetId
            /// (where Adopt would detach the entry). A null RefAny result means the decode failed and
            /// the entry stalls unresident, the same shape a corrupt blob takes on the loader path. A
            /// loader's sync path leaves this empty and returns a ready Resource.
            optional<Task<RefAny>> AsyncResource;
        };

        /// @brief One dependency a parse names, which the manager loads before completing the asset.
        struct AssetDependency
        {
            /// @brief The dependency's asset type.
            AssetTypeId Type;
            /// @brief The dependency's id.
            AssetId Id;
        };

        /// @brief The worker half of a two-phase load: what the blob said, and how to finish it.
        ///
        /// AssetLoader::Parse produces it; the manager then loads each of Dependencies on the main
        /// thread — a cache lookup per dependency, each one's own parse in turn going to a worker —
        /// and runs Complete there with their cache entries.
        struct ParsedAsset
        {
            /// @brief The assets this one needs, in the order Complete receives their entries.
            vector<AssetDependency> Dependencies;

            /// @brief Builds the load job on the main thread from the parse and its dependencies.
            ///
            /// Receives one non-null cache entry per Dependencies element, in order: a load whose
            /// dependency does not resolve fails before Complete runs. On an asynchronous load the
            /// entries may still be pending, so the job names the ones its Finalize needs resident
            /// in LoadJob::Dependencies; on LoadSync they are resident already. Runs where
            /// main-thread-only work is legal — the manager's cache, the bindless registry, the
            /// pipeline cache — and may run after the archive the blob came from is unmounted, so it
            /// captures what it needs from the blob by value, never a span into it. Must be set; it
            /// never yields a LoadJob::AsyncResource.
            function<AssetResult<LoadJob>(AssetManager&, std::span<const Ref<AssetCacheEntry>>)>
                Complete;
        };

        /// @brief Wraps a job a parse finished outright — no dependencies, nothing left for the
        ///        main thread — into a ParsedAsset.
        /// @param job  The finished job, or the parse's error.
        /// @return The ParsedAsset completing to @p job, or the error.
        [[nodiscard]] inline AssetResult<ParsedAsset> ParsedJob(AssetResult<LoadJob> job)
        {
            if (!job)
            {
                return std::unexpected(std::move(job.error()));
            }
            return ParsedAsset{
                .Complete = [job = std::move(*job)](
                                AssetManager&,
                                std::span<const Ref<AssetCacheEntry>>) -> AssetResult<LoadJob>
                { return job; },
            };
        }
    }

    /// @brief What a loader's Parse may read and use, which is what is legal off the main thread.
    ///
    /// Deliberately not the AssetManager: its cache, its mounts and its loaders are main-thread state,
    /// and a parse reaches the assets it depends on by naming them (ParsedAsset::Dependencies).
    struct AssetParseContext
    {
        /// @brief The render context: capability queries, and the resource creation that is legal
        ///        on a worker (buffers, images, views). Bindless registration and pipeline builds are
        ///        not, and belong in ParsedAsset::Complete or LoadJob::Finalize.
        Renderer::Context& Context;
        /// @brief The task system, for scheduling a worker upload.
        TaskSystem& Tasks;
        /// @brief The engine type registry, read-only.
        const TypeRegistry& Types;
        /// @brief The asset types the manager knows, read-only.
        const AssetTypeRegistry& AssetTypes;
        /// @brief The texture-quality mip skip in force when the load was issued.
        u32 TextureQualityMipSkip = 0;
        /// @brief True on an asynchronous load, where Parse runs on a worker; false on LoadSync,
        ///        where it runs inline on the calling thread and may upload blocking.
        bool Async = true;
    };

    /// @brief Per-type cooked-blob loader, registered into AssetManager and dispatched from Load/LoadSync.
    ///
    /// A loader implements one of two protocols. A **two-phase** loader (ParsesOffThread true)
    /// implements Parse: the manager runs it on a worker for an asynchronous Load, so the blob's
    /// decode never costs the thread that asked, and inline for LoadSync, and finishes the load on
    /// the main thread through ParsedAsset::Complete. Every builtin loader is two-phase. A
    /// **single-phase** loader implements Load, which the manager runs on the calling thread for
    /// either kind of load — the protocol for a loader whose decode cannot be split from main-thread
    /// work.
    class AssetLoader
    {
    public:
        virtual ~AssetLoader() = default;

        /// @brief Returns the AssetTypeId this loader handles.
        [[nodiscard]] virtual AssetTypeId Type() const = 0;

        /// @brief Whether this loader is two-phase: the manager calls Parse, never Load.
        [[nodiscard]] virtual bool ParsesOffThread() const { return false; }

        /// @brief Decodes a cooked blob into the dependencies it names and the step that finishes it.
        ///
        /// Runs on a worker for an asynchronous load and inline for LoadSync (@p context.Async
        /// says which), so it touches nothing main-thread-only: not the AssetManager (it names its
        /// dependencies in ParsedAsset::Dependencies instead of loading them), not the bindless
        /// registry, not the pipeline cache. CPU decode, validation and worker-legal resource
        /// creation belong here; the rest belongs in ParsedAsset::Complete or LoadJob::Finalize. A
        /// malformed blob is an AssetLoadError, not a crash. Only called when ParsesOffThread is
        /// true; the default reports the loader as single-phase.
        /// @param context  What the parse may use.
        /// @param id       The asset being loaded.
        /// @param cooked   The cooked blob bytes; valid only for the duration of the call.
        /// @return The parsed asset, or the decode error.
        [[nodiscard]] virtual AssetResult<Detail::ParsedAsset>
        Parse(const AssetParseContext& context, AssetId id, std::span<const u8> cooked) const;

        /// @brief Decodes a cooked blob into a LoadJob containing the unregistered resource and its finalize step.
        ///
        /// The single-phase protocol, run on the calling thread; a two-phase loader leaves it
        /// unimplemented. When async is true the loader records GPU uploads through the task system
        /// (no device wait) and resolves dependencies via manager.Load; when false it uses the
        /// blocking UploadSync path and manager.LoadSync. A MissingDependency surfaces as an
        /// AssetLoadError, not a crash. The default reports the loader as two-phase.
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
             TypeRegistry& types, AssetId id, std::span<const u8> cooked, bool async) const;
    };
}
