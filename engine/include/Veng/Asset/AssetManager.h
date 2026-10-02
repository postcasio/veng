#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/Archive.h>
#include <Veng/Asset/AssetBuild.h>
#include <Veng/Asset/AssetError.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetLoader.h>
#include <Veng/Asset/AssetLoaderRegistry.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Task/TaskSystem.h>

namespace Veng::Renderer
{
    class Context;
}

namespace Veng::Text
{
    class GlyphSource;
    class GlyphAtlas;
}

namespace Veng
{
    class TaskSystem;
    class TypeRegistry;
    struct MeshSocket;
    struct Skeleton;

    /// @brief Construction parameters for AssetManager.
    struct AssetManagerInfo
    {
        /// @brief Host-owned asset-type identities merged on top of the manager's own builtins.
        ///
        /// Null (the default) means no module-defined asset types; the manager still knows every
        /// builtin, because it fills its own registry before merging this one. Registrations
        /// already present under the same id are skipped, so pointing this at a registry the host
        /// pre-filled with the builtins is the normal case rather than a collision. Type
        /// *dispatch* keys on the id value and never consults a registry.
        const AssetTypeRegistry* AssetTypes = nullptr;
        /// @brief Host-owned loader factories a module registered, instantiated at construction.
        ///
        /// Null (the default) means no module-defined asset types. Each factory produces one
        /// loader for a type the engine does not itself handle; claiming a builtin type is fatal.
        /// @warning The factories and the loaders they produce live in the module image, so the
        ///          module handle must outlive this manager.
        const AssetLoaderRegistry* Loaders = nullptr;
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

        /// @brief Move-constructs, transferring ownership of the mounted archive.
        MountHandle(MountHandle&& other) noexcept;
        /// @brief Move-assigns, unmounting any archive this handle currently owns first.
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
    /// Load<T> is asynchronous: it returns a not-yet-resident handle immediately, and the blob's
    /// inflate and decode run on the task system (AssetLoader::Parse); PumpFinalizes then lands the
    /// parse on the main thread, loads the dependencies it names, and finalizes the asset once they
    /// are resident. LoadSync<T> is the blocking sibling — it runs the whole pipeline inline and
    /// returns a resident handle or a structured AssetLoadError.
    class VE_API AssetManager
    {
    public:
        friend class MountHandle;

        /// @brief Constructs an AssetManager bound to the given context, task system, and type registry.
        AssetManager(Renderer::Context& context, TaskSystem& tasks, TypeRegistry& types,
                     const AssetManagerInfo& info = {});
        ~AssetManager();

        // Owns unique loaders/cache and holds references to its context/tasks/registry:
        // never copied or moved. Declared explicitly because MSVC's STL eagerly
        // instantiates the (deleted-in-effect) copy of the move-only m_Loaders map
        // otherwise; libc++/libstdc++ only do so on use, so this never surfaced on the
        // arm64 dev platform.
        AssetManager(const AssetManager&) = delete;
        AssetManager& operator=(const AssetManager&) = delete;
        AssetManager(AssetManager&&) = delete;
        AssetManager& operator=(AssetManager&&) = delete;

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
        /// The blob's inflate and decode, and the GPU upload, run on the task system; the dependency
        /// loads, bindless registration and the cache swap run on the main thread during
        /// PumpFinalizes(). A cache hit (resident or pending) returns the existing handle. A
        /// resolution failure (NotFound/WrongType) returns an empty handle; a later failure — a
        /// corrupt blob, a dependency that does not resolve, a failed finalize — is logged, marks the
        /// handle failed (AssetHandle::HasFailed) and drops the id from the cache, so a later load of
        /// it starts over.
        template <typename T>
        AssetHandle<T> Load(AssetId id)
        {
            const Ref<Detail::AssetCacheEntry> entry = LoadUntyped(AssetTypeTrait<T>::Type, id);
            if (!entry)
            {
                return AssetHandle<T>();
            }

            return AssetHandle<T>(id, entry);
        }

        /// @brief Blocking load — resolves, loads, uploads, and finalizes inline, then returns a resident handle.
        ///
        /// Dependency loads are synchronous and eager. Returns a structured AssetLoadError on failure
        /// (branch on AssetError::Kind). Bypasses the async continuation queue entirely — no deadlock risk.
        template <typename T>
        AssetResult<AssetHandle<T>> LoadSync(AssetId id)
        {
            const AssetResult<Ref<Detail::AssetCacheEntry>> entry =
                LoadSyncUntyped(AssetTypeTrait<T>::Type, id);
            if (!entry)
            {
                return std::unexpected(entry.error());
            }

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

        /// @brief Returns a typed handle onto a cache entry, the inverse of EntryOf.
        ///
        /// For a two-phase loader's completion, which receives its dependencies as cache entries
        /// (Detail::ParsedAsset::Complete). The caller owns the type contract, as with LoadUntyped:
        /// @p entry must hold an asset of AssetTypeTrait<T>::Type.
        /// @param entry  The cache entry; null yields an empty handle.
        /// @return A handle sharing @p entry.
        template <typename T>
        [[nodiscard]] static AssetHandle<T> HandleOf(const Ref<Detail::AssetCacheEntry>& entry)
        {
            if (entry == nullptr)
            {
                return AssetHandle<T>();
            }
            VE_ASSERT(entry->Type == AssetTypeTrait<T>::Type,
                      "AssetManager::HandleOf: asset {} is not of the requested type",
                      entry->Id.Value);
            return AssetHandle<T>(entry->Id, entry);
        }

        /// @brief Wraps an already-resident, runtime-created resource in an AssetHandle<T>.
        ///
        /// The handle carries the invalid AssetId (Id().IsValid() == false): a runtime resource
        /// has no content identity, so a reflective serializer records it as "no asset". The
        /// backing cache entry is detached — never inserted into the AssetId map, so CollectGarbage()
        /// leaves it alone. Each call yields a distinct entry; adopting does not deduplicate.
        ///
        /// Static, because a detached entry needs nothing from a manager: a resource built with no
        /// device — a CPU-only asset in a headless tool or test — is wrapped without constructing
        /// one. Calling it on an instance is unchanged. The Task overload below is not static: it
        /// needs the task system to run the factory.
        template <typename T>
        [[nodiscard]] static AssetHandle<T> Adopt(Ref<T> resource)
        {
            VE_ASSERT(resource != nullptr, "AssetManager::Adopt: resource is null");

            auto entry = CreateRef<Detail::AssetCacheEntry>(Detail::AssetCacheEntry{
                .Id = AssetId{},
                .Type = AssetTypeTrait<T>::Type,
                .Resource = std::static_pointer_cast<void>(std::move(resource)),
            });

            return AssetHandle<T>(AssetId{}, std::move(entry));
        }

        /// @brief Wraps a not-yet-built runtime resource in an AssetHandle<T> that becomes
        ///        resident when `factory` completes.
        ///
        /// The pending-resource overload of Adopt: where Adopt(Ref<T>) takes a resident resource,
        /// this takes a streaming one. Returns immediately with a pending handle
        /// (IsLoaded() == false). The factory's Task runs on the task system; its result is
        /// assigned into a detached cache entry through the main-thread continuation pump, after
        /// which IsLoaded() is true. Like the resident overload, the entry carries the invalid
        /// AssetId and is never inserted into the AssetId map, so a reflective serializer records
        /// it as "no asset" and CollectGarbage() leaves it alone; it stays alive exactly as long
        /// as a handle references it.
        ///
        /// While pending, a manager-owned keep-alive Ref holds the entry off CollectGarbage()'s
        /// use_count() == 1 eviction; the continuation drops it once the resource lands.
        /// @tparam T  The asset resource type the factory produces.
        /// @param factory  Task producing the resource; its continuation runs on the main thread.
        /// @return A pending handle that becomes resident through PumpMainThread().
        template <typename T>
        [[nodiscard]] AssetHandle<T> Adopt(Task<Ref<T>> factory)
        {
            auto entry = CreateRef<Detail::AssetCacheEntry>(Detail::AssetCacheEntry{
                .Id = AssetId{},
                .Type = AssetTypeTrait<T>::Type,
                .Resource = nullptr,
            });

            AddPendingCreate(entry);

            factory.Then(
                [this, entry](Result<Ref<T>> result) mutable
                {
                    if (!result)
                    {
                        FailPendingCreate(entry, result.error());
                        return;
                    }

                    FinalizePendingCreate(entry,
                                          std::static_pointer_cast<void>(std::move(*result)));
                });

            return AssetHandle<T>(AssetId{}, std::move(entry));
        }

        /// @brief Builds a runtime asset off the render thread, returning a pending handle.
        ///
        /// The async-default sibling of the synchronous X::BuildSync factories: the worker-legal
        /// construction (decode/upload) runs on the task system and the render-thread-only finalize
        /// (bindless registration) lands on the main-thread continuation pump, after which
        /// IsLoaded() is true. Like Adopt, the handle carries the invalid AssetId and its cache
        /// entry is detached — a runtime resource has no content identity. Supported for Texture,
        /// Mesh, Material, and MaterialInstance; the arguments are the asset's build description (a
        /// TextureData, a MeshData + name, a MaterialInfo + pipeline layout, or a
        /// MaterialInstanceInfo).
        /// @tparam T     The asset resource type to build.
        /// @tparam Args  The asset's build-description arguments.
        /// @param args   Forwarded to the per-type build (e.g. a TextureData, or a MeshData + name).
        /// @return A pending handle that becomes resident through PumpMainThread().
        template <typename T, typename... Args>
        [[nodiscard]] AssetHandle<T> Build(Args&&... args)
        {
            auto entry = CreateRef<Detail::AssetCacheEntry>(Detail::AssetCacheEntry{
                .Id = AssetId{},
                .Type = AssetTypeTrait<T>::Type,
                .Resource = nullptr,
            });

            AddPendingCreate(entry);

            Detail::SubmitAssetBuild(m_Context, m_Tasks, std::forward<Args>(args)...)
                .Then(
                    [this, entry](Result<Detail::BuiltAsset<T>> result) mutable
                    {
                        if (!result)
                        {
                            FailPendingCreate(entry, result.error());
                            return;
                        }

                        // The render-thread-only bindless registration runs here on the main-thread
                        // continuation, never on the worker that built the resource.
                        if (result->Finalize)
                        {
                            if (const VoidResult finalized = result->Finalize(); !finalized)
                            {
                                FailPendingCreate(entry, finalized.error());
                                return;
                            }
                        }

                        FinalizePendingCreate(
                            entry, std::static_pointer_cast<void>(std::move(result->Resource)));
                    });

            return AssetHandle<T>(AssetId{}, std::move(entry));
        }

        /// @brief Builds a runtime asset inline on the calling thread, returning a resident handle.
        ///
        /// The blocking sibling of Build<T>: it constructs, uploads, and finalizes the asset inline
        /// (the finalize — bindless registration — runs on the calling thread, so call this on the
        /// render thread), then adopts the ready resource. Like Build, the handle carries the
        /// invalid AssetId and its cache entry is detached. Supported for Texture, Mesh, Material,
        /// and MaterialInstance.
        /// @tparam T     The asset resource type to build.
        /// @tparam Args  The asset's build-description arguments.
        /// @param args   Forwarded to the per-type build (e.g. a TextureData, or a MeshData + name).
        /// @return A resident handle (IsLoaded() == true).
        template <typename T, typename... Args>
        [[nodiscard]] AssetHandle<T> BuildSync(Args&&... args)
        {
            return Adopt<T>(Detail::BuildAssetSync(m_Context, std::forward<Args>(args)...));
        }

        /// @brief Asynchronous load naming the asset type as a value rather than a template argument.
        ///
        /// What Load<T> is written on top of, exposed for the loaders that discover a dependency's
        /// type at runtime — a prefab resolving the AssetHandle fields of its components has an
        /// AssetTypeId in hand and no concrete T to name. Returns null on a resolution failure,
        /// exactly as Load<T> returns an empty handle.
        ///
        /// The caller owns the type contract: the entry is typed by @p type, and wrapping it in an
        /// AssetHandle<T> whose AssetTypeTrait<T>::Type differs is undefined. Prefer Load<T>
        /// wherever the type is known statically.
        /// @param type  The asset type to resolve and load as.
        /// @param id    The asset to load.
        /// @return The cache entry, or null when the id does not resolve.
        [[nodiscard]] Ref<Detail::AssetCacheEntry> LoadUntyped(AssetTypeId type, AssetId id);

        /// @brief Blocking sibling of LoadUntyped, returning a resident entry or a structured error.
        ///
        /// Carries the same caller-owned type contract as LoadUntyped. Prefer LoadSync<T> wherever
        /// the type is known statically.
        /// @param type  The asset type to resolve and load as.
        /// @param id    The asset to load.
        /// @return The resident cache entry, or the load error.
        [[nodiscard]] AssetResult<Ref<Detail::AssetCacheEntry>> LoadSyncUntyped(AssetTypeId type,
                                                                                AssetId id);

        /// @brief Returns an asset's cooked bytes as the mounted archives hold them, loading nothing.
        ///
        /// Resolves @p id exactly as a load does — memory mounts first, then on-disk archives in
        /// mount order — and checks the archive entry's type, but runs no loader: nothing is made
        /// resident, cached, registered, or uploaded, and the manager's render context is never
        /// touched. It is the seam for reading what a cooked asset says without presenting it (a
        /// headless process, a planner, a tool). A stored entry is a zero-copy view into the
        /// archive; a zstd entry is inflated on first read into the archive reader's own cache,
        /// where it stays for that archive's lifetime (the same cost a load of it pays).
        /// @param type  The asset type the entry must carry.
        /// @param id    The asset to read.
        /// @return A view of the cooked blob, valid while the archive holding it stays mounted, or
        ///         NotFound / WrongType.
        [[nodiscard]] AssetResult<std::span<const u8>> ReadCooked(AssetTypeId type,
                                                                  AssetId id) const;

        /// @brief Reads a cooked mesh's sockets without making the mesh resident.
        ///
        /// Decodes the socket table of the mesh's cooked blob (ParseCookedMeshSockets) — the same
        /// decoder the mesh loader runs, so the result equals Mesh::GetSockets() of the resident
        /// mesh — and touches nothing else: no vertex or index buffer, no material dependency, no
        /// render context. Costs what ReadCooked costs plus the socket decode; nothing is cached,
        /// so a caller reading the same mesh repeatedly caches the result itself. Include
        /// Veng/Asset/Mesh.h to use the MeshSocket values.
        /// @param mesh  The AssetTypes::Mesh asset to read.
        /// @return The sockets, sorted by name, or NotFound / WrongType / Corrupt.
        [[nodiscard]] AssetResult<vector<MeshSocket>> ReadMeshSockets(AssetId mesh) const;

        /// @brief Reads a cooked skeleton without making it resident.
        ///
        /// Decodes the skeleton's cooked blob through ParseCookedSkeleton — the decoder the
        /// skeleton loader runs, so the result equals the resident Skeleton of the same id — into
        /// a Skeleton by value: bone names, parents, bind-local transforms, inverse-binds and the
        /// global inverse. Nothing is cached or registered and no render context is touched, so a
        /// headless process can place a joint (Skeleton::JointModelTransform) of a model it never
        /// draws. A caller reading the same skeleton repeatedly caches the result itself.
        /// @param skeleton  The AssetTypes::Skeleton asset to read.
        /// @return The skeleton, or NotFound / WrongType / Corrupt.
        [[nodiscard]] AssetResult<Skeleton> ReadSkeleton(AssetId skeleton) const;

        /// @brief Reads the skeleton a cooked skinned mesh references, making neither resident.
        ///
        /// Decodes only the mesh blob's header to find its skeleton reference, then reads that
        /// skeleton as ReadSkeleton does. It is the CPU counterpart of Mesh::GetSkeleton() on the
        /// resident mesh, for a process that holds a mesh id but never presents the model.
        /// @param mesh  The AssetTypes::Mesh asset whose skeleton is read.
        /// @return The skeleton; NotFound / WrongType / Corrupt for the mesh; LoadFailed when the
        ///         mesh is static (references no skeleton); MissingDependency when its skeleton is
        ///         not mounted.
        [[nodiscard]] AssetResult<Skeleton> ReadMeshSkeleton(AssetId mesh) const;

        /// @brief Returns the cache entry for an id, or null if it is not cached.
        ///
        /// Untyped — the prefab loader uses it to rehydrate an embedded handle without naming
        /// the asset's concrete type. One id names one cached asset of one type, so a direct
        /// lookup is unambiguous. Never touches mounted archives or loaders.
        [[nodiscard]] Ref<Detail::AssetCacheEntry> CachedEntry(AssetId id) const
        {
            const auto it = m_Cache.find(id);
            if (it == m_Cache.end())
            {
                return nullptr;
            }
            return it->second;
        }

        /// @brief Cache-only typed lookup — never touches mounted archives or loaders.
        template <typename T>
        [[nodiscard]] optional<AssetHandle<T>> Get(AssetId id) const
        {
            const auto it = m_Cache.find(id);
            if (it == m_Cache.end())
            {
                return std::nullopt;
            }

            return AssetHandle<T>(id, it->second);
        }

        /// @brief Returns the type registry the prefab loader and editor reflect components through.
        [[nodiscard]] TypeRegistry& GetTypeRegistry() const { return m_Types; }

        /// @brief Returns the asset types this manager knows: every builtin, plus any the host merged in.
        ///
        /// Always populated — the manager fills it with the builtins at construction — so a
        /// consumer resolving an AssetHandle field's leaf TypeId, or naming a type in a
        /// diagnostic, never has to handle a missing registry.
        [[nodiscard]] const AssetTypeRegistry& GetAssetTypes() const { return m_AssetTypes; }

        /// @brief Returns the render context the manager builds and uploads GPU resources through.
        [[nodiscard]] Renderer::Context& GetContext() const { return m_Context; }

        /// @brief Returns the task system the manager runs its async loads and builds through.
        ///
        /// The same worker pool a consumer threads into a SystemContext, reachable wherever an
        /// AssetManager is in hand.
        [[nodiscard]] TaskSystem& GetTaskSystem() const { return m_Tasks; }

        /// @brief Sets the global texture-quality mip-skip level read by future texture builds.
        ///
        /// A cappable texture built afterward drops its top @p level mip levels at upload, so a
        /// lower tier uploads a genuinely smaller image. Read at build time only: a resident texture
        /// is a cache hit and is not rebuilt, so a change takes effect for textures loaded fresh
        /// afterward — in practice on the next process start. 0 (the default) uploads the full chain.
        void SetTextureQualityMipSkip(u32 level) { m_TextureQualityMipSkip = level; }

        /// @brief Returns the global texture-quality mip-skip level future texture builds read.
        [[nodiscard]] u32 GetTextureQualityMipSkip() const { return m_TextureQualityMipSkip; }

        /// @brief Points font loads at the shared runtime rasterizer and dynamic glyph atlas.
        ///
        /// The FontLoader loads a font's face into @p source and warms its hot set into @p atlas at
        /// load, and each loaded Font resolves and ensures glyphs through them. Both are owned by
        /// Application and outlive the manager's font handles; a manager with neither set (a headless
        /// test) still loads a font's cooked-charset atlas, but the font carries no runtime face.
        /// @param source  The shared GlyphSource, or null to clear.
        /// @param atlas   The shared GlyphAtlas, or null to clear.
        void SetGlyphSystems(Text::GlyphSource* source, Text::GlyphAtlas* atlas)
        {
            m_GlyphSource = source;
            m_GlyphAtlas = atlas;
        }

        /// @brief Returns the shared runtime rasterizer font loads use, or null when none is set.
        [[nodiscard]] Text::GlyphSource* GetGlyphSource() const { return m_GlyphSource; }

        /// @brief Returns the shared dynamic glyph atlas font loads use, or null when none is set.
        [[nodiscard]] Text::GlyphAtlas* GetGlyphAtlas() const { return m_GlyphAtlas; }

        /// @brief Lands finished parses and runs any pending finalizes whose dependencies are resident.
        ///
        /// Called from the frame loop after the task system's continuation pump, on the main thread.
        /// A two-phase load whose worker parse has finished lands here first: the dependencies it
        /// names are loaded (each parsing on a worker in turn) and its completion runs; it then
        /// finalizes, here or on a later pump, once those dependencies are resident. Never waits for
        /// a parse still running. Safe to call from inside a finalize.
        void PumpFinalizes();

        /// @brief Returns how many asynchronous loads have a parse not yet landed by PumpFinalizes.
        [[nodiscard]] usize GetParsingCount() const { return m_Parsing.size(); }

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

        /// @brief A submitted-but-not-yet-finalized async load, or a pending-Adopt keep-alive.
        ///
        /// The cache entry exists with a null Resource (pending); Finalize swaps the resource in
        /// once every Dependency is resident and finalized. The render graph folds the
        /// transfer-timeline wait into the first frame that samples the resource, so registration
        /// is safe before the GPU copy lands. A pending-Adopt entry rides this list as a bare
        /// keep-alive — null Finalize, finalized by its own factory continuation — so
        /// PumpFinalizes() steps over it.
        struct PendingLoad
        {
            /// @brief The asset being loaded; invalid for a pending-Adopt keep-alive.
            AssetId Id;
            /// @brief The cache slot (Resource is null until finalized).
            Ref<Detail::AssetCacheEntry> Entry;
            /// @brief The created-but-unregistered resource; null for a pending-Adopt keep-alive.
            Detail::RefAny Resource;
            /// @brief Kept alive until Finalize runs.
            vector<Ref<Detail::AssetCacheEntry>> Dependencies;
            /// @brief Main-thread registration step; null for a pending-Adopt keep-alive or when not needed.
            function<VoidResult()> Finalize;
        };

        /// @brief Where an asset's cooked blob lives, found without inflating it.
        struct BlobLocation
        {
            /// @brief The mounted reader holding the blob.
            const ArchiveReader* Reader = nullptr;
            /// @brief The entry as stored, carrying its type; its Blob is the stored, not inflated, bytes.
            ArchiveEntry Stored;
        };

        /// @brief A two-phase load whose Parse is running on a worker, or has finished unlanded.
        struct InFlightParse
        {
            /// @brief The asset being loaded.
            AssetId Id;
            /// @brief The pending cache slot the load fills.
            Ref<Detail::AssetCacheEntry> Entry;
            /// @brief The worker running the loader's Parse over the inflated blob.
            Task<AssetResult<Detail::ParsedAsset>> Parse;
            /// @brief The parse's result, once WaitForParses collected it ahead of its landing.
            optional<Result<AssetResult<Detail::ParsedAsset>>> Outcome;
        };

        /// @brief Finds the reader holding @p id, in Find's precedence, without inflating anything.
        [[nodiscard]] optional<BlobLocation> Locate(AssetId id) const;

        /// @brief Builds the context a loader's Parse runs with.
        /// @param async  Whether the parse runs on a worker for an asynchronous load.
        [[nodiscard]] AssetParseContext MakeParseContext(bool async) const;

        /// @brief Lands the in-flight parses that have finished — or, with @p wait, all of them.
        ///
        /// Landing one loads the dependencies it names, which may start further parses; with @p wait
        /// those are waited for and landed too, so the list is empty on return.
        void LandParses(bool wait);

        /// @brief Waits for every in-flight parse's worker to finish, keeping the results to land later.
        ///
        /// What a mount or unmount waits on first: a running parse reads its blob through a mounted
        /// reader, which the mount list's change would move or free.
        void WaitForParses();

        /// @brief Completes one finished parse: its dependencies, its completion, its finalize entry.
        void LandParse(InFlightParse parse);

        /// @brief Files a load job against its pending entry: resident at once without a finalize,
        ///        otherwise queued for PumpFinalizes.
        void EnqueueLoadJob(AssetId id, const Ref<Detail::AssetCacheEntry>& entry,
                            Detail::LoadJob job);

        /// @brief Marks an asynchronous load failed: logs, flags the entry and drops it from the cache.
        void FailLoad(AssetId id, const Ref<Detail::AssetCacheEntry>& entry, const string& detail);

        /// @brief Runs a two-phase loader inline for LoadSync: parse, blocking dependency loads, completion.
        [[nodiscard]] AssetResult<Detail::LoadJob> RunParseInline(const AssetLoader& loader,
                                                                  AssetTypeId type, AssetId id);

        /// @brief Registers a manager-owned keep-alive for a pending-Adopt entry.
        ///
        /// The Ref holds the detached entry off CollectGarbage()'s use_count() == 1 eviction
        /// until its continuation resolves it. The keep-alive carries no Finalize, so
        /// PumpFinalizes() steps over it — its own continuation does the finalization.
        void AddPendingCreate(Ref<Detail::AssetCacheEntry> entry);

        /// @brief Resolves a pending-Adopt entry, swapping in its resource and dropping the keep-alive.
        ///
        /// Runs on the main thread from the factory task's continuation; after it, IsLoaded()
        /// is true and the entry reverts to the resident-Adopt lifetime (alive only while a handle holds it).
        void FinalizePendingCreate(const Ref<Detail::AssetCacheEntry>& entry,
                                   Detail::RefAny resource);

        /// @brief Drops a pending-Adopt entry's keep-alive after its factory failed.
        ///
        /// Runs on the main thread; the entry is marked failed, never becomes resident, and is
        /// freed once the last handle drops — mirroring an async Load's deferred-failure behavior,
        /// including leaving the cache when the entry is id-keyed.
        void FailPendingCreate(const Ref<Detail::AssetCacheEntry>& entry, const string& error);

        /// @brief Finds an id's archive entry and checks it carries the requested type.
        ///
        /// Shared by the load paths and ReadCooked, so a read and a load resolve identically.
        [[nodiscard]] AssetResult<ArchiveEntry> FindTyped(AssetTypeId type, AssetId id) const;

        /// @brief Resolves an id to a loader and cooked blob, validating type against the archive entry.
        ///
        /// Shared by the async and sync load paths.
        [[nodiscard]] AssetResult<std::pair<AssetLoader*, ArchiveEntry>> Resolve(AssetTypeId type,
                                                                                 AssetId id);

        /// @brief Resolves and runs the loader for a (type, id).
        ///
        /// The archive entry's type must match the requested type exactly; a mismatch is a
        /// WrongType error. Shared by the async and sync load paths.
        [[nodiscard]] AssetResult<Detail::LoadJob> RunLoader(AssetTypeId type, AssetId id,
                                                             bool async);

        [[nodiscard]] optional<ArchiveEntry> Find(AssetId id) const;

        void RegisterLoader(Unique<AssetLoader> loader);

        /// @brief Names an asset type for a diagnostic, falling back to its hex id when unregistered.
        [[nodiscard]] string TypeName(AssetTypeId type) const;

        Renderer::Context& m_Context;
        TaskSystem& m_Tasks;
        /// @brief Borrowed; the prefab loader reflects component fields through it.
        TypeRegistry& m_Types;
        /// @brief Owned: the builtins, plus whatever AssetManagerInfo::AssetTypes added.
        AssetTypeRegistry m_AssetTypes;

        /// @brief The global texture-quality mip-skip level applied to future cappable texture builds.
        u32 m_TextureQualityMipSkip = 0;

        /// @brief The shared runtime rasterizer font loads use; non-owning, null until wired.
        Text::GlyphSource* m_GlyphSource = nullptr;
        /// @brief The shared dynamic glyph atlas font loads use; non-owning, null until wired.
        Text::GlyphAtlas* m_GlyphAtlas = nullptr;

        vector<MountedArchive> m_Mounts;
        vector<MemoryMount> m_MemoryMounts;
        u64 m_NextMemoryToken = 1;
        unordered_map<AssetTypeId, Unique<AssetLoader>> m_Loaders;
        std::unordered_map<AssetId, Ref<Detail::AssetCacheEntry>> m_Cache;
        vector<PendingLoad> m_Pending;
        /// @brief Two-phase loads whose worker parse has not been landed yet, in issue order.
        vector<InFlightParse> m_Parsing;
    };
}
