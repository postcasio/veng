#pragma once

#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Veng.h>

namespace Veng
{
    /// @brief The AssetType an embedded AssetHandle<T> field resolves to, keyed by the field's leaf
    ///        TypeId.
    ///
    /// The single mapping shared by the runtime prefab loader (which loads a set handle's dependency)
    /// and the cooker's prefab importer (which validates a set handle's id against the pack), so the
    /// two cannot drift out of sync. A field whose leaf TypeId is none of veng's asset-handle leaves
    /// returns nullopt — an AssetHandle of a type the engine does not recognize.
    /// @param fieldType  The reflected leaf TypeId of the AssetHandle<T> field.
    /// @return The asset type the handle resolves to, or nullopt for an unrecognized handle type.
    [[nodiscard]] inline optional<AssetType> AssetTypeForHandleField(TypeId fieldType)
    {
        if (fieldType == TypeIdOf<AssetHandle<Texture>>())
        {
            return AssetType::Texture;
        }
        if (fieldType == TypeIdOf<AssetHandle<Mesh>>())
        {
            return AssetType::Mesh;
        }
        if (fieldType == TypeIdOf<AssetHandle<Material>>())
        {
            return AssetType::Material;
        }
        if (fieldType == TypeIdOf<AssetHandle<MaterialInstance>>())
        {
            return AssetType::MaterialInstance;
        }
        if (fieldType == TypeIdOf<AssetHandle<Prefab>>())
        {
            return AssetType::Prefab;
        }
        if (fieldType == TypeIdOf<AssetHandle<Animation>>())
        {
            return AssetType::Animation;
        }
        if (fieldType == TypeIdOf<AssetHandle<EnvironmentMap>>())
        {
            return AssetType::Environment;
        }
        if (fieldType == TypeIdOf<AssetHandle<InputMappingContext>>())
        {
            return AssetType::InputMap;
        }
        if (fieldType == TypeIdOf<AssetHandle<Gui::UIDocument>>())
        {
            return AssetType::UIDocument;
        }
        if (fieldType == TypeIdOf<AssetHandle<RawAsset>>())
        {
            return AssetType::Raw;
        }
        return std::nullopt;
    }
}
