#pragma once

#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Veng.h>

namespace Veng
{
    /// @brief The AssetTypeId an embedded AssetHandle<T> field resolves to, keyed by the field's leaf
    ///        TypeId.
    ///
    /// The single mapping shared by the runtime prefab loader (which loads a set handle's dependency)
    /// and the cooker's prefab importer (which validates a set handle's id against the pack), so the
    /// two cannot drift out of sync. A field whose leaf TypeId is none of veng's asset-handle leaves
    /// returns nullopt — an AssetHandle of a type the engine does not recognize.
    /// @param fieldType  The reflected leaf TypeId of the AssetHandle<T> field.
    /// @return The asset type the handle resolves to, or nullopt for an unrecognized handle type.
    [[nodiscard]] inline optional<AssetTypeId> AssetTypeForHandleField(TypeId fieldType)
    {
        if (fieldType == TypeIdOf<AssetHandle<Texture>>())
        {
            return AssetTypes::Texture;
        }
        if (fieldType == TypeIdOf<AssetHandle<Mesh>>())
        {
            return AssetTypes::Mesh;
        }
        if (fieldType == TypeIdOf<AssetHandle<Material>>())
        {
            return AssetTypes::Material;
        }
        if (fieldType == TypeIdOf<AssetHandle<MaterialInstance>>())
        {
            return AssetTypes::MaterialInstance;
        }
        if (fieldType == TypeIdOf<AssetHandle<Prefab>>())
        {
            return AssetTypes::Prefab;
        }
        if (fieldType == TypeIdOf<AssetHandle<Animation>>())
        {
            return AssetTypes::Animation;
        }
        if (fieldType == TypeIdOf<AssetHandle<EnvironmentMap>>())
        {
            return AssetTypes::Environment;
        }
        if (fieldType == TypeIdOf<AssetHandle<InputMappingContext>>())
        {
            return AssetTypes::InputMap;
        }
        if (fieldType == TypeIdOf<AssetHandle<Gui::UIDocument>>())
        {
            return AssetTypes::UIDocument;
        }
        if (fieldType == TypeIdOf<AssetHandle<RawAsset>>())
        {
            return AssetTypes::Raw;
        }
        return std::nullopt;
    }
}
