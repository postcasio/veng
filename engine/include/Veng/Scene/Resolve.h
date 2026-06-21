#pragma once

#include <Veng/Veng.h>

namespace Veng
{
    class Scene;
    class AssetManager;
    struct Entity;

    /// @brief Fires every resolver-bearing component on `entity`, generating/assigning
    ///        its derived resources.
    ///
    /// Enumerates the entity's components and calls each non-null
    /// TypeInfo::SpawnResolve. The single resolve code path: Prefab::SpawnInto runs the
    /// same firing per spawned entity, and a runtime caller (the editor's add/edit, game
    /// code) calls this directly after adding or editing a component that carries a
    /// resolver. In effect idempotent for an unchanged recipe — it rebuilds and reassigns.
    /// @param scene    The scene holding the entity.
    /// @param entity   The entity whose components are resolved; must be alive.
    /// @param manager  The asset manager a resolver reaches derived resources through.
    void ResolveComponents(Scene& scene, Entity entity, AssetManager& manager);
}
