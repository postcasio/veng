#include <Veng/Asset/AssetManager.h>

#include <cstring>

#include <Veng/Assert.h>
#include <Veng/Asset/HexId.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Skeleton.h>
#include <Veng/Diagnostics/Profiler.h>
#include <Veng/Log.h>
#include <Veng/Task/TaskSystem.h>

#include "Loaders/AnimationLoader.h"
#include "Loaders/AudioClipLoader.h"
#include "Loaders/CollisionShapeLoader.h"
#include "Loaders/DataTableLoader.h"
#include "Loaders/AudioBusGraphLoader.h"
#include "Loaders/SettingsSchemaLoader.h"
#include "Loaders/EnvironmentLoader.h"
#include "Loaders/FlipbookLoader.h"
#include "Loaders/FontLoader.h"
#include "Loaders/InputMapLoader.h"
#include "Loaders/RumbleClipLoader.h"
#include "Loaders/LevelLoader.h"
#include "Loaders/LocaleCatalogLoader.h"
#include "Loaders/LocaleIndexLoader.h"
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
        RegisterLoader(CreateUnique<LocaleCatalogLoader>());
        RegisterLoader(CreateUnique<LocaleIndexLoader>());
        RegisterLoader(CreateUnique<InputMapLoader>());
        RegisterLoader(CreateUnique<RumbleClipLoader>());
        RegisterLoader(CreateUnique<StyleSheetLoader>());
        RegisterLoader(CreateUnique<UIDocumentLoader>());
        RegisterLoader(CreateUnique<TableSchemaLoader>());
        RegisterLoader(CreateUnique<DataTableLoader>());
        RegisterLoader(CreateUnique<CollisionShapeLoader>());
        RegisterLoader(CreateUnique<AudioClipLoader>());
        RegisterLoader(CreateUnique<SettingsSchemaLoader>());
        RegisterLoader(CreateUnique<AudioBusGraphLoader>());
        RegisterLoader(CreateUnique<FlipbookLoader>());

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

    AssetManager::~AssetManager()
    {
        // A parse still running reads through a mounted reader and this manager's registries, so
        // it finishes before any of them go.
        WaitForParses();
    }

    VoidResult AssetManager::Mount(const path& archive)
    {
        // A running parse holds a pointer to its reader, which growing the mount list would move.
        WaitForParses();
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
        WaitForParses();
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
        WaitForParses();
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
        WaitForParses();
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
        WaitForParses();
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

    optional<AssetManager::BlobLocation> AssetManager::Locate(AssetId id) const
    {
        // The precedence Find resolves with: the newest memory mount, then the on-disk mounts in
        // mount order.
        for (auto it = m_MemoryMounts.rbegin(); it != m_MemoryMounts.rend(); ++it)
        {
            if (optional<ArchiveEntry> stored = it->Reader.FindStored(id))
            {
                return BlobLocation{.Reader = &it->Reader, .Stored = *stored};
            }
        }
        for (const MountedArchive& mount : m_Mounts)
        {
            if (optional<ArchiveEntry> stored = mount.Reader.FindStored(id))
            {
                return BlobLocation{.Reader = &mount.Reader, .Stored = *stored};
            }
        }
        return std::nullopt;
    }

    AssetParseContext AssetManager::MakeParseContext(const bool async) const
    {
        return AssetParseContext{
            .Context = m_Context,
            .Tasks = m_Tasks,
            .Types = m_Types,
            .AssetTypes = m_AssetTypes,
            .TextureQualityMipSkip = m_TextureQualityMipSkip,
            .Async = async,
        };
    }

    AssetResult<ArchiveEntry> AssetManager::FindTyped(AssetTypeId type, AssetId id) const
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

        return *found;
    }

    AssetResult<std::span<const u8>> AssetManager::ReadCooked(AssetTypeId type, AssetId id) const
    {
        const AssetResult<ArchiveEntry> found = FindTyped(type, id);
        if (!found)
        {
            return std::unexpected(found.error());
        }
        return found->Blob;
    }

    AssetResult<vector<MeshSocket>> AssetManager::ReadMeshSockets(AssetId mesh) const
    {
        const AssetResult<std::span<const u8>> cooked = ReadCooked(AssetTypes::Mesh, mesh);
        if (!cooked)
        {
            return std::unexpected(cooked.error());
        }

        Result<vector<MeshSocket>> sockets = ParseCookedMeshSockets(*cooked);
        if (!sockets)
        {
            return std::unexpected(AssetLoadError{
                .Kind = AssetError::Corrupt, .Id = mesh, .Detail = std::move(sockets.error())});
        }
        return std::move(*sockets);
    }

    AssetResult<Skeleton> AssetManager::ReadSkeleton(AssetId skeleton) const
    {
        const AssetResult<std::span<const u8>> cooked = ReadCooked(AssetTypes::Skeleton, skeleton);
        if (!cooked)
        {
            return std::unexpected(cooked.error());
        }

        Result<Skeleton> decoded = ParseCookedSkeleton(*cooked);
        if (!decoded)
        {
            return std::unexpected(AssetLoadError{
                .Kind = AssetError::Corrupt, .Id = skeleton, .Detail = std::move(decoded.error())});
        }
        return std::move(*decoded);
    }

    AssetResult<Skeleton> AssetManager::ReadMeshSkeleton(AssetId mesh) const
    {
        const AssetResult<std::span<const u8>> cooked = ReadCooked(AssetTypes::Mesh, mesh);
        if (!cooked)
        {
            return std::unexpected(cooked.error());
        }
        if (cooked->size() < sizeof(CookedMeshHeader))
        {
            return std::unexpected(
                AssetLoadError{.Kind = AssetError::Corrupt,
                               .Id = mesh,
                               .Detail = "mesh: cooked blob smaller than CookedMeshHeader"});
        }

        CookedMeshHeader header;
        std::memcpy(&header, cooked->data(), sizeof(header));
        if (header.Version != CookedMeshVersion)
        {
            return std::unexpected(AssetLoadError{
                .Kind = AssetError::Corrupt,
                .Id = mesh,
                .Detail = fmt::format("mesh: cooked version {} does not match expected {}",
                                      header.Version, CookedMeshVersion)});
        }
        if (header.SkeletonId == 0)
        {
            return std::unexpected(AssetLoadError{.Kind = AssetError::LoadFailed,
                                                  .Id = mesh,
                                                  .Detail = "mesh is static: no skeleton"});
        }

        const AssetId skeleton{header.SkeletonId};
        AssetResult<Skeleton> read = ReadSkeleton(skeleton);
        if (!read && read.error().Kind == AssetError::NotFound)
        {
            return std::unexpected(AssetLoadError{
                .Kind = AssetError::MissingDependency,
                .Id = skeleton,
                .Detail = fmt::format("mesh {}'s skeleton is not mounted", mesh.Value)});
        }
        return read;
    }

    AssetResult<std::pair<AssetLoader*, ArchiveEntry>> AssetManager::Resolve(AssetTypeId type,
                                                                             AssetId id)
    {
        const AssetResult<ArchiveEntry> found = FindTyped(type, id);
        if (!found)
        {
            return std::unexpected(found.error());
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

        // A two-phase loader parses on a worker: the entry is handed out pending now, and the
        // parse lands in PumpFinalizes. Only the TOC is read here — the blob's inflate runs on the
        // worker with its decode.
        const auto loaderIt = m_Loaders.find(type);
        if (loaderIt != m_Loaders.end() && loaderIt->second->ParsesOffThread())
        {
            const optional<BlobLocation> location = Locate(id);
            if (!location)
            {
                Log::Error("AssetManager::Load: asset {} not found in any mounted archive",
                           id.Value);
                return nullptr;
            }
            if (location->Stored.Type != type)
            {
                Log::Error("AssetManager::Load: asset {} is asset type {}, not {}", id.Value,
                           TypeName(location->Stored.Type), TypeName(type));
                return nullptr;
            }

            Ref<Detail::AssetCacheEntry> entry =
                CreateRef<Detail::AssetCacheEntry>(Detail::AssetCacheEntry{
                    .Id = id,
                    .Type = type,
                    .Resource = nullptr,
                });
            m_Cache[id] = entry;

            const AssetLoader* const loader = loaderIt->second.get();
            const ArchiveReader* const reader = location->Reader;
            const AssetParseContext context = MakeParseContext(true);
            m_Parsing.push_back(InFlightParse{
                .Id = id,
                .Entry = entry,
                .Parse = m_Tasks.Submit(
                    [loader, reader, context, id]() -> AssetResult<Detail::ParsedAsset>
                    {
                        const optional<ArchiveEntry> found = reader->Find(id);
                        if (!found)
                        {
                            return std::unexpected(AssetLoadError{
                                .Kind = AssetError::Corrupt,
                                .Id = id,
                                .Detail = fmt::format("asset {}'s blob does not inflate", id.Value),
                            });
                        }
                        return loader->Parse(context, id, found->Blob);
                    },
                    "Asset/Parse"),
            });
            return entry;
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

        if (job->AsyncResource)
        {
            // A CPU-heavy loader decodes on a worker; the id-keyed entry becomes resident when the
            // Task lands on the main-thread continuation pump, so the decode never blocks the render
            // thread and the asset keeps its AssetId (unlike Adopt's detached entry). A null result
            // stalls the entry unresident, the corrupt-blob shape.
            AddPendingCreate(entry);
            job->AsyncResource->Then(
                [this, entry](Result<Detail::RefAny> result) mutable
                {
                    if (result && *result != nullptr)
                    {
                        FinalizePendingCreate(entry, std::move(*result));
                        return;
                    }
                    FailPendingCreate(entry, result ? "loader decode produced no resource"
                                                    : result.error());
                });
            return entry;
        }

        EnqueueLoadJob(id, entry, std::move(*job));
        return entry;
    }

    void AssetManager::EnqueueLoadJob(const AssetId id, const Ref<Detail::AssetCacheEntry>& entry,
                                      Detail::LoadJob job)
    {
        if (job.Finalize)
        {
            m_Pending.push_back(PendingLoad{
                .Id = id,
                .Entry = entry,
                .Resource = std::move(job.Resource),
                .Dependencies = std::move(job.Dependencies),
                .Finalize = std::move(job.Finalize),
            });
            return;
        }
        // No finalize: the resource is resident the moment its job exists.
        entry->Resource = std::move(job.Resource);
    }

    void AssetManager::FailLoad(const AssetId id, const Ref<Detail::AssetCacheEntry>& entry,
                                const string& detail)
    {
        Log::Error("AssetManager: async load of asset {} failed: {}", id.Value, detail);
        entry->Failed = true;
        // The id is dropped from the cache so a later load of it starts over rather than handing
        // back an entry that will never become resident.
        if (const auto it = m_Cache.find(id); it != m_Cache.end() && it->second == entry)
        {
            m_Cache.erase(it);
        }
    }

    void AssetManager::WaitForParses()
    {
        for (InFlightParse& parse : m_Parsing)
        {
            if (!parse.Outcome)
            {
                parse.Outcome = parse.Parse.Get();
            }
        }
    }

    void AssetManager::LandParses(const bool wait)
    {
        // By index over a list landing may grow: a landed parse loads its dependencies, and each one
        // that parses off-thread joins the end. Each parse is taken out of the list before it lands,
        // so a load landing a parse re-entrantly (a LoadSync from a completion) cannot disturb it.
        for (usize i = 0; i < m_Parsing.size();)
        {
            InFlightParse& candidate = m_Parsing[i];
            if (!candidate.Outcome)
            {
                if (!wait && !candidate.Parse.IsReady())
                {
                    ++i;
                    continue;
                }
                candidate.Outcome = candidate.Parse.Get();
            }
            InFlightParse parse = std::move(candidate);
            m_Parsing.erase(m_Parsing.begin() + static_cast<std::ptrdiff_t>(i));
            LandParse(std::move(parse));
        }
    }

    void AssetManager::LandParse(InFlightParse parse)
    {
        Result<AssetResult<Detail::ParsedAsset>>& outcome = *parse.Outcome;
        if (!outcome)
        {
            FailLoad(parse.Id, parse.Entry, outcome.error());
            return;
        }
        if (!*outcome)
        {
            FailLoad(parse.Id, parse.Entry, outcome->error().Detail);
            return;
        }
        const Detail::ParsedAsset& parsed = **outcome;

        vector<Ref<Detail::AssetCacheEntry>> dependencies;
        dependencies.reserve(parsed.Dependencies.size());
        for (const Detail::AssetDependency& dependency : parsed.Dependencies)
        {
            Ref<Detail::AssetCacheEntry> entry = LoadUntyped(dependency.Type, dependency.Id);
            if (entry == nullptr)
            {
                FailLoad(parse.Id, parse.Entry,
                         fmt::format("dependency {} did not resolve", dependency.Id.Value));
                return;
            }
            dependencies.push_back(std::move(entry));
        }

        VE_ASSERT(parsed.Complete != nullptr, "AssetManager: asset {}'s parse set no Complete",
                  parse.Id.Value);
        AssetResult<Detail::LoadJob> job = parsed.Complete(*this, dependencies);
        if (!job)
        {
            FailLoad(parse.Id, parse.Entry, job.error().Detail);
            return;
        }
        VE_ASSERT(!job->AsyncResource.has_value(),
                  "AssetManager: asset {}'s completion yielded an AsyncResource", parse.Id.Value);
        EnqueueLoadJob(parse.Id, parse.Entry, std::move(*job));
    }

    AssetResult<Detail::LoadJob> AssetManager::RunParseInline(const AssetLoader& loader,
                                                              const AssetTypeId type,
                                                              const AssetId id)
    {
        const AssetResult<ArchiveEntry> found = FindTyped(type, id);
        if (!found)
        {
            return std::unexpected(found.error());
        }

        AssetResult<Detail::ParsedAsset> parsed =
            loader.Parse(MakeParseContext(false), id, found->Blob);
        if (!parsed)
        {
            return std::unexpected(std::move(parsed.error()));
        }

        vector<Ref<Detail::AssetCacheEntry>> dependencies;
        dependencies.reserve(parsed->Dependencies.size());
        for (const Detail::AssetDependency& dependency : parsed->Dependencies)
        {
            AssetResult<Ref<Detail::AssetCacheEntry>> entry =
                LoadSyncUntyped(dependency.Type, dependency.Id);
            if (!entry)
            {
                return std::unexpected(std::move(entry.error()));
            }
            dependencies.push_back(std::move(*entry));
        }

        VE_ASSERT(parsed->Complete != nullptr, "AssetManager: asset {}'s parse set no Complete",
                  id.Value);
        return parsed->Complete(*this, dependencies);
    }

    void AssetManager::PumpFinalizes()
    {
        // Parses that finished on a worker land first, so a load whose dependencies are already
        // resident finalizes in this same pump.
        LandParses(false);

        // Finalize every pending load whose dependencies are resident. A material whose textures
        // finalize this same pump waits one more iteration; the loop terminates because each
        // pass makes monotonic progress (at least one entry finalizes per pass).
        bool progressed = true;
        while (progressed)
        {
            progressed = false;

            for (usize i = 0; i < m_Pending.size();)
            {
                const PendingLoad& candidate = m_Pending[i];

                // A pending-Adopt keep-alive rides this list with no Finalize; its own
                // factory continuation resolves the entry, so step over it here.
                if (!candidate.Finalize)
                {
                    ++i;
                    continue;
                }

                bool depsReady = true;
                bool depFailed = false;
                for (const Ref<Detail::AssetCacheEntry>& dep : candidate.Dependencies)
                {
                    if (dep != nullptr && dep->Failed)
                    {
                        depFailed = true;
                        break;
                    }
                    if (dep == nullptr || dep->Resource == nullptr)
                    {
                        depsReady = false;
                    }
                }

                if (!depFailed && !depsReady)
                {
                    ++i;
                    continue;
                }

                // Taken out of the list before its Finalize runs, which may itself load (and so
                // pump) re-entrantly.
                PendingLoad pending = std::move(m_Pending[i]);
                m_Pending.erase(m_Pending.begin() + static_cast<std::ptrdiff_t>(i));
                progressed = true;

                if (depFailed)
                {
                    FailLoad(pending.Id, pending.Entry, "a dependency failed to load");
                    continue;
                }

                const VoidResult finalized = pending.Finalize();
                if (!finalized)
                {
                    FailLoad(pending.Id, pending.Entry, finalized.error());
                    continue;
                }

                // The decode → upload → continuation chain lands back on the main thread here;
                // the instant marks where an async load becomes resident.
                VE_PROFILE_INSTANT("Asset/Finalize");
                pending.Entry->Resource = std::move(pending.Resource);
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
        entry->Failed = true;
        std::erase_if(m_Pending,
                      [&entry](const PendingLoad& pending) { return pending.Entry == entry; });
        // An id-keyed entry (a single-phase loader's AsyncResource) leaves the cache as any failed
        // load does, so a later load of the id starts over.
        if (const auto it = m_Cache.find(entry->Id); it != m_Cache.end() && it->second == entry)
        {
            m_Cache.erase(it);
        }
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

            // The id was already Load()ed async and is still pending. A sync handle must be
            // resident, so wait out every parse still running, then drain the finalize queue (it
            // finalizes dependencies before dependents) to land it inline. The entry is held
            // rather than the iterator, which the landing's own cache inserts invalidate, and so
            // it stays observable even if the landing fails it out of the cache.
            const Ref<Detail::AssetCacheEntry> cached = it->second;
            if (cached->Resource == nullptr)
            {
                const Ref<Detail::AssetCacheEntry>& pending = cached;
                LandParses(true);
                PumpFinalizes();
                if (pending->Failed)
                {
                    return std::unexpected(AssetLoadError{
                        .Kind = AssetError::LoadFailed,
                        .Id = id,
                        .Detail = fmt::format("asset {}'s async load failed", id.Value),
                    });
                }
                if (pending->Resource == nullptr)
                {
                    return std::unexpected(AssetLoadError{
                        .Kind = AssetError::LoadFailed,
                        .Id = id,
                        .Detail = fmt::format("asset {} is still pending an async load", id.Value),
                    });
                }
            }

            return cached;
        }

        const auto loaderIt = m_Loaders.find(type);
        AssetResult<Detail::LoadJob> job =
            loaderIt != m_Loaders.end() && loaderIt->second->ParsesOffThread()
                ? RunParseInline(*loaderIt->second, type, id)
                : RunLoader(type, id, false);
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
