#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/Archive.h>
#include <Veng/Asset/AssetError.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetLoader.h>
#include <Veng/Asset/AssetType.h>

namespace Veng::Renderer
{
    class Context;
}

namespace Veng
{
    class TaskSystem;
    class TypeRegistry;

    /// @brief Construction parameters for AssetManager.
    struct AssetManagerInfo
    {
    };

    /// @brief RAII token for an in-memory archive mounted via MountMemory.
    ///
    /// Holding the handle keeps the archive mounted; dropping it unmounts and
    /// frees the bytes. Moveable, non-copyable; a default-constructed or moved-from
    /// handle owns nothing.
    class VE_API MountHandle
    {
    public:
        /// @brief Constructs an empty (owning-nothing) handle.
        MountHandle() = default;
        ~MountHandle();

        MountHandle(const MountHandle&) = delete;
        MountHandle& operator=(const MountHandle&) = delete;

        MountHandle(MountHandle&& other) noexcept;
        MountHandle& operator=(MountHandle&& other) noexcept;

        /// @brief Returns true when this handle owns a mounted archive.
        [[nodiscard]] bool IsValid() const { return m_Manager != nullptr; }

    private:
        friend class AssetManager;
        MountHandle(AssetManager& manager, u64 token) : m_Manager(&manager), m_Token(token) {}

        void Release();

        AssetManager* m_Manager = nullptr;
        u64 m_Token = 0;
    };

    /// @brief Mounts cooked .vengpack archives, resolves AssetIds, and loads assets via per-type AssetLoaders.
    ///
    /// Load<T> is asynchronous: it returns a not-yet-resident handle immediately and fills it in
    /// on the main-thread continuation (decode + GPU upload run on the task system).
    /// LoadSync<T> is the blocking sibling — it runs the whole pipeline inline and returns a
    /// resident handle or a structured AssetLoadError.
    class VE_API AssetManager
    {
    public:
        friend class MountHandle;

        /// @brief Constructs an AssetManager bound to the given context, task system, and type registry.
        AssetManager(Renderer::Context& context, TaskSystem& tasks, TypeRegistry& types,
                     const AssetManagerInfo& info = {});
        ~AssetManager();

        /// @brief Opens an on-disk .vengpack archive and indexes its table of contents.
        ///
        /// Mounting the same path twice is a no-op. When the same AssetId appears in more
        /// than one mounted archive, the archive mounted first wins.
        VoidResult Mount(const path& archive);

        /// @brief Unmounts the archive at the given path.
        void Unmount(const path& archive);

        /// @brief Mounts an in-memory archive, copying the bytes into the reader's own storage.
        ///
        /// Deduplicates by identity (a synthetic path string such as "<core>"); mounting the
        /// same identity twice is a no-op.
        VoidResult MountBytes(const path& identity, std::span<const u8> bytes);

        /// @brief Mounts an in-memory archive that shadows all on-disk mounts.
        ///
        /// Load<T>(id) resolves against memory mounts before any path-mounted archive, so a
        /// freshly cooked blob overrides the on-disk version of the same AssetId. The bytes
        /// are moved into the manager. The returned MountHandle unmounts and frees the archive
        /// on destruction. Mounting failures assert — the bytes come from an in-process cook,
        /// not untrusted input.
        [[nodiscard]] MountHandle MountMemory(vector<u8> archiveBytes, string debugName);

        /// @brief Asynchronous load — returns immediately with a handle that may not yet be resident.
        ///
        /// Decode + GPU upload run on the task system; bindless registration and the cache swap
        /// run on the main thread during PumpFinalizes(). A cache hit (resident or pending) returns
        /// the existing handle. A resolution failure (NotFound/WrongType) returns an empty handle;
        /// a later decode failure leaves the entry permanently pending and logs.
        template <typename T>
        AssetHandle<T> Load(AssetId id)
        {
            const Ref<Detail::AssetCacheEntry> entry = LoadUntyped(AssetTypeTrait<T>::Type, id);
            if (!entry)
                return AssetHandle<T>();

            return AssetHandle<T>(id, entry);
        }

        /// @brief Blocking load — resolves, loads, uploads, and finalizes inline, then returns a resident handle.
        ///
        /// Dependency loads are synchronous and eager. Returns a structured AssetLoadError on failure
        /// (branch on AssetError::Kind). Bypasses the async continuation queue entirely — no deadlock risk.
        template <typename T>
        AssetResult<AssetHandle<T>> LoadSync(AssetId id)
        {
            const AssetResult<Ref<Detail::AssetCacheEntry>> entry = LoadSyncUntyped(AssetTypeTrait<T>::Type, id);
            if (!entry)
                return std::unexpected(entry.error());

            return AssetHandle<T>(id, *entry);
        }

        /// @brief Returns the cache entry backing a handle.
        ///
        /// Loaders use this to record a material's texture/shader sub-loads as dependencies of
        /// its async finalize. Returns null for an empty handle.
        template <typename T>
        [[nodiscard]] static Ref<Detail::AssetCacheEntry> EntryOf(const AssetHandle<T>& handle)
        {
            return handle.m_Entry;
        }

        /// @brief Wraps an already-resident, runtime-created resource in an AssetHandle<T>.
        ///
        /// The handle carries the invalid AssetId (Id().IsValid() == false): a runtime resource
        /// has no content identity, so a reflective serializer records it as "no asset". The
        /// backing cache entry is detached — never inserted into the AssetId map, so CollectGarbage()
        /// leaves it alone. Each call yields a distinct entry; adopting does not deduplicate.
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

        /// @brief Returns the cache entry for an id, or null if it is not cached.
        ///
        /// Untyped — the prefab loader uses it to rehydrate an embedded handle without naming
        /// the asset's concrete type. Never touches mounted archives or loaders.
        [[nodiscard]] Ref<Detail::AssetCacheEntry> CachedEntry(AssetId id) const
        {
            const auto it = m_Cache.find(id);
            return it == m_Cache.end() ? nullptr : it->second;
        }

        /// @brief Cache-only typed lookup — never touches mounted archives or loaders.
        template <typename T>
        [[nodiscard]] optional<AssetHandle<T>> Get(AssetId id) const
        {
            const auto it = m_Cache.find(id);
            if (it == m_Cache.end() || it->second->Type != AssetTypeTrait<T>::Type)
                return std::nullopt;

            return AssetHandle<T>(id, it->second);
        }

        /// @brief Returns the type registry the prefab loader and editor reflect components through.
        [[nodiscard]] TypeRegistry& GetTypeRegistry() const { return m_Types; }

        /// @brief Runs any pending async finalizes whose uploads completed and whose dependencies are resident.
        ///
        /// Called from the frame loop after the task system's continuation pump, on the main thread.
        void PumpFinalizes();

        /// @brief Drops cache entries with no AssetHandle<T> referencing them.
        ///
        /// Their engine resources retire through the per-frame deferred-destruction path — safe to
        /// call mid-frame. A still-pending (not-yet-resident) entry is never evicted.
        void CollectGarbage();

    private:
        /// @brief One on-disk .vengpack archive and its indexed reader.
        struct MountedArchive
        {
            path Path;
            ArchiveReader Reader;
        };

        /// @brief An in-memory archive mounted via MountMemory, searched before on-disk mounts.
        ///
        /// Identified by a monotonic token that the owning MountHandle drops on destruction.
        struct MemoryMount
        {
            u64 Token;
            string DebugName;
            ArchiveReader Reader;
        };

        /// @brief Drops the memory mount with the given token.
        ///
        /// Invoked by MountHandle on destruction; a token with no live mount is a no-op.
        void UnmountMemory(u64 token);

        /// @brief A submitted-but-not-yet-finalized async load.
        ///
        /// The cache entry exists with a null Resource (pending); Finalize swaps the resource in
        /// once every Dependency is resident and finalized. The render graph folds the
        /// transfer-timeline wait into the first frame that samples the resource, so registration
        /// is safe before the GPU copy lands.
        struct PendingLoad
        {
            /// @brief The asset being loaded.
            AssetId Id;
            /// @brief The cache slot (Resource is null until finalized).
            Ref<Detail::AssetCacheEntry> Entry;
            /// @brief The created-but-unregistered resource.
            Detail::RefAny Resource;
            /// @brief Kept alive until Finalize runs.
            vector<Ref<Detail::AssetCacheEntry>> Dependencies;
            /// @brief Main-thread registration step; null if not needed.
            function<VoidResult()> Finalize;
        };

        [[nodiscard]] Ref<Detail::AssetCacheEntry> LoadUntyped(AssetType type, AssetId id);
        [[nodiscard]] AssetResult<Ref<Detail::AssetCacheEntry>> LoadSyncUntyped(AssetType type, AssetId id);

        /// @brief Resolves an id to a loader and cooked blob, validating type against the archive entry.
        ///
        /// Shared by the async and sync load paths.
        [[nodiscard]] AssetResult<std::pair<AssetLoader*, ArchiveEntry>> Resolve(AssetType type, AssetId id);

        [[nodiscard]] optional<ArchiveEntry> Find(AssetId id) const;

        void RegisterLoader(Unique<AssetLoader> loader);

        Renderer::Context& m_Context;
        TaskSystem& m_Tasks;
        /// @brief Borrowed; the prefab loader reflects component fields through it.
        TypeRegistry& m_Types;

        vector<MountedArchive> m_Mounts;
        vector<MemoryMount> m_MemoryMounts;
        u64 m_NextMemoryToken = 1;
        unordered_map<AssetType, Unique<AssetLoader>> m_Loaders;
        unordered_map<AssetId, Ref<Detail::AssetCacheEntry>> m_Cache;
        vector<PendingLoad> m_Pending;
    };
}
