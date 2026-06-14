#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/Archive.h>
#include <Veng/Asset/AssetError.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetLoader.h>
#include <Veng/Asset/AssetType.h>

// AssetManager (planset-5 plan 04): mounts cooked .vengpack archives, resolves
// AssetIds against them, and synchronously loads assets via a per-type
// AssetLoader (see AssetLoader.h), handing back a typed, refcounted
// AssetHandle<T>.
//
// Synchronous-only, by decision (see plans/planset-5/README.md): LoadSync
// blocks. Async Load() is future work (the threading planset); the verbose
// name is deliberate so it keeps it when that lands.

namespace Veng::Renderer
{
    class Context;
}

namespace Veng
{
    struct AssetManagerInfo
    {
    };

    class VE_API AssetManager
    {
    public:
        explicit AssetManager(Renderer::Context& context, const AssetManagerInfo& info = {});
        ~AssetManager();

        // Opens archive and indexes its TOC. Mounting the same path twice is a
        // no-op. If the same AssetId appears in more than one mounted archive,
        // the archive mounted first wins.
        VoidResult Mount(const path& archive);
        void Unmount(const path& archive);

        // Resolves id across mounted archives, loads (and caches) it via the
        // registered AssetLoader for its AssetType, and returns a typed handle.
        // A cache hit returns immediately without touching the archive or
        // loader again. Dependency loads (e.g. a material's textures) are
        // synchronous and eager.
        template <typename T>
        AssetResult<AssetHandle<T>> LoadSync(AssetId id)
        {
            const AssetResult<Ref<Detail::AssetCacheEntry>> entry = LoadSyncUntyped(AssetTypeTrait<T>::Type, id);
            if (!entry)
                return std::unexpected(entry.error());

            return AssetHandle<T>(id, *entry);
        }

        // Cached lookup only — never touches mounted archives or loaders.
        template <typename T>
        [[nodiscard]] optional<AssetHandle<T>> Get(AssetId id) const
        {
            const auto it = m_Cache.find(id);
            if (it == m_Cache.end() || it->second->Type != AssetTypeTrait<T>::Type)
                return std::nullopt;

            return AssetHandle<T>(id, it->second);
        }

        // Drops cache entries with no AssetHandle<T> referencing them. Their
        // engine resources retire through the existing per-frame
        // deferred-destruction path (Context::Retire) — safe to call mid-frame.
        void CollectGarbage();

    private:
        struct MountedArchive
        {
            path Path;
            ArchiveReader Reader;
        };

        [[nodiscard]] AssetResult<Ref<Detail::AssetCacheEntry>> LoadSyncUntyped(AssetType type, AssetId id);
        [[nodiscard]] optional<ArchiveEntry> Find(AssetId id) const;

        void RegisterLoader(Unique<AssetLoader> loader);

        Renderer::Context& m_Context;

        vector<MountedArchive> m_Mounts;
        unordered_map<AssetType, Unique<AssetLoader>> m_Loaders;
        unordered_map<AssetId, Ref<Detail::AssetCacheEntry>> m_Cache;
    };
}
