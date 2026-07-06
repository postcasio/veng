#include <Veng/Asset/AssetType.h>

namespace Veng
{
    const char* ToString(AssetType type)
    {
        switch (type)
        {
        case AssetType::Raw:
            return "Raw";
        case AssetType::Texture:
            return "Texture";
        case AssetType::Mesh:
            return "Mesh";
        case AssetType::Shader:
            return "Shader";
        case AssetType::Material:
            return "Material";
        case AssetType::MaterialInstance:
            return "MaterialInstance";
        case AssetType::VertexLayout:
            return "VertexLayout";
        case AssetType::Prefab:
            return "Prefab";
        case AssetType::Level:
            return "Level";
        case AssetType::Skeleton:
            return "Skeleton";
        case AssetType::Animation:
            return "Animation";
        case AssetType::Environment:
            return "Environment";
        case AssetType::InputMap:
            return "InputMap";
        case AssetType::Font:
            return "Font";
        case AssetType::StyleSheet:
            return "StyleSheet";
        case AssetType::UIDocument:
            return "UIDocument";
        }
        return "unknown";
    }

    optional<AssetType> ParseAssetType(std::string_view name)
    {
        if (name == "Raw")
        {
            return AssetType::Raw;
        }
        if (name == "Texture")
        {
            return AssetType::Texture;
        }
        if (name == "Mesh")
        {
            return AssetType::Mesh;
        }
        if (name == "Shader")
        {
            return AssetType::Shader;
        }
        if (name == "Material")
        {
            return AssetType::Material;
        }
        if (name == "MaterialInstance")
        {
            return AssetType::MaterialInstance;
        }
        if (name == "VertexLayout")
        {
            return AssetType::VertexLayout;
        }
        if (name == "Prefab")
        {
            return AssetType::Prefab;
        }
        if (name == "Level")
        {
            return AssetType::Level;
        }
        if (name == "Skeleton")
        {
            return AssetType::Skeleton;
        }
        if (name == "Animation")
        {
            return AssetType::Animation;
        }
        if (name == "Environment")
        {
            return AssetType::Environment;
        }
        if (name == "InputMap")
        {
            return AssetType::InputMap;
        }
        if (name == "Font")
        {
            return AssetType::Font;
        }
        if (name == "StyleSheet")
        {
            return AssetType::StyleSheet;
        }
        if (name == "UIDocument")
        {
            return AssetType::UIDocument;
        }
        return std::nullopt;
    }
}
