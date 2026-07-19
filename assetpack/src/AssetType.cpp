#include <Veng/Asset/AssetType.h>

#include <Veng/Asset/HexId.h>

#include <cstdlib>

#include <fmt/format.h>

namespace Veng
{
    namespace
    {
        // assetpack carries no engine dependency, so it cannot reach VE_ASSERT. A collision is
        // an authoring error that must never reach a cook or a runtime, so it aborts here the
        // same way the reflection registry's assert does.
        [[noreturn]] void FatalCollision(const string& message)
        {
            fmt::print(stderr, "AssetTypeRegistry: {}\n", message);
            std::abort();
        }
    }

    void AssetTypeRegistry::Register(AssetTypeInfo info)
    {
        if (!info.Id.IsValid())
        {
            FatalCollision(
                fmt::format("asset type '{}' claims the reserved invalid id 0", info.Name));
        }

        if (const auto existing = m_Types.find(info.Id); existing != m_Types.end())
        {
            FatalCollision(fmt::format("AssetTypeId collision: '{}' and '{}' both claim {}",
                                       existing->second.Name, info.Name,
                                       FormatHexId(info.Id.Value)));
        }

        if (const auto existing = m_ByName.find(info.Name); existing != m_ByName.end())
        {
            FatalCollision(fmt::format(
                "asset type name collision: '{}' is claimed by both {} and {}", info.Name,
                FormatHexId(existing->second.Value), FormatHexId(info.Id.Value)));
        }

        m_ByName.emplace(info.Name, info.Id);
        m_Types.emplace(info.Id, std::move(info));
    }

    const AssetTypeInfo* AssetTypeRegistry::Find(AssetTypeId id) const
    {
        const auto it = m_Types.find(id);
        return it == m_Types.end() ? nullptr : &it->second;
    }

    optional<AssetTypeId> AssetTypeRegistry::FindByName(std::string_view name) const
    {
        const auto it = m_ByName.find(string(name));
        if (it == m_ByName.end())
        {
            return std::nullopt;
        }
        return it->second;
    }

    string AssetTypeRegistry::GetName(AssetTypeId id) const
    {
        const AssetTypeInfo* info = Find(id);
        return info == nullptr ? FormatHexId(id.Value) : info->Name;
    }

    string AssetTypeRegistry::GetDisplayName(AssetTypeId id) const
    {
        const AssetTypeInfo* info = Find(id);
        if (info == nullptr)
        {
            return FormatHexId(id.Value);
        }
        return info->DisplayName.empty() ? info->Name : info->DisplayName;
    }

    string AssetTypeRegistry::GetGlyph(AssetTypeId id) const
    {
        const AssetTypeInfo* info = Find(id);
        return info == nullptr || info->Glyph.empty() ? string{"?"} : info->Glyph;
    }

    void RegisterBuiltinAssetTypes(AssetTypeRegistry& registry)
    {
        registry.Register(
            {.Id = AssetTypes::Raw, .Name = "Raw", .DisplayName = "Raw", .Glyph = "RAW"});
        registry.Register({.Id = AssetTypes::Texture,
                           .Name = "Texture",
                           .DisplayName = "Texture",
                           .Glyph = "TEX"});
        registry.Register(
            {.Id = AssetTypes::Mesh, .Name = "Mesh", .DisplayName = "Mesh", .Glyph = "MSH"});
        registry.Register(
            {.Id = AssetTypes::Shader, .Name = "Shader", .DisplayName = "Shader", .Glyph = "SHD"});
        registry.Register({.Id = AssetTypes::Material,
                           .Name = "Material",
                           .DisplayName = "Material",
                           .Glyph = "MAT"});
        registry.Register({.Id = AssetTypes::MaterialInstance,
                           .Name = "MaterialInstance",
                           .DisplayName = "MaterialInstance",
                           .Glyph = "MTI"});
        registry.Register({.Id = AssetTypes::VertexLayout,
                           .Name = "VertexLayout",
                           .DisplayName = "VertexLayout",
                           .Glyph = "VTX"});
        registry.Register(
            {.Id = AssetTypes::Prefab, .Name = "Prefab", .DisplayName = "Prefab", .Glyph = "PFB"});
        registry.Register(
            {.Id = AssetTypes::Level, .Name = "Level", .DisplayName = "Level", .Glyph = "LVL"});
        registry.Register({.Id = AssetTypes::Skeleton,
                           .Name = "Skeleton",
                           .DisplayName = "Skeleton",
                           .Glyph = "SKL"});
        registry.Register({.Id = AssetTypes::Animation,
                           .Name = "Animation",
                           .DisplayName = "Animation",
                           .Glyph = "ANM"});
        registry.Register({.Id = AssetTypes::Environment,
                           .Name = "Environment",
                           .DisplayName = "EnvironmentMap",
                           .Glyph = "ENV"});
        registry.Register({.Id = AssetTypes::InputMap,
                           .Name = "InputMap",
                           .DisplayName = "InputMap",
                           .Glyph = "INP"});
        registry.Register(
            {.Id = AssetTypes::Font, .Name = "Font", .DisplayName = "Font", .Glyph = "FNT"});
        registry.Register({.Id = AssetTypes::StyleSheet,
                           .Name = "StyleSheet",
                           .DisplayName = "StyleSheet",
                           .Glyph = "USS"});
        registry.Register({.Id = AssetTypes::UIDocument,
                           .Name = "UIDocument",
                           .DisplayName = "UIDocument",
                           .Glyph = "VUI"});
        registry.Register({.Id = AssetTypes::TableSchema,
                           .Name = "TableSchema",
                           .DisplayName = "TableSchema",
                           .Glyph = "TSC"});
        registry.Register({.Id = AssetTypes::DataTable,
                           .Name = "DataTable",
                           .DisplayName = "DataTable",
                           .Glyph = "TBL"});
    }
}
