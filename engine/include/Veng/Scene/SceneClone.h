#pragma once

#include <Veng/Veng.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Reflection/TypeRegistry.h>

namespace Veng
{
    /// @brief Remaps an old/source Entity to the freshly created one, or Entity::Null when unmapped.
    ///
    /// Prefab spawning maps a prefab-local index; scene cloning maps a live source
    /// Entity. Either returns Entity::Null for the null sentinel and for a value
    /// outside the mapped set.
    using EntityRemap = function<Entity(Entity)>;

    /// @brief Visits an AssetHandle field's bytes during the post-deserialize fix-up walk.
    ///
    /// The field pointer addresses a type-erased AssetHandle whose leading AssetId
    /// the deserializer already wrote; the handler rehydrates or replaces it as the
    /// caller's path requires (load-time dependency lookup for a prefab, direct
    /// handle copy for a clone).
    /// @param fieldPtr  Pointer to the AssetHandle field within the destination component.
    using AssetHandleFixup = function<void(void* fieldPtr)>;

    /// @brief Walks a populated component's fields, remapping Entity references and visiting AssetHandle fields.
    ///
    /// One recursion shared by Prefab::SpawnInto and Scene::Clone. After a component
    /// is deserialized, its FieldClass::Reference fields still hold source-space
    /// Entity handles: each is rewritten through `remap`. FieldClass::AssetHandle
    /// fields are handed to `assetHandle` for path-specific rehydration. Struct and
    /// the active Variant alternative recurse so a nested reference or embedded
    /// handle is reached. Other field classes are left untouched.
    /// @param obj         Pointer to the component (or nested struct) being fixed up.
    /// @param type        TypeInfo carrying the field descriptors to walk.
    /// @param registry    Registry used to resolve nested struct/variant field types.
    /// @param remap       Maps a source Entity reference to the destination Entity.
    /// @param assetHandle Visits each AssetHandle field for path-specific rehydration.
    VE_API void RemapComponentReferences(void* obj, const TypeInfo& type,
                                         const TypeRegistry& registry, const EntityRemap& remap,
                                         const AssetHandleFixup& assetHandle);
}
