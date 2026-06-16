#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/Archive.h>
#include <Veng/Asset/AssetError.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetLoader.h>
#include <Veng/Asset/AssetType.h>

// AssetManager: mounts cooked .vengpack archives, resolves AssetIds against
// them, and loads assets via a per-type AssetLoader (see AssetLoader.h), handing
// back a typed, refcounted AssetHandle<T>.
//
// Load is asynchronous: it returns a not-yet-resident handle immediately and
// fills it in on the main-thread continuation (decode + GPU upload run on the
// task system). LoadSync is the blocking sibling — it runs the whole pipeline
// inline and returns a resident handle (or a structured AssetLoadError).

namespace Veng::Renderer
{
    class Context;
}

namespace Veng
{
    class TaskSystem;
    class TypeRegistry;

    struct AssetManagerInfo
    {
    };

    class VE_API AssetManager
    {
    public:
        AssetManager(Renderer::Context& context, TaskSystem& tasks, TypeRegistry& types,
                     const AssetManagerInfo& info = {});
        ~AssetManager();

        // Opens archive and indexes its TOC. Mounting the same path twice is a
        // no-op. If the same AssetId appears in more than one mounted archive,
        // the archive mounted first wins.
        VoidResult Mount(const path& archive);
        void Unmount(const path& archive);

        // Mounts an in-memory archive (e.g. an embedded core pack). The bytes
        // are copied into the reader's own storage. Deduplicates by identity
        // (a synthetic path string such as "<core>"); mounting the same identity
        // twice is a no-op.
        VoidResult MountBytes(const path& identity, std::span<const u8> bytes);

        // Asynchronous load: returns immediately with a handle that is not yet
        // resident (IsLoaded() == false). The decode + GPU upload run on the task
        // system; registration into the bindless registry and the cache swap run
        // on the main thread during PumpFinalizes() (called from the frame loop).
        // A cache hit (resident or pending) returns the existing handle. A
        // resolution failure (NotFound/WrongType) returns an empty handle; a
        // later load/decode failure leaves the entry permanently pending and logs.
        template <typename T>
        AssetHandle<T> Load(AssetId id)
        {
            const Ref<Detail::AssetCacheEntry> entry = LoadUntyped(AssetTypeTrait<T>::Type, id);
            if (!entry)
                return AssetHandle<T>();

            return AssetHandle<T>(id, entry);
        }

        // Blocking load: resolves, loads, uploads, and finalizes inline, then
        // returns a resident handle. Dependency loads are synchronous and eager.
        // Returns a structured AssetLoadError on failure (branch on
        // AssetError::Kind). Does not deadlock against the async continuation
        // queue — it bypasses it entirely.
        template <typename T>
        AssetResult<AssetHandle<T>> LoadSync(AssetId id)
        {
            const AssetResult<Ref<Detail::AssetCacheEntry>> entry = LoadSyncUntyped(AssetTypeTrait<T>::Type, id);
            if (!entry)
                return std::unexpected(entry.error());

            return AssetHandle<T>(id, *entry);
        }

        // The cache entry backing a handle — loaders use it to record a
        // material's texture/shader sub-loads as dependencies of its async
        // finalize. Returns null for an empty handle.
        template <typename T>
        [[nodiscard]] static Ref<Detail::AssetCacheEntry> EntryOf(const AssetHandle<T>& handle)
        {
            return handle.m_Entry;
        }

        // Wraps an already-resident, runtime-created resource (e.g. a Mesh built
        // from Primitives) in an AssetHandle<T> so it is usable everywhere a
        // cooked, AssetId-loaded handle is. The handle carries the invalid
        // AssetId (Id().IsValid() == false): a runtime resource has no content
        // identity, so a reflective serializer records it as "no asset". The
        // backing cache entry is detached — never inserted into the AssetId map,
        // so CollectGarbage() leaves it alone — and stays alive for exactly as
        // long as a handle references it; the last drop retires the resource
        // through the per-frame deferred-destruction path like any other.
        // Adopting does not deduplicate: each call yields a distinct entry.
        template <typename T>
        [[nodiscard]] AssetHandle<T> Adopt(Ref<T> resource)
        {
            VE_ASSERT(resource != nullptr, "AssetManager::Adopt: resource is null");

            auto entry = CreateRef<Detail::AssetCacheEntry>(Detail::AssetCacheEntry{
                .Id = AssetId{},
                .Type = AssetTypeTrait<T>::Type,
                .Resource = std::static_pointer_cast<void>(std::move(resource)),
            });

            return AssetHandle<T>(AssetId{}, std::move(entry));
        }

        // The cache entry for an id, or null if it is not cached. Untyped — the
        // prefab loader's spawn uses it to rehydrate an embedded handle whose
        // dependency was already loaded (and whose entry the prefab keeps
        // resident), without naming the asset's concrete type. Never touches
        // mounted archives or loaders.
        [[nodiscard]] Ref<Detail::AssetCacheEntry> CachedEntry(AssetId id) const
        {
            const auto it = m_Cache.find(id);
            return it == m_Cache.end() ? nullptr : it->second;
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

        // Run any pending async finalizes whose upload completed and whose
        // dependencies are resident. Called from the frame loop after the task
        // system's continuation pump, on the main thread.
        void PumpFinalizes();

        // Drops cache entries with no AssetHandle<T> referencing them. Their
        // engine resources retire through the existing per-frame
        // deferred-destruction path (Context::Retire) — safe to call mid-frame.
        // A still-pending (not-yet-resident) entry is never evicted.
        void CollectGarbage();

    private:
        struct MountedArchive
        {
            path Path;
            ArchiveReader Reader;
        };

        // A submitted-but-not-yet-finalized async load. The cache entry exists
        // (pending: null Resource); Finalize swaps the resource in once every
        // Dependency is resident + finalized. The async upload itself need not
        // have completed: the render graph folds the transfer-timeline wait into
        // the first frame that samples the resource, so registration is safe
        // before the GPU copy lands.
        struct PendingLoad
        {
            AssetId Id;
            Ref<Detail::AssetCacheEntry> Entry;
            Detail::RefAny Resource;
            vector<Ref<Detail::AssetCacheEntry>> Dependencies;
            function<VoidResult()> Finalize;
        };

        [[nodiscard]] Ref<Detail::AssetCacheEntry> LoadUntyped(AssetType type, AssetId id);
        [[nodiscard]] AssetResult<Ref<Detail::AssetCacheEntry>> LoadSyncUntyped(AssetType type, AssetId id);

        // Resolves id to a loader and cooked blob, validating type. Shared by the
        // async and sync paths.
        [[nodiscard]] AssetResult<std::pair<AssetLoader*, ArchiveEntry>> Resolve(AssetType type, AssetId id);

        [[nodiscard]] optional<ArchiveEntry> Find(AssetId id) const;

        void RegisterLoader(Unique<AssetLoader> loader);

        Renderer::Context& m_Context;
        TaskSystem& m_Tasks;
        // Borrowed: the prefab loader reflects a component's fields through it.
        // Same explicit-threading discipline as the context and task system.
        TypeRegistry& m_Types;

        vector<MountedArchive> m_Mounts;
        unordered_map<AssetType, Unique<AssetLoader>> m_Loaders;
        unordered_map<AssetId, Ref<Detail::AssetCacheEntry>> m_Cache;
        vector<PendingLoad> m_Pending;
    };
}
