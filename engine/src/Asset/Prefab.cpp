#include <Veng/Asset/Prefab.h>

#include <Veng/Assert.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Scene/Resolve.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SceneClone.h>

#include <cstring>

namespace Veng
{
    namespace
    {
        // Post-ReadFields pass: remap Reference fields (prefab-local index → spawned
        // Entity) and rehydrate AssetHandle fields (raw AssetId → cache entry).
        // Shares its field walk with Scene::Clone via RemapComponentReferences.
        void Resolve(void* obj, const TypeInfo& type, const TypeRegistry& registry,
                     const vector<Entity>& spawned, AssetManager& manager)
        {
            const EntityRemap remap = [&spawned](Entity reference) -> Entity
            {
                // The null sentinel stays null; never a valid prefab-local index, so
                // it cannot collide with an intra-prefab ref.
                if (reference.Index == Entity::InvalidIndex)
                {
                    return Entity::Null;
                }

                // The cook bounds-checked the index against the entity count, so an
                // out-of-range value here is corruption past the validated cook — fatal.
                VE_ASSERT(reference.Index < spawned.size(),
                          "Prefab::SpawnInto: entity reference index {} out of range ({} entities)",
                          reference.Index, spawned.size());

                return spawned[reference.Index];
            };

            const AssetHandleFixup rehydrate = [&manager](void* fieldPtr)
            {
                // ReadFields wrote the raw AssetId at offset 0; the cache entry is
                // null. Resolve the id to the prefab's already-resident dependency
                // entry (loaded at load time). An invalid id is the "no asset" value
                // — leave it empty.
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
            };

            RemapComponentReferences(obj, type, registry, remap, rehydrate);
        }

        // Walk a live component for FieldClass::AssetHandle fields and track each not-yet-resident
        // one into the batch, recursing into struct fields and a variant's active alternative — the
        // CollectHandleDeps shape, run over the spawned (rehydrated, recipe-built) components rather
        // than the cooked records. The walk is uniform over every handle field; a cooked embedded
        // dependency is already resident (the rehydrate above asserts it), so in practice the batch
        // captures the recipe-built mesh handles a spawn introduces.
        void CollectPendingHandles(const void* obj, const TypeInfo& type,
                                   const TypeRegistry& registry, ResidencyBatch& batch)
        {
            for (const FieldDescriptor& field : type.Fields)
            {
                const void* fieldPtr = static_cast<const u8*>(obj) + field.Offset;

                if (field.Class == FieldClass::AssetHandle)
                {
                    batch.TrackHandleField(fieldPtr);
                }
                else if (field.Class == FieldClass::Struct)
                {
                    CollectPendingHandles(fieldPtr, registry.Info(field.Type), registry, batch);
                }
                else if (field.Class == FieldClass::Variant)
                {
                    const TypeInfo& variant = registry.Info(field.Type);
                    const TypeId active = variant.VariantActiveType(fieldPtr);
                    if (active != InvalidTypeId)
                    {
                        CollectPendingHandles(variant.VariantActivePtrConst(fieldPtr),
                                              registry.Info(active), registry, batch);
                    }
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

    Prefab::SpawnResult Prefab::SpawnInto(Scene& scene, AssetManager& manager) const
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

                // A MeshRenderer's inline recipe source builds into its Mesh through the
                // ordinary async load path, yielding a pending handle exactly like the
                // cooked-mesh dependency a sibling entity would carry.
                if (component.Type == TypeIdOf<MeshRenderer>())
                {
                    auto& renderer = *static_cast<MeshRenderer*>(slot);
                    if (renderer.Source.ActiveType() != InvalidTypeId)
                    {
                        renderer.Mesh = BuildPrimitiveMesh(manager, renderer.Source);
                    }
                }
            }
        }

        // 3. Rebuild the intrusive sibling/child links from the parent edges, in
        //    authoring order so appending preserves authored child order.
        //
        //    ReadFields pre-set each Hierarchy's Parent (a reflected field) but left
        //    the derived FirstChild/PrevSibling/NextSibling links null. Capture every
        //    parent target, then clear the half-set Parent edges before linking: with
        //    Parent still set but the links null, SetParent's UnlinkFromSiblings would
        //    treat the child as the head of its parent's list and drop a prior sibling
        //    sharing that parent. Clearing first lets SetParent build from a clean
        //    slate. Roots — no Hierarchy or a null parent edge — are returned in order.
        vector<Entity> parents(spawned.size());
        for (usize i = 0; i < spawned.size(); ++i)
        {
            parents[i] = scene.GetParent(spawned[i]);
            if (auto* link = scene.TryGet<Hierarchy>(spawned[i]))
            {
                link->Parent = Entity::Null;
            }
        }

        vector<Entity> roots;
        for (usize i = 0; i < spawned.size(); ++i)
        {
            if (parents[i].IsNull())
            {
                roots.push_back(spawned[i]);
            }
            else
            {
                scene.SetParent(spawned[i], parents[i]);
            }
        }

        // Collect the handles this spawn left pending — the recipe-built meshes, plus any cooked
        // handle a future loader path leaves unresolved. Walk the live components (rehydrated and
        // recipe-built above), not the cooked records, so the batch reflects the spawned state.
        ResidencyBatch pending;
        for (const Entity entity : spawned)
        {
            scene.ForEachComponent(
                entity, [&](TypeId type, void* component)
                { CollectPendingHandles(component, registry.Info(type), registry, pending); });
        }

        return SpawnResult{.Roots = std::move(roots), .Pending = std::move(pending)};
    }
}
