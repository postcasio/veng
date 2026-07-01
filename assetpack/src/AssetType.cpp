#include <Veng/Asset/AssetType.h>

namespace Veng
{
    const char* ToString(AssetType type)
    {
        switch (type)
        {
        case AssetType::Raw:
            return "raw";
        case AssetType::Texture:
            return "texture";
        case AssetType::Mesh:
            return "mesh";
        case AssetType::Shader:
            return "shader";
        case AssetType::Material:
            return "material";
        case AssetType::MaterialInstance:
            return "material_instance";
        case AssetType::VertexLayout:
            return "vertex_layout";
        case AssetType::Prefab:
            return "prefab";
        case AssetType::Level:
            return "level";
        case AssetType::Skeleton:
            return "skeleton";
        case AssetType::Animation:
            return "animation";
        case AssetType::Environment:
            return "environment";
        }
        return "unknown";
    }

    optional<AssetType> ParseAssetType(std::string_view name)
    {
        if (name == "raw")
        {
            return AssetType::Raw;
        }
        if (name == "texture")
        {
            return AssetType::Texture;
        }
        if (name == "mesh")
        {
            return AssetType::Mesh;
        }
        if (name == "shader")
        {
            return AssetType::Shader;
        }
        if (name == "material")
        {
            return AssetType::Material;
        }
        if (name == "material_instance")
        {
            return AssetType::MaterialInstance;
        }
        if (name == "vertex_layout")
        {
            return AssetType::VertexLayout;
        }
        if (name == "prefab")
        {
            return AssetType::Prefab;
        }
        if (name == "level")
        {
            return AssetType::Level;
        }
        if (name == "skeleton")
        {
            return AssetType::Skeleton;
        }
        if (name == "animation")
        {
            return AssetType::Animation;
        }
        if (name == "environment")
        {
            return AssetType::Environment;
        }
        return std::nullopt;
    }
}
