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
    }

    AssetResult<Detail::ParsedAsset>
    MaterialInstanceLoader::Parse(const AssetParseContext& /*context*/, const AssetId id,
                                  const std::span<const u8> cooked) const
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

        // The parent is the first dependency and each texture override's texture follows it, in
        // override order; a value override carries its bytes and no dependency.
        Detail::ParsedAsset parsed;
        parsed.Dependencies.push_back(
            {.Type = AssetTypes::Material, .Id = AssetId{header.ParentId}});

        auto overrides = CreateRef<vector<MaterialOverride>>();
        overrides->reserve(header.OverrideCount);
        // The overrides that take a texture, in order; the k-th takes dependency k + 1.
        vector<usize> textureOverrides;
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
                overrides->push_back(MaterialOverride{
                    .Name = BridgeName(co.Name), .Value = std::move(value), .Texture = {}});
            }
            else if (co.Kind == 1)
            {
                parsed.Dependencies.push_back(
                    {.Type = AssetTypes::Texture, .Id = AssetId{co.TextureId}});
                // The texture is filled in by the completion, from this dependency's entry.
                textureOverrides.push_back(overrides->size());
                overrides->push_back(
                    MaterialOverride{.Name = BridgeName(co.Name), .Value = {}, .Texture = {}});
            }
            else
            {
                return std::unexpected(Corrupt(
                    id, fmt::format("material instance: override '{}' has unrecognized Kind {}",
                                    BridgeName(co.Name), co.Kind)));
            }
        }

        parsed.Complete = [overrides, textureOverrides = std::move(textureOverrides),
                           id](AssetManager& manager,
                               std::span<const Ref<Detail::AssetCacheEntry>> resolved)
            -> AssetResult<Detail::LoadJob>
        {
            for (usize k = 0; k < textureOverrides.size(); ++k)
            {
                (*overrides)[textureOverrides[k]].Texture =
                    AssetManager::HandleOf<Texture>(resolved[k + 1]);
            }

            const MaterialInstanceInfo info{
                .Name = fmt::format("MaterialInstance {}", id.Value),
                .Context = &manager.GetContext(),
                .Parent = AssetManager::HandleOf<Material>(resolved[0]),
                .Overrides = std::move(*overrides),
            };
            const Ref<MaterialInstance> instance = MaterialInstance::Prepare(info);

            return Detail::LoadJob{
                .Resource = Detail::RefAny(instance),
                .Dependencies =
                    vector<Ref<Detail::AssetCacheEntry>>(resolved.begin(), resolved.end()),
                .Finalize = [instance]() -> VoidResult
                {
                    instance->Finalize();
                    return {};
                },
            };
        };
        return parsed;
    }
}
