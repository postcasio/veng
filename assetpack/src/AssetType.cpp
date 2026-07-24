#include <Veng/Asset/AssetType.h>

#include <Veng/Asset/HexId.h>

#include <cstdlib>
#include <memory>
#include <utility>

#include <fmt/format.h>

namespace Veng
{
    /// @brief The registry's lookup tables, so no header-including TU instantiates them.
    struct AssetTypeRegistry::Impl
    {
        /// @brief Registered types keyed by id.
        std::unordered_map<AssetTypeId, AssetTypeInfo> Types;
        /// @brief Canonical-name index into Types, so a manifest name resolves in one lookup.
        std::unordered_map<string, AssetTypeId> ByName;
        /// @brief Handle-leaf-TypeId index into Types, so a reflected field resolves in one lookup.
        std::unordered_map<u64, AssetTypeId> ByHandleField;
    };

    AssetTypeRegistry::AssetTypeRegistry() : m_Impl(std::make_unique<Impl>()) {}

    AssetTypeRegistry::~AssetTypeRegistry() = default;

    AssetTypeRegistry::AssetTypeRegistry(AssetTypeRegistry&& other) noexcept = default;

    AssetTypeRegistry& AssetTypeRegistry::operator=(AssetTypeRegistry&& other) noexcept = default;

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

        if (const auto existing = m_Impl->Types.find(info.Id); existing != m_Impl->Types.end())
        {
            FatalCollision(fmt::format("AssetTypeId collision: '{}' and '{}' both claim {}",
                                       existing->second.Name, info.Name,
                                       FormatHexId(info.Id.Value)));
        }

        if (const auto existing = m_Impl->ByName.find(info.Name); existing != m_Impl->ByName.end())
        {
            FatalCollision(fmt::format(
                "asset type name collision: '{}' is claimed by both {} and {}", info.Name,
                FormatHexId(existing->second.Value), FormatHexId(info.Id.Value)));
        }

        if (info.HandleFieldType != 0)
        {
            if (const auto existing = m_Impl->ByHandleField.find(info.HandleFieldType);
                existing != m_Impl->ByHandleField.end())
            {
                FatalCollision(fmt::format("asset handle leaf type {} is claimed by both {} and {}",
                                           FormatHexId(info.HandleFieldType),
                                           FormatHexId(existing->second.Value),
                                           FormatHexId(info.Id.Value)));
            }
            m_Impl->ByHandleField.emplace(info.HandleFieldType, info.Id);
        }

        m_Impl->ByName.emplace(info.Name, info.Id);
        m_Impl->Types.emplace(info.Id, std::move(info));
    }

    const AssetTypeInfo* AssetTypeRegistry::Find(AssetTypeId id) const
    {
        const auto it = m_Impl->Types.find(id);
        return it == m_Impl->Types.end() ? nullptr : &it->second;
    }

    optional<AssetTypeId> AssetTypeRegistry::FindByName(std::string_view name) const
    {
        const auto it = m_Impl->ByName.find(string(name));
        if (it == m_Impl->ByName.end())
        {
            return std::nullopt;
        }
        return it->second;
    }

    optional<AssetTypeId> AssetTypeRegistry::FindByHandleField(u64 handleFieldType) const
    {
        const auto it = m_Impl->ByHandleField.find(handleFieldType);
        if (it == m_Impl->ByHandleField.end())
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

    bool AssetTypeRegistry::IsRegistered(AssetTypeId id) const
    {
        return m_Impl->Types.contains(id);
    }

    const std::unordered_map<AssetTypeId, AssetTypeInfo>& AssetTypeRegistry::All() const
    {
        return m_Impl->Types;
    }

    void RegisterBuiltinAssetTypes(AssetTypeRegistry& registry)
    {
        registry.Register({.Id = AssetTypes::Raw,
                           .Name = "Raw",
                           .DisplayName = "Raw",
                           .Glyph = "RAW",
                           .HandleFieldType = AssetHandleFieldTypes::Raw});
        registry.Register({.Id = AssetTypes::Texture,
                           .Name = "Texture",
                           .DisplayName = "Texture",
                           .Glyph = "TEX",
                           .HandleFieldType = AssetHandleFieldTypes::Texture});
        registry.Register({.Id = AssetTypes::Mesh,
                           .Name = "Mesh",
                           .DisplayName = "Mesh",
                           .Glyph = "MSH",
                           .HandleFieldType = AssetHandleFieldTypes::Mesh});
        registry.Register(
            {.Id = AssetTypes::Shader, .Name = "Shader", .DisplayName = "Shader", .Glyph = "SHD"});
        registry.Register({.Id = AssetTypes::Material,
                           .Name = "Material",
                           .DisplayName = "Material",
                           .Glyph = "MAT",
                           .HandleFieldType = AssetHandleFieldTypes::Material});
        registry.Register({.Id = AssetTypes::MaterialInstance,
                           .Name = "MaterialInstance",
                           .DisplayName = "MaterialInstance",
                           .Glyph = "MTI",
                           .HandleFieldType = AssetHandleFieldTypes::MaterialInstance});
        registry.Register({.Id = AssetTypes::VertexLayout,
                           .Name = "VertexLayout",
                           .DisplayName = "VertexLayout",
                           .Glyph = "VTX"});
        registry.Register({.Id = AssetTypes::Prefab,
                           .Name = "Prefab",
                           .DisplayName = "Prefab",
                           .Glyph = "PFB",
                           .HandleFieldType = AssetHandleFieldTypes::Prefab});
        registry.Register({.Id = AssetTypes::Level,
                           .Name = "Level",
                           .DisplayName = "Level",
                           .Glyph = "LVL",
                           .HandleFieldType = AssetHandleFieldTypes::Level});
        registry.Register({.Id = AssetTypes::Skeleton,
                           .Name = "Skeleton",
                           .DisplayName = "Skeleton",
                           .Glyph = "SKL",
                           .HandleFieldType = AssetHandleFieldTypes::Skeleton});
        registry.Register({.Id = AssetTypes::Animation,
                           .Name = "Animation",
                           .DisplayName = "Animation",
                           .Glyph = "ANM",
                           .HandleFieldType = AssetHandleFieldTypes::Animation});
        registry.Register({.Id = AssetTypes::Environment,
                           .Name = "Environment",
                           .DisplayName = "EnvironmentMap",
                           .Glyph = "ENV",
                           .HandleFieldType = AssetHandleFieldTypes::Environment});
        registry.Register({.Id = AssetTypes::InputMap,
                           .Name = "InputMap",
                           .DisplayName = "InputMap",
                           .Glyph = "INP",
                           .HandleFieldType = AssetHandleFieldTypes::InputMap});
        registry.Register({.Id = AssetTypes::Font,
                           .Name = "Font",
                           .DisplayName = "Font",
                           .Glyph = "FNT",
                           .HandleFieldType = AssetHandleFieldTypes::Font});
        registry.Register({.Id = AssetTypes::StyleSheet,
                           .Name = "StyleSheet",
                           .DisplayName = "StyleSheet",
                           .Glyph = "USS",
                           .HandleFieldType = AssetHandleFieldTypes::StyleSheet});
        registry.Register({.Id = AssetTypes::UIDocument,
                           .Name = "UIDocument",
                           .DisplayName = "UIDocument",
                           .Glyph = "VUI",
                           .HandleFieldType = AssetHandleFieldTypes::UIDocument});
        registry.Register({.Id = AssetTypes::TableSchema,
                           .Name = "TableSchema",
                           .DisplayName = "TableSchema",
                           .Glyph = "TSC",
                           .HandleFieldType = AssetHandleFieldTypes::TableSchema});
        registry.Register({.Id = AssetTypes::DataTable,
                           .Name = "DataTable",
                           .DisplayName = "DataTable",
                           .Glyph = "TBL",
                           .HandleFieldType = AssetHandleFieldTypes::DataTable});
        registry.Register({.Id = AssetTypes::CollisionShape,
                           .Name = "CollisionShape",
                           .DisplayName = "CollisionShape",
                           .Glyph = "COL",
                           .HandleFieldType = AssetHandleFieldTypes::CollisionShape});
    }
}
