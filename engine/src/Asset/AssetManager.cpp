#include <Veng/Asset/AssetManager.h>

#include <Veng/Assert.h>

#include "Loaders/MaterialLoader.h"
#include "Loaders/MeshLoader.h"
#include "Loaders/RawAssetLoader.h"
#include "Loaders/ShaderLoader.h"
#include "Loaders/TextureLoader.h"
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
    namespace
    {
        string_view ToString(AssetType type)
        {
            switch (type)
            {
                case AssetType::Raw: return "Raw";
                case AssetType::Texture: return "Texture";
                case AssetType::Mesh: return "Mesh";
                case AssetType::Shader: return "Shader";
                case AssetType::Material: return "Material";
                case AssetType::VertexLayout: return "VertexLayout";
            }

            VE_ASSERT(false, "AssetManager: unhandled AssetType {}", static_cast<u32>(type));
        }
    }

    AssetManager::AssetManager(Renderer::Context& context, const AssetManagerInfo& /*info*/) :
        m_Context(context)
    {
        RegisterLoader(CreateUnique<RawAssetLoader>());
        RegisterLoader(CreateUnique<TextureLoader>());
        RegisterLoader(CreateUnique<MeshLoader>());
        RegisterLoader(CreateUnique<ShaderLoader>());
        RegisterLoader(CreateUnique<VertexLayoutLoader>());
        RegisterLoader(CreateUnique<MaterialLoader>());

#ifdef VENG_HAS_CORE_PACK
        const VoidResult coreMount = MountBytes(
            path("<core>"),
            std::span<const u8>(g_CoreLayoutPack, static_cast<usize>(g_CoreLayoutPackSize)));
        VE_ASSERT(coreMount.has_value(), "AssetManager: failed to mount embedded core pack: {}", coreMount.error());
#endif
    }

    AssetManager::~AssetManager() = default;

    VoidResult AssetManager::Mount(const path& archive)
    {
        for (const MountedArchive& mount : m_Mounts)
        {
            if (mount.Path == archive)
                return {};
        }

        Result<ArchiveReader> reader = ArchiveReader::Open(archive);
        if (!reader)
            return std::unexpected(reader.error());

        m_Mounts.push_back(MountedArchive{.Path = archive, .Reader = std::move(*reader)});
        return {};
    }

    VoidResult AssetManager::MountBytes(const path& identity, std::span<const u8> bytes)
    {
        for (const MountedArchive& mount : m_Mounts)
        {
            if (mount.Path == identity)
                return {};
        }

        Result<ArchiveReader> reader = ArchiveReader::FromBytes(bytes);
        if (!reader)
            return std::unexpected(reader.error());

        m_Mounts.push_back(MountedArchive{.Path = identity, .Reader = std::move(*reader)});
        return {};
    }

    void AssetManager::Unmount(const path& archive)
    {
        std::erase_if(m_Mounts, [&archive](const MountedArchive& mount) { return mount.Path == archive; });
    }

    void AssetManager::CollectGarbage()
    {
        std::erase_if(m_Cache, [](const auto& entry) { return entry.second.use_count() == 1; });
    }

    void AssetManager::RegisterLoader(Unique<AssetLoader> loader)
    {
        const AssetType type = loader->Type();
        m_Loaders[type] = std::move(loader);
    }

    optional<ArchiveEntry> AssetManager::Find(AssetId id) const
    {
        for (const MountedArchive& mount : m_Mounts)
        {
            if (optional<ArchiveEntry> entry = mount.Reader.Find(id))
                return entry;
        }

        return std::nullopt;
    }

    AssetResult<Ref<Detail::AssetCacheEntry>> AssetManager::LoadSyncUntyped(AssetType type, AssetId id)
    {
        if (const auto it = m_Cache.find(id); it != m_Cache.end())
        {
            if (it->second->Type != type)
            {
                return std::unexpected(AssetLoadError{
                    .Kind = AssetError::WrongType,
                    .Id = id,
                    .Detail = fmt::format("asset {} is cached as {}, not {}", id.Value,
                        ToString(it->second->Type), ToString(type)),
                });
            }

            return it->second;
        }

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
                .Detail = fmt::format("asset {} is a {}, not {}", id.Value,
                    ToString(found->Type), ToString(type)),
            });
        }

        const auto loaderIt = m_Loaders.find(type);
        if (loaderIt == m_Loaders.end())
        {
            return std::unexpected(AssetLoadError{
                .Kind = AssetError::LoadFailed,
                .Id = id,
                .Detail = fmt::format("no loader registered for AssetType {}", ToString(type)),
            });
        }

        AssetResult<Detail::RefAny> resource = loaderIt->second->Load(*this, m_Context, id, found->Blob);
        if (!resource)
            return std::unexpected(resource.error());

        Ref<Detail::AssetCacheEntry> entry = CreateRef<Detail::AssetCacheEntry>(Detail::AssetCacheEntry{
            .Id = id,
            .Type = type,
            .Resource = std::move(*resource),
        });

        m_Cache[id] = entry;
        return entry;
    }
}
