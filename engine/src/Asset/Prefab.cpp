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
                else if (field.Class == FieldClass::Array)
                {
                    const TypeInfo& element = registry.Info(field.ElementType);
                    const usize count = field.ArraySize(fieldPtr);
                    for (usize i = 0; i < count; ++i)
                    {
                        const void* elementPtr = field.ArrayElementConst(fieldPtr, i);
                        if (element.Class == FieldClass::AssetHandle)
                        {
                            batch.TrackHandleField(elementPtr);
                        }
                        else if (element.Class == FieldClass::Struct)
                        {
                            CollectPendingHandles(elementPtr, element, registry, batch);
                        }
                    }
                }
            }
        }
    }

    namespace
    {
        // Resolve a nesting entity's body: the named prefab, which the loader kept resident as an
        // ordinary dependency. A missing or mistyped entry is a loader/registry invariant
        // violation past the validated cook, not a recoverable case.
        const Prefab& NestedBody(AssetManager& manager, AssetId id)
        {
            const Ref<Detail::AssetCacheEntry> entry = manager.CachedEntry(id);
            VE_ASSERT(entry != nullptr && entry->Resource != nullptr,
                      "Prefab::SpawnInto: nested prefab {} is not resident — the prefab loader "
                      "should have loaded it as a dependency",
                      id.Value);
            VE_ASSERT(entry->Type == AssetTypes::Prefab,
                      "Prefab::SpawnInto: nested asset {} is not a prefab", id.Value);
            return *static_cast<const Prefab*>(entry->Resource.get());
        }

        // An override replaces a whole component rather than merging into the fields the nested
        // prefab authored, so an existing one is reset to its default state before it is read
        // into. Returns the slot to read the override record into.
        void* ReplaceComponent(Scene& scene, Entity entity, TypeId type, const TypeInfo& info)
        {
            void* existing = scene.TryGetComponent(entity, type);
            if (existing == nullptr)
            {
                return scene.AddComponent(entity, type);
            }
            info.Destruct(existing);
            info.DefaultConstruct(existing);
            return existing;
        }

        // True when a prefab entity is server-authoritative: it carries an Authority whose Tier is
        // Server, or it carries no Authority at all (the authored default is Server). The client-mode
        // load skips exactly these — they arrive from the spawn stream instead.
        bool IsServerAuthoritative(const Prefab::PrefabEntity& entity, const TypeRegistry& registry)
        {
            const TypeId authorityId = TypeIdOf<Authority>();
            for (const Prefab::Component& component : entity.Components)
            {
                if (component.Type != authorityId)
                {
                    continue;
                }
                Authority authority;
                if (ReadFields(component.Record, &authority, registry.Info(authorityId), registry))
                {
                    return authority.Tier == Tier::Server;
                }
                return true; // an undecodable Authority record: treat as the default (Server).
            }
            return true; // no Authority component: the authored default is Server.
        }
    }

    Prefab::Prefab(vector<PrefabEntity> entities, vector<Ref<Detail::AssetCacheEntry>> dependencies,
                   const AssetId source)
        : m_Entities(std::move(entities)), m_SourceId(source),
          m_Dependencies(std::move(dependencies))
    {
    }

    Ref<Prefab> Prefab::Create(vector<PrefabEntity> entities,
                               vector<Ref<Detail::AssetCacheEntry>> dependencies,
                               const AssetId source)
    {
        return Ref<Prefab>(new Prefab(std::move(entities), std::move(dependencies), source));
    }

    Prefab::SpawnResult Prefab::SpawnInto(Scene& scene, AssetManager& manager) const
    {
        return SpawnInto(scene, manager, SpawnOptions{});
    }

    Prefab::SpawnResult Prefab::SpawnInto(Scene& scene, AssetManager& manager,
                                          const SpawnOptions& options) const
    {
        const TypeRegistry& registry = scene.GetTypeRegistry();

        // A skipped entity's slot stays Entity::Null so index-keyed reference remaps read null.
        // A nesting entity is never itself skipped: it authors no body of its own, and the
        // authority of the entities its prefab declares is decided inside that prefab's spawn.
        vector<bool> skip(m_Entities.size(), false);
        if (options.SkipServerAuthoritative)
        {
            for (usize i = 0; i < m_Entities.size(); ++i)
            {
                skip[i] = !m_Entities[i].NestedPrefab.IsValid() &&
                          IsServerAuthoritative(m_Entities[i], registry);
            }
        }

        // 1. Create every non-skipped entity first, so a Reference field (which may
        //    point forward) always resolves to a created handle.
        vector<Entity> spawned;
        spawned.reserve(m_Entities.size());
        for (usize i = 0; i < m_Entities.size(); ++i)
        {
            spawned.push_back(skip[i] ? Entity::Null : scene.CreateEntity());
        }

        // The handles this spawn leaves pending, including every nested expansion's own — so one
        // SpawnResult still reports everything a spawn left pending.
        ResidencyBatch pending;

        // 2. Populate each entity's components, then remap references / rehydrate
        //    handles once every entity exists. A Hierarchy component's serialized
        //    Parent edge now holds the remapped spawned entity; its sibling/child
        //    links are derived and rebuilt below. A nesting entity instead expands its
        //    named prefab here and reads its records onto the expansion's first root.
        vector<vector<Entity>> nestedRoots(m_Entities.size());
        for (usize i = 0; i < m_Entities.size(); ++i)
        {
            if (skip[i])
            {
                continue;
            }
            const PrefabEntity& prefabEntity = m_Entities[i];
            const Entity entity = spawned[i];
            const bool nesting = prefabEntity.NestedPrefab.IsValid();

            // The overrides land on the expansion's first root; an expansion that materialized
            // nothing (an empty prefab, or every authored entity skipped) leaves the nesting
            // entity itself as the thing they describe.
            Entity overridden = entity;
            if (nesting)
            {
                SpawnResult nested = NestedBody(manager, prefabEntity.NestedPrefab)
                                         .SpawnInto(scene, manager, options);
                pending.Merge(std::move(nested.Pending));
                nestedRoots[i] = std::move(nested.Roots);
                if (!nestedRoots[i].empty())
                {
                    overridden = nestedRoots[i].front();
                }
            }

            for (const Component& component : prefabEntity.Components)
            {
                // The cook validated each component against a registered
                // descriptor; an unknown TypeId at runtime is a registry/module
                // mismatch, not a recoverable case.
                VE_ASSERT(registry.IsRegistered(component.Type),
                          "Prefab::SpawnInto: component TypeId {:#018x} is not registered",
                          component.Type);

                const TypeInfo& typeInfo = registry.Info(component.Type);

                // Hierarchy names an entity in *this* prefab's table, so it stays on the nesting
                // entity — the expansion's roots take their parent from that entity below.
                const Entity host =
                    (nesting && component.Type != TypeIdOf<Hierarchy>()) ? overridden : entity;

                void* slot = nesting ? ReplaceComponent(scene, host, component.Type, typeInfo)
                                     : scene.AddComponent(host, component.Type);

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

                // An override written onto an expansion's root is outside the final sweep below,
                // which walks this prefab's own entities.
                if (host != entity)
                {
                    CollectPendingHandles(slot, typeInfo, registry, pending);
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
            if (spawned[i].IsNull())
            {
                continue;
            }
            parents[i] = scene.GetParent(spawned[i]);
            if (auto* link = scene.TryGet<Hierarchy>(spawned[i]))
            {
                link->Parent = Entity::Null;
            }
        }

        vector<Entity> roots;
        for (usize i = 0; i < spawned.size(); ++i)
        {
            if (spawned[i].IsNull())
            {
                continue;
            }
            if (parents[i].IsNull())
            {
                roots.push_back(spawned[i]);
            }
            else
            {
                scene.SetParent(spawned[i], parents[i]);
            }
        }

        // Each expansion's roots hang under their nesting entity, appended in authored order.
        // This runs after the pass above: with the nesting entity's child list already holding a
        // nested root, that pass's UnlinkFromSiblings would treat a freshly linked authored child
        // as the head of the list and drop the nested root out of it.
        for (usize i = 0; i < nestedRoots.size(); ++i)
        {
            for (const Entity root : nestedRoots[i])
            {
                scene.SetParent(root, spawned[i]);
            }
        }

        // Record spawn provenance on each root. A runtime-built prefab carries no addressable id, so
        // there is nothing to record and none is added.
        if (m_SourceId.IsValid())
        {
            for (const Entity root : roots)
            {
                scene.Add<PrefabSource>(root, PrefabSource{.Prefab = m_SourceId});
            }
        }

        // Collect the handles this spawn left pending — the recipe-built meshes, plus any cooked
        // handle a future loader path leaves unresolved. Walk the live components (rehydrated and
        // recipe-built above), not the cooked records, so the batch reflects the spawned state.
        for (const Entity entity : spawned)
        {
            if (entity.IsNull())
            {
                continue;
            }
            scene.ForEachComponent(
                entity, [&](TypeId type, void* component)
                { CollectPendingHandles(component, registry.Info(type), registry, pending); });
        }

        return SpawnResult{.Roots = std::move(roots), .Pending = std::move(pending)};
    }
}
