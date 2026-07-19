#pragma once

#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Veng.h>

namespace Veng
{
    // AssetTypeInfo::HandleFieldType is what every consumer resolves an AssetHandle<T> field
    // through, and assetpack must spell the builtins' leaf ids as literals — it carries no
    // reflection dependency and the veng-free core-pack bootstrap registers builtins too. These
    // bind those literals to the VE_LEAF authorings, so the two cannot drift apart unnoticed.
    static_assert(AssetHandleFieldTypes::Raw == TypeIdOf<AssetHandle<RawAsset>>());
    static_assert(AssetHandleFieldTypes::Texture == TypeIdOf<AssetHandle<Texture>>());
    static_assert(AssetHandleFieldTypes::Mesh == TypeIdOf<AssetHandle<Mesh>>());
    static_assert(AssetHandleFieldTypes::Material == TypeIdOf<AssetHandle<Material>>());
    static_assert(AssetHandleFieldTypes::MaterialInstance ==
                  TypeIdOf<AssetHandle<MaterialInstance>>());
    static_assert(AssetHandleFieldTypes::Prefab == TypeIdOf<AssetHandle<Prefab>>());
    static_assert(AssetHandleFieldTypes::Animation == TypeIdOf<AssetHandle<Animation>>());
    static_assert(AssetHandleFieldTypes::Environment == TypeIdOf<AssetHandle<EnvironmentMap>>());
    static_assert(AssetHandleFieldTypes::InputMap == TypeIdOf<AssetHandle<InputMappingContext>>());
    static_assert(AssetHandleFieldTypes::UIDocument == TypeIdOf<AssetHandle<Gui::UIDocument>>());

    /// @brief Whether an asset of type @p actual may fill an AssetHandle field expecting @p expected.
    ///
    /// The one substitution rule the engine allows: a MaterialInstance field accepts a bare
    /// Material id, resolved at load to that material's zero-override default instance. Shared by
    /// every cook-time handle validation so no importer invents a second answer.
    /// @param expected  The asset type the handle field resolves to.
    /// @param actual    The asset type the referenced id was declared as.
    /// @return True when the reference is acceptable.
    [[nodiscard]] inline bool AssetHandleFieldAccepts(const AssetTypeId expected,
                                                      const AssetTypeId actual)
    {
        if (actual == expected)
        {
            return true;
        }
        return expected == AssetTypes::MaterialInstance && actual == AssetTypes::Material;
    }
}
