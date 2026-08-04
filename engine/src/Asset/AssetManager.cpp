#include <Veng/Asset/AssetManager.h>

#include <Veng/Assert.h>
#include <Veng/Asset/HexId.h>
#include <Veng/Diagnostics/Profiler.h>
#include <Veng/Log.h>
#include <Veng/Task/TaskSystem.h>

#include "Loaders/AnimationLoader.h"
#include "Loaders/AudioClipLoader.h"
#include "Loaders/CollisionShapeLoader.h"
#include "Loaders/DataTableLoader.h"
#include "Loaders/EnvironmentLoader.h"
#include "Loaders/FontLoader.h"
#include "Loaders/InputMapLoader.h"
#include "Loaders/LevelLoader.h"
#include "Loaders/MaterialInstanceLoader.h"
#include "Loaders/MaterialLoader.h"
#include "Loaders/MeshLoader.h"
#include "Loaders/PrefabLoader.h"
#include "Loaders/RawAssetLoader.h"
#include "Loaders/ShaderLoader.h"
#include "Loaders/SkeletonLoader.h"
#include "Loaders/TableSchemaLoader.h"
#include "Loaders/StyleSheetLoader.h"
#include "Loaders/TextureLoader.h"
#include "Loaders/UIDocumentLoader.h"
#include "Loaders/VertexLayoutLoader.h"

#ifdef VENG_HAS_CORE_PACK
namespace Veng
{
    extern const unsigned char g_CoreLayoutPack[];
    extern const unsigned long g_CoreLayoutPackSize;
}
#endif

namespace Veng
{
    AssetManager::AssetManager(Renderer::Context& context, TaskSystem& tasks, TypeRegistry& types,
                               const AssetManagerInfo& info)
        : m_Context(context), m_Tasks(tasks), m_Types(types)
    {
        // The builtins first, then the host's additions — mirroring the loader registration
        // below. A handle field on a component resolves through this registry, so it must be
        // complete whether or not the host wired one up.
        RegisterBuiltinAssetTypes(m_AssetTypes);
        if (info.AssetTypes != nullptr)
        {
            for (const auto& [id, typeInfo] : info.AssetTypes->All())
            {
                if (!m_AssetTypes.IsRegistered(id))
                {
                    m_AssetTypes.Register(typeInfo);
                }
            }
        }

        RegisterLoader(CreateUnique<RawAssetLoader>());
        RegisterLoader(CreateUnique<TextureLoader>());
        RegisterLoader(CreateUnique<MeshLoader>());
        RegisterLoader(CreateUnique<ShaderLoader>());
        RegisterLoader(CreateUnique<VertexLayoutLoader>());
        RegisterLoader(CreateUnique<MaterialLoader>());
        RegisterLoader(CreateUnique<MaterialInstanceLoader>());
        RegisterLoader(CreateUnique<PrefabLoader>());
        RegisterLoader(CreateUnique<LevelLoader>());
        RegisterLoader(CreateUnique<SkeletonLoader>());
        RegisterLoader(CreateUnique<AnimationLoader>());
        RegisterLoader(CreateUnique<EnvironmentLoader>());
        RegisterLoader(CreateUnique<FontLoader>());
        RegisterLoader(CreateUnique<InputMapLoader>());
        RegisterLoader(CreateUnique<StyleSheetLoader>());
        RegisterLoader(CreateUnique<UIDocumentLoader>());
        RegisterLoader(CreateUnique<TableSchemaLoader>());
        RegisterLoader(CreateUnique<DataTableLoader>());
        RegisterLoader(CreateUnique<CollisionShapeLoader>());
        RegisterLoader(CreateUnique<AudioClipLoader>());

        // Module-registered loaders come last, so a factory claiming a type the engine already
        // handles is caught rather than silently shadowing the builtin — override semantics for
        // builtin types stay engine-owned.
        if (info.Loaders != nullptr)
        {
            for (const auto& [type, factory] : info.Loaders->All())
            {
                VE_ASSERT(!m_Loaders.contains(type),
                          "AssetManager: a module registered a loader for asset type {}, which the "
                          "engine already handles",
                          TypeName(type));
                Unique<AssetLoader> loader = factory();
                VE_ASSERT(loader != nullptr,
                          "AssetManager: loader factory for asset type {} produced null",
                          TypeName(type));
                VE_ASSERT(loader->Type() == type,
                          "AssetManager: loader factory registered for asset type {} produced a "
                          "loader claiming {}",
                          TypeName(type), TypeName(loader->Type()));
                RegisterLoader(std::move(loader));
            }
        }

#ifdef VENG_HAS_CORE_PACK
        const VoidResult coreMount = MountBytes(
            path("<core>"),
            std::span<const u8>(g_CoreLayoutPack, static_cast<usize>(g_CoreLayoutPackSize)));
        VE_ASSERT(coreMount.has_value(), "AssetManager: failed to mount embedded core pack: {}",
                  coreMount.error());
#endif
    }

    AssetManager::~AssetManager() = default;

    VoidResult AssetManager::Mount(const path& archive)
    {
        for (const MountedArchive& mount : m_Mounts)
        {
            if (mount.Path == archive)
            {
                return {};
            }
        }

        Result<ArchiveReader> reader = ArchiveReader::Open(archive);
        if (!reader)
        {
            return std::unexpected(reader.error());
        }

        m_Mounts.push_back(MountedArchive{.Path = archive, .Reader = std::move(*reader)});
        return {};
    }

    VoidResult AssetManager::MountBytes(const path& identity, std::span<const u8> bytes)
    {
        for (const MountedArchive& mount : m_Mounts)
        {
            if (mount.Path == identity)
            {
                return {};
            }
        }

        Result<ArchiveReader> reader = ArchiveReader::FromBytes(bytes);
        if (!reader)
        {
            return std::unexpected(reader.error());
        }

        m_Mounts.push_back(MountedArchive{.Path = identity, .Reader = std::move(*reader)});
        return {};
    }

    void AssetManager::Unmount(const path& archive)
    {
        std::erase_if(m_Mounts,
                      [&archive](const MountedArchive& mount) { return mount.Path == archive; });
    }

    void AssetManager::CollectGarbage()
    {
        // A pending (not-yet-resident) entry is referenced by its PendingLoad, so
        // its use_count is never 1 while in flight — it is never evicted.
        std::erase_if(m_Cache, [](const auto& entry) { return entry.second.use_count() == 1; });
    }

    void AssetManager::RegisterLoader(Unique<AssetLoader> loader)
    {
        const AssetTypeId type = loader->Type();
        m_Loaders[type] = std::move(loader);
    }

    string AssetManager::TypeName(AssetTypeId type) const
    {
        return m_AssetTypes.GetName(type);
    }

    MountHandle::~MountHandle()
    {
        Release();
    }

    MountHandle::MountHandle(MountHandle&& other) noexcept
        : m_Manager(other.m_Manager), m_Token(other.m_Token)
    {
        other.m_Manager = nullptr;
        other.m_Token = 0;
    }

    MountHandle& MountHandle::operator=(MountHandle&& other) noexcept
    {
        if (this != &other)
        {
            Release();
            m_Manager = other.m_Manager;
            m_Token = other.m_Token;
            other.m_Manager = nullptr;
            other.m_Token = 0;
        }
        return *this;
    }

    void MountHandle::Release()
    {
        if (m_Manager)
        {
            m_Manager->UnmountMemory(m_Token);
            m_Manager = nullptr;
            m_Token = 0;
        }
    }

    MountHandle AssetManager::MountMemory(vector<u8> archiveBytes, string debugName)
    {
        Result<ArchiveReader> reader = ArchiveReader::FromBytes(archiveBytes);
        VE_ASSERT(reader.has_value(), "AssetManager::MountMemory: '{}': {}", debugName,
                  reader.error());

        const u64 token = m_NextMemoryToken++;
        m_MemoryMounts.push_back(MemoryMount{
            .Token = token,
            .DebugName = std::move(debugName),
            .Reader = std::move(*reader),
        });

        return MountHandle(*this, token);
    }

    void AssetManager::UnmountMemory(u64 token)
    {
        std::erase_if(m_MemoryMounts,
                      [token](const MemoryMount& mount) { return mount.Token == token; });
    }

    optional<ArchiveEntry> AssetManager::Find(AssetId id) const
    {
        // Memory mounts shadow on-disk archives: a freshly cooked blob overrides
        // its on-disk version. The most recently mounted wins among memory mounts.
        for (auto it = m_MemoryMounts.rbegin(); it != m_MemoryMounts.rend(); ++it)
        {
            if (optional<ArchiveEntry> entry = it->Reader.Find(id))
            {
                return entry;
            }
        }

        for (const MountedArchive& mount : m_Mounts)
        {
            if (optional<ArchiveEntry> entry = mount.Reader.Find(id))
            {
                return entry;
            }
        }

        return std::nullopt;
    }

    AssetResult<std::pair<AssetLoader*, ArchiveEntry>> AssetManager::Resolve(AssetTypeId type,
                                                                             AssetId id)
    {
        const optional<ArchiveEntry> found = Find(id);
        if (!found)
        {
            return std::unexpected(AssetLoadError{
                .Kind = AssetError::NotFound,
                .Id = id,
                .Detail = fmt::format("asset {} not found in any mounted archive", id.Value),
            });
        }

        if (found->Type != type)
        {
            return std::unexpected(AssetLoadError{
                .Kind = AssetError::WrongType,
                .Id = id,
                .Detail = fmt::format("asset {} is asset type {}, not {}", id.Value,
                                      TypeName(found->Type), TypeName(type)),
            });
        }

        const auto loaderIt = m_Loaders.find(type);
        if (loaderIt == m_Loaders.end())
        {
            return std::unexpected(AssetLoadError{
                .Kind = AssetError::LoadFailed,
                .Id = id,
                .Detail = fmt::format("no loader registered for asset type {}", TypeName(type)),
            });
        }

        return std::pair{loaderIt->second.get(), *found};
    }

    AssetResult<Detail::LoadJob> AssetManager::RunLoader(AssetTypeId type, AssetId id, bool async)
    {
        const AssetResult<std::pair<AssetLoader*, ArchiveEntry>> resolved = Resolve(type, id);
        if (!resolved)
        {
            return std::unexpected(resolved.error());
        }

        return resolved->first->Load(*this, m_Context, m_Tasks, m_Types, id, resolved->second.Blob,
                                     async);
    }

    Ref<Detail::AssetCacheEntry> AssetManager::LoadUntyped(AssetTypeId type, AssetId id)
    {
        // A cache hit (resident or pending) returns the existing entry. One id names one asset of
        // one type, so a cached entry of a different type is a failed load, not a reinterpret.
        if (const auto it = m_Cache.find(id); it != m_Cache.end())
        {
            if (it->second->Type != type)
            {
                Log::Error("AssetManager::Load: asset {} is asset type {}, not {}", id.Value,
                           FormatHexId(it->second->Type.Value), FormatHexId(type.Value));
                return nullptr;
            }
            return it->second;
        }

        // The blob lives in the mounted archive reader's storage, so the span outlives the call.
        AssetResult<Detail::LoadJob> job = RunLoader(type, id, true);
        if (!job)
        {
            Log::Error("AssetManager::Load: {}", job.error().Detail);
            return nullptr;
        }

        // Create the entry pending (null Resource) and return it; the PendingLoad holds the resource until Finalize swaps it in.
        Ref<Detail::AssetCacheEntry> entry =
            CreateRef<Detail::AssetCacheEntry>(Detail::AssetCacheEntry{
                .Id = id,
                .Type = type,
                .Resource = nullptr,
            });
        m_Cache[id] = entry;

        if (job->Finalize)
        {
            m_Pending.push_back(PendingLoad{
                .Id = id,
                .Entry = entry,
                .Resource = std::move(job->Resource),
                .Dependencies = std::move(job->Dependencies),
                .Finalize = std::move(job->Finalize),
            });
        }
        else
        {
            // No finalize (Raw/Mesh/Shader/VertexLayout): the resource is resident
            // the moment its worker phase returns — swap it in immediately.
            entry->Resource = std::move(job->Resource);
        }

        return entry;
    }

    void AssetManager::PumpFinalizes()
    {
        // Finalize every pending load whose dependencies are resident. A material whose textures
        // finalize this same pump waits one more iteration; the loop terminates because each
        // pass makes monotonic progress (at least one entry finalizes per pass).
        bool progressed = true;
        while (progressed)
        {
            progressed = false;

            for (usize i = 0; i < m_Pending.size();)
            {
                PendingLoad& pending = m_Pending[i];

                // A pending-Adopt keep-alive rides this list with no Finalize; its own
                // factory continuation resolves the entry, so step over it here.
                if (!pending.Finalize)
                {
                    ++i;
                    continue;
                }

                bool depsReady = true;
                for (const Ref<Detail::AssetCacheEntry>& dep : pending.Dependencies)
                {
                    if (dep == nullptr || dep->Resource == nullptr)
                    {
                        depsReady = false;
                        break;
                    }
                }

                if (!depsReady)
                {
                    ++i;
                    continue;
                }

                const VoidResult finalized = pending.Finalize();
                if (!finalized)
                {
                    // A deferred failure leaves the entry permanently pending;
                    // log and drop the PendingLoad so it isn't retried forever.
                    Log::Error("AssetManager: async finalize of asset {} failed: {}",
                               pending.Id.Value, finalized.error());
                }
                else
                {
                    // The decode → upload → continuation chain lands back on the main thread here;
                    // the instant marks where an async load becomes resident.
                    VE_PROFILE_INSTANT("Asset/Finalize");
                    pending.Entry->Resource = std::move(pending.Resource);
                }

                m_Pending.erase(m_Pending.begin() + static_cast<std::ptrdiff_t>(i));
                progressed = true;
            }
        }
    }

    void AssetManager::AddPendingCreate(Ref<Detail::AssetCacheEntry> entry)
    {
        m_Pending.push_back(PendingLoad{
            .Id = AssetId{},
            .Entry = std::move(entry),
            .Resource = nullptr,
            .Dependencies = {},
            .Finalize = nullptr,
        });
    }

    void AssetManager::FinalizePendingCreate(const Ref<Detail::AssetCacheEntry>& entry,
                                             Detail::RefAny resource)
    {
        entry->Resource = std::move(resource);
        std::erase_if(m_Pending,
                      [&entry](const PendingLoad& pending) { return pending.Entry == entry; });
    }

    void AssetManager::FailPendingCreate(const Ref<Detail::AssetCacheEntry>& entry,
                                         const string& error)
    {
        Log::Error("AssetManager: async Adopt factory failed: {}", error);
        std::erase_if(m_Pending,
                      [&entry](const PendingLoad& pending) { return pending.Entry == entry; });
    }

    AssetResult<Ref<Detail::AssetCacheEntry>> AssetManager::LoadSyncUntyped(AssetTypeId type,
                                                                            AssetId id)
    {
        if (const auto it = m_Cache.find(id); it != m_Cache.end())
        {
            // One id names one asset of one type: a cached entry of a different type is a
            // WrongType, the same as a miss against an archive entry of the wrong type.
            if (it->second->Type != type)
            {
                return std::unexpected(AssetLoadError{
                    .Kind = AssetError::WrongType,
                    .Id = id,
                    .Detail =
                        fmt::format("asset {} is asset type {}, not {}", id.Value,
                                    FormatHexId(it->second->Type.Value), FormatHexId(type.Value)),
                });
            }

            // The id was already Load()ed async and is still pending. A sync
            // handle must be resident, so drain the finalize queue now (it
            // finalizes dependencies before dependents) to land it inline.
            if (it->second->Resource == nullptr)
            {
                PumpFinalizes();
                if (it->second->Resource == nullptr)
                {
                    return std::unexpected(AssetLoadError{
                        .Kind = AssetError::LoadFailed,
                        .Id = id,
                        .Detail = fmt::format("asset {} is still pending an async load", id.Value),
                    });
                }
            }

            return it->second;
        }

        AssetResult<Detail::LoadJob> job = RunLoader(type, id, false);
        if (!job)
        {
            return std::unexpected(job.error());
        }

        // Finalize inline (on the main thread, blocking) — bypassing the async
        // continuation queue entirely, so there is no self-deadlock. The
        // dependencies were resolved through LoadSync above, so they are already
        // resident and finalized.
        if (job->Finalize)
        {
            const VoidResult finalized = job->Finalize();
            if (!finalized)
            {
                return std::unexpected(AssetLoadError{
                    .Kind = AssetError::Corrupt,
                    .Id = id,
                    .Detail = finalized.error(),
                });
            }
        }

        Ref<Detail::AssetCacheEntry> entry =
            CreateRef<Detail::AssetCacheEntry>(Detail::AssetCacheEntry{
                .Id = id,
                .Type = type,
                .Resource = std::move(job->Resource),
            });

        m_Cache[id] = entry;
        return entry;
    }
}
