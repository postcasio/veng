#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Reflection/TypeId.h>

#include <span>

namespace Veng
{
    class Scene;
    class AssetManager;

    /// @brief Cached, immutable cooked-prefab asset: a serialized recipe of entities and components.
    ///
    /// Each entity carries components keyed by their stable TypeId; each component's field values
    /// are held as the reflection serializer's name-keyed WriteFields record (no per-component
    /// deserialization at load). Embedded AssetHandle dependencies are resolved as ordinary
    /// load-time dependencies and kept resident for the prefab's lifetime.
    ///
    /// Prefab is CPU data with no GPU resource. Load it through AssetManager::Load like any other
    /// asset, create a Scene, and call SpawnInto.
    class Prefab
    {
    public:
        /// @brief One component on a prefab entity: its TypeId and the WriteFields record bytes.
        struct Component
        {
            /// @brief The component's stable type identity.
            TypeId Type = InvalidTypeId;
            /// @brief Name-keyed reflection record used to populate the component at spawn.
            vector<u8> Record;
        };

        /// @brief One prefab entity and the components it carries, in authored order.
        struct PrefabEntity
        {
            vector<Component> Components;
        };

        /// @brief Creates a Prefab from a list of entities and their resolved dependency entries.
        static Ref<Prefab> Create(vector<PrefabEntity> entities,
                                  vector<Ref<Detail::AssetCacheEntry>> dependencies);

        /// @brief Spawns this prefab's entities and components into `scene`.
        ///
        /// Returns the spawned root entities (those with no in-prefab Parent), in authoring order.
        /// Remaps intra-prefab Entity references to freshly created handles and rehydrates
        /// AssetHandle fields via `manager`. Spawning twice produces two independent copies.
        [[nodiscard]] vector<Entity> SpawnInto(Scene& scene, AssetManager& manager) const;

        /// @brief Returns the list of prefab entities.
        [[nodiscard]] const vector<PrefabEntity>& Entities() const { return m_Entities; }

    private:
        Prefab(vector<PrefabEntity> entities, vector<Ref<Detail::AssetCacheEntry>> dependencies);

        vector<PrefabEntity> m_Entities;
        /// @brief Resolved embedded-handle cache entries, kept resident so SpawnInto's rehydration is a cheap cache lookup.
        vector<Ref<Detail::AssetCacheEntry>> m_Dependencies;
    };

    /// @brief AssetTypeTrait specialization mapping Prefab to AssetType::Prefab.
    template <>
    struct AssetTypeTrait<Prefab>
    {
        /// @brief The asset type tag for Prefab.
        static constexpr AssetType Type = AssetType::Prefab;
    };
}
