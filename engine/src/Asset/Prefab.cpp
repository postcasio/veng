#include <Veng/Asset/Prefab.h>

#include <Veng/Assert.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Scene/Scene.h>

#include <cstring>

namespace Veng
{
    namespace
    {
        // Post-ReadFields pass: remap Reference fields (prefab-local index → spawned Entity)
        // and rehydrate AssetHandle fields (raw AssetId → cache entry). Recurses into structs.
        void Resolve(void* obj, const TypeInfo& type, const TypeRegistry& registry,
                     const vector<Entity>& spawned, AssetManager& manager)
        {
            for (const FieldDescriptor& field : type.Fields)
            {
                void* fieldPtr = static_cast<u8*>(obj) + field.Offset;

                switch (field.Class)
                {
                case FieldClass::Reference:
                {
                    Entity& entity = *static_cast<Entity*>(fieldPtr);

                    // The null sentinel stays null; never a valid prefab-local
                    // index, so it cannot collide with an intra-prefab ref.
                    if (entity.Index == Entity::InvalidIndex)
                    {
                        entity = Entity::Null;
                        break;
                    }

                    // The cook bounds-checked the index against the entity
                    // count, so an out-of-range value here is corruption past
                    // the validated cook — fatal.
                    VE_ASSERT(
                        entity.Index < spawned.size(),
                        "Prefab::SpawnInto: entity reference index {} out of range ({} entities)",
                        entity.Index, spawned.size());

                    entity = spawned[entity.Index];
                    break;
                }

                case FieldClass::AssetHandle:
                {
                    // ReadFields wrote the raw AssetId at offset 0; the cache
                    // entry is null. Resolve the id to the prefab's
                    // already-resident dependency entry (loaded at load time).
                    // An invalid id is the "no asset" value — leave it empty.
                    AssetId id{};
                    std::memcpy(&id, fieldPtr, sizeof(id));
                    if (id.IsValid())
                    {
                        const Ref<Detail::AssetCacheEntry> entry = manager.CachedEntry(id);
                        VE_ASSERT(entry != nullptr,
                                  "Prefab::SpawnInto: embedded asset {} is not resident — "
                                  "the prefab loader should have loaded it as a dependency",
                                  id.Value);
                        Detail::RehydrateHandleField(fieldPtr, id, entry);
                    }
                    break;
                }

                case FieldClass::Struct:
                {
                    const TypeInfo& nested = registry.Info(field.Type);
                    Resolve(fieldPtr, nested, registry, spawned, manager);
                    break;
                }

                case FieldClass::Variant:
                {
                    // Descend into the active alternative so an AssetHandle/Reference
                    // field inside it rehydrates like one in a nested struct; an empty
                    // variant has nothing to resolve.
                    const TypeInfo& info = registry.Info(field.Type);
                    void* memberPtr = info.VariantActivePtr(fieldPtr);
                    if (memberPtr != nullptr)
                    {
                        const TypeId active = info.VariantActiveType(fieldPtr);
                        Resolve(memberPtr, registry.Info(active), registry, spawned, manager);
                    }
                    break;
                }

                default:
                    break;
                }
            }
        }
    }

    Prefab::Prefab(vector<PrefabEntity> entities, vector<Ref<Detail::AssetCacheEntry>> dependencies)
        : m_Entities(std::move(entities)), m_Dependencies(std::move(dependencies))
    {
    }

    Ref<Prefab> Prefab::Create(vector<PrefabEntity> entities,
                               vector<Ref<Detail::AssetCacheEntry>> dependencies)
    {
        return Ref<Prefab>(new Prefab(std::move(entities), std::move(dependencies)));
    }

    vector<Entity> Prefab::SpawnInto(Scene& scene, AssetManager& manager) const
    {
        const TypeRegistry& registry = scene.GetTypeRegistry();

        // 1. Create every entity first, so a Reference field (which may point
        //    forward) always resolves to a created handle.
        vector<Entity> spawned;
        spawned.reserve(m_Entities.size());
        for (usize i = 0; i < m_Entities.size(); ++i)
        {
            spawned.push_back(scene.CreateEntity());
        }

        // 2. Populate each entity's components, then remap references / rehydrate
        //    handles once every entity exists. A Hierarchy component's serialized
        //    Parent edge now holds the remapped spawned entity; its sibling/child
        //    links are derived and rebuilt below.
        for (usize i = 0; i < m_Entities.size(); ++i)
        {
            const PrefabEntity& prefabEntity = m_Entities[i];
            const Entity entity = spawned[i];

            for (const Component& component : prefabEntity.Components)
            {
                // The cook validated each component against a registered
                // descriptor; an unknown TypeId at runtime is a registry/module
                // mismatch, not a recoverable case.
                VE_ASSERT(registry.IsRegistered(component.Type),
                          "Prefab::SpawnInto: component TypeId {:#018x} is not registered",
                          component.Type);

                const TypeInfo& typeInfo = registry.Info(component.Type);

                void* slot = scene.AddComponent(entity, component.Type);

                // The prefab loader validated this record at load; a read failure
                // here would be an engine invariant violation, not bad data.
                ReadFields(component.Record, slot, typeInfo, registry).value();
                Resolve(slot, typeInfo, registry, spawned, manager);
            }
        }

        // 2b. Fire each spawned component's resolver, now that every entity and
        //     component exists (a resolver may read sibling entities). A resolver may
        //     add a component (a MeshRenderer), so iterate the prefab's authored
        //     component list — not a live pool — and fetch storage fresh by TypeId.
        //     This runs before the hierarchy rebuild; adding a renderable does not
        //     affect parent edges.
        for (usize i = 0; i < m_Entities.size(); ++i)
        {
            const Entity entity = spawned[i];
            for (const Component& component : m_Entities[i].Components)
            {
                const TypeInfo& typeInfo = registry.Info(component.Type);
                if (typeInfo.SpawnResolve != nullptr)
                {
                    void* slot = scene.TryGetComponent(entity, component.Type);
                    typeInfo.SpawnResolve(slot, scene, entity, manager);
                }
            }
        }

        // 3. Rebuild the intrusive sibling/child links from the parent edges, in
        //    authoring order so appending preserves authored child order. SetParent
        //    detaches the (empty) old links and appends each child under its parent.
        //    Roots — no Hierarchy or a null parent edge — are returned in order.
        vector<Entity> roots;
        for (const Entity entity : spawned)
        {
            const Entity parent = scene.GetParent(entity);
            if (parent.IsNull())
            {
                roots.push_back(entity);
            }
            else
            {
                scene.SetParent(entity, parent);
            }
        }

        return roots;
    }
}
