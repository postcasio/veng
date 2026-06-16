#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetError.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetType.h>

#include <span>

// The engine-side loader table: one AssetLoader per
// AssetType, registered into AssetManager and dispatched from Load/LoadSync.
// This is the only place a type touches Context — mirrors how the cooker keeps
// its GPU-free Cook() separate (Veng::Cook::AssetImporter).

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
        // The two-phase result a loader hands back. The "worker phase"
        // (create + record upload + resolve dependencies) has run; the
        // main-thread "finalize phase" (registration into the bindless registry,
        // index patching, pipeline build) is deferred into Finalize so the async
        // path can run it on the render-thread continuation.
        struct LoadJob
        {
            // The created-but-unregistered resource (type-erased; AssetHandle<T>
            // downcasts it). Swapped into the cache entry once finalized.
            RefAny Resource;

            // Dependency cache entries this asset's Finalize requires resident
            // and finalized first (a material's textures + shaders). Empty for a
            // leaf asset. Keeps the dependencies alive until Finalize runs.
            vector<Ref<AssetCacheEntry>> Dependencies;

            // Main-thread finalize: register into the bindless registry, patch
            // resolved indices, build the GPU pipeline, etc. Null if the asset
            // needs no finalize (Raw/Mesh/Shader/VertexLayout). Runs once every
            // Dependencies entry is resident (finalized). Returns a VoidResult so
            // a deferred failure (e.g. a corrupt material the pipeline build
            // rejects) surfaces as an AssetLoadError on the sync path.
            function<VoidResult()> Finalize;
        };
    }

    class AssetLoader
    {
    public:
        virtual ~AssetLoader() = default;

        [[nodiscard]] virtual AssetType Type() const = 0;

        // Cooked blob (assetformat layout) -> a LoadJob: the created, unregistered
        // engine resource plus its main-thread Finalize and dependency set. When
        // async is true the loader records GPU uploads through the task system
        // (no device wait) and resolves dependencies via manager.Load; when
        // false it uploads through the blocking UploadSync path and resolves
        // dependencies via manager.LoadSync. A MissingDependency surfaces as an
        // AssetLoadError, not a crash.
        //
        // `types` is the engine-owned TypeRegistry threaded through the
        // AssetManager; the prefab loader reflects a component's fields against it
        // to surface embedded AssetHandle ids as dependencies. Every other loader
        // ignores it.
        [[nodiscard]] virtual AssetResult<Detail::LoadJob> Load(
            AssetManager& manager, Renderer::Context& context, TaskSystem& tasks,
            TypeRegistry& types, AssetId id, std::span<const u8> cooked, bool async) const = 0;
    };
}
