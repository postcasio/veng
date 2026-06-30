#include "MaterialInstanceLoader.h"

#include <cstring>

#include <fmt/format.h>

#include <Veng/Assert.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/Texture.h>
#include <Veng/Renderer/Context.h>

namespace Veng
{
    namespace
    {
        // Cooked names are fixed-size, nul-terminated char arrays (CookedBlobs.h).
        template <usize N>
        string BridgeName(const char (&name)[N])
        {
            return string(name, strnlen(name, N));
        }

        AssetLoadError Corrupt(AssetId id, string detail)
        {
            return AssetLoadError{
                .Kind = AssetError::Corrupt, .Id = id, .Detail = std::move(detail)};
        }

        // Resolve the parent Material handle (async or blocking), recording its cache entry.
        AssetResult<AssetHandle<Material>> LoadParent(AssetManager& manager, AssetId instanceId,
                                                      u64 parentId, bool async)
        {
            if (async)
            {
                AssetHandle<Material> handle = manager.Load<Material>(AssetId{parentId});
                if (!AssetManager::EntryOf(handle))
                {
                    return std::unexpected(AssetLoadError{
                        .Kind = AssetError::MissingDependency,
                        .Id = AssetId{parentId},
                        .Detail = fmt::format("instance {}: parent material {} did not resolve",
                                              instanceId.Value, parentId)});
                }
                return handle;
            }
            return manager.LoadSync<Material>(AssetId{parentId});
        }

        // Resolve an override texture handle (async or blocking).
        AssetResult<AssetHandle<Texture>>
        LoadOverrideTexture(AssetManager& manager, AssetId instanceId, u64 textureId, bool async)
        {
            if (async)
            {
                AssetHandle<Texture> handle = manager.Load<Texture>(AssetId{textureId});
                if (!AssetManager::EntryOf(handle))
                {
                    return std::unexpected(AssetLoadError{
                        .Kind = AssetError::MissingDependency,
                        .Id = AssetId{textureId},
                        .Detail = fmt::format("instance {}: override texture {} did not resolve",
                                              instanceId.Value, textureId)});
                }
                return handle;
            }
            return manager.LoadSync<Texture>(AssetId{textureId});
        }
    }

    AssetResult<Detail::LoadJob>
    MaterialInstanceLoader::Load(AssetManager& manager, Renderer::Context& context,
                                 TaskSystem& /*tasks*/, TypeRegistry& /*types*/, AssetId id,
                                 std::span<const u8> cooked, bool async) const
    {
        if (cooked.size() < sizeof(CookedMaterialInstanceHeader))
        {
            return std::unexpected(
                Corrupt(id, "material instance: cooked blob smaller than header"));
        }

        CookedMaterialInstanceHeader header;
        std::memcpy(&header, cooked.data(), sizeof(header));

        if (header.Version != CookedMaterialInstanceVersion)
        {
            return std::unexpected(Corrupt(
                id, fmt::format("material instance: blob version {} does not match version {} — "
                                "re-cook the pack",
                                header.Version, CookedMaterialInstanceVersion)));
        }

        usize cursor = sizeof(CookedMaterialInstanceHeader);

        const usize overrideBytes =
            static_cast<usize>(header.OverrideCount) * sizeof(CookedMaterialInstanceOverride);
        if (cooked.size() < cursor + overrideBytes + header.ValueRegionBytes)
        {
            return std::unexpected(Corrupt(id, "material instance: cooked blob truncated"));
        }

        vector<CookedMaterialInstanceOverride> cookedOverrides(header.OverrideCount);
        for (u32 i = 0; i < header.OverrideCount; ++i)
        {
            std::memcpy(&cookedOverrides[i],
                        cooked.data() + cursor + i * sizeof(CookedMaterialInstanceOverride),
                        sizeof(CookedMaterialInstanceOverride));
        }
        cursor += overrideBytes;

        const std::span<const u8> valueRegion = cooked.subspan(cursor, header.ValueRegionBytes);

        // Resolve the parent.
        const AssetResult<AssetHandle<Material>> parentResult =
            LoadParent(manager, id, header.ParentId, async);
        if (!parentResult)
        {
            return std::unexpected(parentResult.error());
        }
        const AssetHandle<Material> parent = *parentResult;

        vector<Ref<Detail::AssetCacheEntry>> dependencies;
        dependencies.push_back(AssetManager::EntryOf(parent));

        // Build the override records, fanning out texture sub-loads.
        vector<MaterialOverride> overrides;
        overrides.reserve(header.OverrideCount);
        for (u32 i = 0; i < header.OverrideCount; ++i)
        {
            const CookedMaterialInstanceOverride& co = cookedOverrides[i];

            if (co.Kind == 0)
            {
                if (static_cast<usize>(co.ValueOffset) + co.ValueSize > header.ValueRegionBytes)
                {
                    return std::unexpected(Corrupt(
                        id,
                        fmt::format("material instance: override '{}' value range out of bounds",
                                    BridgeName(co.Name))));
                }
                vector<std::byte> value(co.ValueSize);
                if (co.ValueSize > 0)
                {
                    std::memcpy(value.data(), valueRegion.data() + co.ValueOffset, co.ValueSize);
                }
                overrides.push_back(MaterialOverride{
                    .Name = BridgeName(co.Name), .Value = std::move(value), .Texture = {}});
            }
            else if (co.Kind == 1)
            {
                const AssetResult<AssetHandle<Texture>> texResult =
                    LoadOverrideTexture(manager, id, co.TextureId, async);
                if (!texResult)
                {
                    return std::unexpected(texResult.error());
                }
                dependencies.push_back(AssetManager::EntryOf(*texResult));
                overrides.push_back(MaterialOverride{
                    .Name = BridgeName(co.Name), .Value = {}, .Texture = *texResult});
            }
            else
            {
                return std::unexpected(Corrupt(
                    id, fmt::format("material instance: override '{}' has unrecognized Kind {}",
                                    BridgeName(co.Name), co.Kind)));
            }
        }

        const MaterialInstanceInfo info{
            .Name = fmt::format("MaterialInstance {}", id.Value),
            .Context = &context,
            .Parent = parent,
            .Overrides = std::move(overrides),
        };

        const Ref<MaterialInstance> instance = MaterialInstance::Prepare(info);

        return Detail::LoadJob{
            .Resource = Detail::RefAny(instance),
            .Dependencies = std::move(dependencies),
            .Finalize = [instance]() -> VoidResult
            {
                instance->Finalize();
                return {};
            },
        };
    }
}
