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

    // The cached, immutable cooked-prefab asset: a serialized recipe of entities,
    // each carrying components keyed by their stable TypeId, each component's
    // field values held as the reflection serializer's name-keyed record (exactly
    // the bytes the cooker wrote with WriteFields — no per-component
    // deserialization at load). The prefab also keeps its embedded AssetHandle
    // dependencies resident (a MeshRenderer's mesh, a Material, ...), resolved as
    // ordinary load-time dependencies like a Material's textures.
    //
    // Prefab is CPU data — a Ref<T> with no GPU resource. It loads through the
    // identical AssetManager::Load path as every other asset; you create a Scene
    // and SpawnInto it.
    class Prefab
    {
    public:
        // One component on a prefab entity: its TypeId plus the WriteFields record
        // bytes that populate it at spawn.
        struct Component
        {
            TypeId Type = InvalidTypeId;
            vector<u8> Record;
        };

        // One prefab entity: the components it carries, in authored order.
        struct PrefabEntity
        {
            vector<Component> Components;
        };

        static Ref<Prefab> Create(vector<PrefabEntity> entities,
                                  vector<Ref<Detail::AssetCacheEntry>> dependencies);

        // Spawn this prefab's entities/components into `scene`. Returns the spawned
        // root entities (those with no in-prefab Parent), in authoring order.
        // Remaps intra-prefab Entity references to the freshly-created handles and
        // rehydrates AssetHandle fields via `manager`. Spawning twice spawns twice —
        // a prefab is a reusable recipe, not a singleton.
        [[nodiscard]] vector<Entity> SpawnInto(Scene& scene, AssetManager& manager) const;

        [[nodiscard]] const vector<PrefabEntity>& Entities() const { return m_Entities; }

    private:
        Prefab(vector<PrefabEntity> entities, vector<Ref<Detail::AssetCacheEntry>> dependencies);

        vector<PrefabEntity> m_Entities;
        // The resolved embedded handles' cache entries, kept resident for the
        // prefab's lifetime so SpawnInto's rehydration is a cheap cache lookup. The
        // spawned component's typed handle is resolved through the manager at spawn.
        vector<Ref<Detail::AssetCacheEntry>> m_Dependencies;
    };

    template <>
    struct AssetTypeTrait<Prefab>
    {
        static constexpr AssetType Type = AssetType::Prefab;
    };
}
