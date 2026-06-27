#include <Veng/Scene/Scene.h>

#include <Veng/Assert.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/SceneClone.h>
#include <Veng/Scene/SceneSimulation.h>

#include "ComponentPool.h"

#include <cstring>

namespace Veng
{
    void RemapComponentReferences(void* obj, const TypeInfo& type, const TypeRegistry& registry,
                                  const EntityRemap& remap, const AssetHandleFixup& assetHandle)
    {
        for (const FieldDescriptor& field : type.Fields)
        {
            void* fieldPtr = static_cast<u8*>(obj) + field.Offset;

            switch (field.Class)
            {
            case FieldClass::Reference:
            {
                Entity& entity = *static_cast<Entity*>(fieldPtr);
                entity = remap(entity);
                break;
            }

            case FieldClass::AssetHandle:
            {
                assetHandle(fieldPtr);
                break;
            }

            case FieldClass::Struct:
            {
                const TypeInfo& nested = registry.Info(field.Type);
                RemapComponentReferences(fieldPtr, nested, registry, remap, assetHandle);
                break;
            }

            case FieldClass::Variant:
            {
                // Descend into the active alternative so a Reference or AssetHandle
                // inside it is reached like one in a nested struct; an empty variant
                // has nothing to fix up.
                const TypeInfo& info = registry.Info(field.Type);
                void* memberPtr = info.VariantActivePtr(fieldPtr);
                if (memberPtr != nullptr)
                {
                    const TypeId active = info.VariantActiveType(fieldPtr);
                    RemapComponentReferences(memberPtr, registry.Info(active), registry, remap,
                                             assetHandle);
                }
                break;
            }

            default:
                break;
            }
        }
    }

    bool Scene::IsSpatialId(TypeId id)
    {
        // These three pools decide draw candidacy and world bounds; checking compile-time
        // TypeId constants (not a registry lookup) keeps mutation-path overhead minimal.
        return id == TypeIdOf<Transform>() || id == TypeIdOf<Hierarchy>() ||
               id == TypeIdOf<MeshRenderer>();
    }

    Scene::Scene(TypeRegistry& registry) : m_Registry(&registry) {}

    Scene::~Scene() = default;

    Unique<Scene> Scene::Create(TypeRegistry& registry)
    {
        // Private constructor: raw new, not CreateUnique.
        return Unique<Scene>(new Scene(registry));
    }

    void Scene::SetSimulation(Unique<SceneSimulation> simulation)
    {
        m_Simulation = std::move(simulation);
    }

    void Scene::StartSimulation(const SystemContext& context)
    {
        if (m_Simulation)
        {
            m_Simulation->Start(*this, context);
        }
    }

    void Scene::TickSimulation(const f32 delta, const SystemContext& context)
    {
        if (m_Simulation)
        {
            m_Simulation->Update(*this, delta, context);
        }
    }

    void Scene::StopSimulation(const SystemContext& context)
    {
        if (m_Simulation)
        {
            m_Simulation->Stop(*this, context);
        }
    }

    Unique<Scene> Scene::Clone() const
    {
        const TypeRegistry& registry = *m_Registry;
        Unique<Scene> clone = Create(*m_Registry);

        // 1. Recreate every live entity first, so a Reference field resolving
        //    forward always lands on a created handle. The destination is empty, so
        //    its slot order mirrors the source's live-slot order — but the remap is
        //    keyed on the source handle, not on slot identity.
        unordered_map<Entity, Entity> remap;
        remap.reserve(m_LiveCount);
        ForEachEntity([&](Entity source) { remap.emplace(source, clone->CreateEntity()); });

        const EntityRemap remapFn = [&remap](Entity source) -> Entity
        {
            if (source.IsNull())
            {
                return Entity::Null;
            }
            const auto it = remap.find(source);
            return it != remap.end() ? it->second : Entity::Null;
        };

        // 2. Copy every component. WriteFields/ReadFields round-trips the value
        //    bytes; the post-pass remaps Reference fields to the cloned handles and
        //    deep-copies AssetHandle fields directly. A serialized AssetHandle keeps
        //    only the AssetId, so a runtime-adopted (id-less) handle would round-trip
        //    to empty — copy the live cache-entry Ref straight across instead, so a
        //    cloned scene still renders its already-resident meshes.
        vector<u8> record;
        for (const auto& [typeId, pool] : m_Pools)
        {
            // Hierarchy is a derived component: only its Parent edge persists, and the
            // sibling/child links are rebuilt from the parent edges in pass 3. Copying
            // it here would pre-set Parent without consistent links, so the SetParent
            // rebuild would unlink against a corrupt list. Skip it and rebuild instead.
            if (typeId == TypeIdOf<Hierarchy>())
            {
                continue;
            }

            const TypeInfo& typeInfo = registry.Info(typeId);
            const usize count = pool->Count();
            const Entity* dense = pool->DenseData();

            for (usize i = 0; i < count; ++i)
            {
                const Entity source = dense[i];
                const Entity target = remap.at(source);
                const void* sourceComponent = pool->TryGet(source);

                record.clear();
                WriteFields(record, sourceComponent, typeInfo, registry);

                void* targetComponent = clone->AddComponent(target, typeId);
                ReadFields(record, targetComponent, typeInfo, registry).value();

                const AssetHandleFixup copyHandle = [&](void* targetField)
                {
                    // The matching source field sits at the same offset within the
                    // identically-laid-out source component; copy the whole handle
                    // (AssetId + cache-entry Ref) so a resident handle survives.
                    const usize offset =
                        static_cast<u8*>(targetField) - static_cast<u8*>(targetComponent);
                    const auto* sourceField = static_cast<const u8*>(sourceComponent) + offset;

                    AssetId id{};
                    std::memcpy(&id, sourceField, sizeof(id));
                    const auto* sourceEntry = reinterpret_cast<const Ref<Detail::AssetCacheEntry>*>(
                        sourceField + Detail::AssetHandleEntryOffset);
                    Detail::RehydrateHandleField(targetField, id, *sourceEntry);
                };

                RemapComponentReferences(targetComponent, typeInfo, registry, remapFn, copyHandle);
            }
        }

        // 3. Rebuild the intrusive sibling/child links from the remapped parent
        //    edges, in source order so SetParent appends children in their original
        //    order — the same derived-link rebuild Prefab::SpawnInto performs.
        ForEachEntity(
            [&](Entity source)
            {
                const Entity target = remap.at(source);
                const Entity sourceParent = GetParent(source);
                if (!sourceParent.IsNull())
                {
                    clone->SetParent(target, remap.at(sourceParent));
                }
            });

        return clone;
    }

    Entity Scene::CreateEntity()
    {
        u32 index;
        if (!m_FreeIndices.empty())
        {
            index = m_FreeIndices.back();
            m_FreeIndices.pop_back();
        }
        else
        {
            index = static_cast<u32>(m_Slots.size());
            m_Slots.push_back(EntitySlot{});
        }

        EntitySlot& slot = m_Slots[index];
        slot.Alive = true;
        ++m_LiveCount;

        return Entity{.Index = index, .Generation = slot.Generation};
    }

    void Scene::DestroyEntity(Entity entity)
    {
        VE_ASSERT(IsAlive(entity), "DestroyEntity on a dead or stale entity");

        // Detach the destroyed root from any surviving parent's child list first,
        // so the siblings that outlive this call stay consistent.
        UnlinkFromSiblings(entity);

        // Destroying an entity destroys its whole subtree. Walk the FirstChild →
        // NextSibling links to collect it in O(subtree), then tear down — never
        // iterating-and-destroying a pool (a structural change mid-iteration is
        // illegal).
        const TypeId hierarchyId = TypeIdOf<Hierarchy>();

        vector<Entity> collected;
        collected.push_back(entity);
        for (usize scanned = 0; scanned < collected.size(); ++scanned)
        {
            const ComponentPool* pool = TryPoolFor(hierarchyId);
            if (pool == nullptr)
            {
                break;
            }
            const auto* link = static_cast<const Hierarchy*>(pool->TryGet(collected[scanned]));
            if (link == nullptr)
            {
                continue;
            }
            for (Entity child = link->FirstChild; !child.IsNull();)
            {
                const auto* childLink = static_cast<const Hierarchy*>(pool->TryGet(child));
                collected.push_back(child);
                child = childLink != nullptr ? childLink->NextSibling : Entity::Null;
            }
        }

        bool spatialTouched = false;
        for (const Entity dead : collected)
        {
            for (auto& [id, pool] : m_Pools)
            {
                if (pool->Contains(dead))
                {
                    spatialTouched = spatialTouched || IsSpatialId(id);
                    pool->Remove(dead);
                }
            }

            EntitySlot& slot = m_Slots[dead.Index];
            slot.Alive = false;
            // Bump so any surviving handle to this slot is detectably stale.
            ++slot.Generation;
            --m_LiveCount;
            m_FreeIndices.push_back(dead.Index);
        }

        if (spatialTouched)
        {
            BumpSpatial();
        }
    }

    Hierarchy& Scene::HierarchyOf(Entity entity)
    {
        // Resolve through the pool directly, not the templated TryGet/Add, so the
        // structural ops bump the spatial version exactly once each (explicitly),
        // never per link touched.
        const TypeId id = TypeIdOf<Hierarchy>();
        if (ComponentPool* pool = TryPoolFor(id))
        {
            if (void* slot = pool->TryGet(entity))
            {
                return *static_cast<Hierarchy*>(slot);
            }
        }
        return *static_cast<Hierarchy*>(PoolFor(id).Add(entity));
    }

    const Hierarchy* Scene::TryHierarchy(Entity entity) const
    {
        if (const ComponentPool* pool = TryPoolFor(TypeIdOf<Hierarchy>()))
        {
            return static_cast<const Hierarchy*>(pool->TryGet(entity));
        }
        return nullptr;
    }

    void Scene::UnlinkFromSiblings(Entity child)
    {
        Hierarchy* link = nullptr;
        if (ComponentPool* pool = TryPoolFor(TypeIdOf<Hierarchy>()))
        {
            link = static_cast<Hierarchy*>(pool->TryGet(child));
        }
        if (link == nullptr)
        {
            return;
        }

        const Entity prev = link->PrevSibling;
        const Entity next = link->NextSibling;

        if (!prev.IsNull())
        {
            HierarchyOf(prev).NextSibling = next;
        }
        else if (!link->Parent.IsNull())
        {
            // child was the head of its parent's list; promote the next sibling.
            HierarchyOf(link->Parent).FirstChild = next;
        }

        if (!next.IsNull())
        {
            HierarchyOf(next).PrevSibling = prev;
        }

        link->PrevSibling = Entity::Null;
        link->NextSibling = Entity::Null;
    }

    bool Scene::IsDescendantOf(Entity candidate, Entity entity) const
    {
        Entity current = candidate;
        while (!current.IsNull())
        {
            if (current == entity)
            {
                return true;
            }
            const Hierarchy* link = TryHierarchy(current);
            current = link != nullptr ? link->Parent : Entity::Null;
        }
        return false;
    }

    void Scene::SetParent(Entity child, Entity parent)
    {
        VE_ASSERT(IsAlive(child), "SetParent on a dead or stale child");
        VE_ASSERT(parent.IsNull() || IsAlive(parent), "SetParent: parent is dead or stale");
        VE_ASSERT(child != parent, "SetParent: an entity cannot parent itself");
        // A descendant adopting an ancestor would form a cycle — API misuse.
        VE_ASSERT(parent.IsNull() || !IsDescendantOf(parent, child),
                  "SetParent: parent is a descendant of child (cycle)");

        UnlinkFromSiblings(child);

        Hierarchy& childLink = HierarchyOf(child);
        childLink.Parent = parent;

        if (!parent.IsNull())
        {
            Hierarchy& parentLink = HierarchyOf(parent);
            if (parentLink.FirstChild.IsNull())
            {
                parentLink.FirstChild = child;
            }
            else
            {
                // Append to the tail so children stay in insertion order.
                Entity tail = parentLink.FirstChild;
                while (true)
                {
                    Hierarchy& tailLink = HierarchyOf(tail);
                    if (tailLink.NextSibling.IsNull())
                    {
                        tailLink.NextSibling = child;
                        // HierarchyOf(child) may have moved the pool; re-fetch.
                        Hierarchy& reChild = HierarchyOf(child);
                        reChild.PrevSibling = tail;
                        break;
                    }
                    tail = tailLink.NextSibling;
                }
            }
        }

        BumpSpatial();
    }

    void Scene::Detach(Entity child)
    {
        SetParent(child, Entity::Null);
    }

    void Scene::MoveBefore(Entity child, Entity sibling)
    {
        VE_ASSERT(IsAlive(child), "MoveBefore on a dead or stale child");
        VE_ASSERT(!sibling.IsNull() && IsAlive(sibling),
                  "MoveBefore: sibling is null, dead, or stale");
        VE_ASSERT(child != sibling, "MoveBefore: child and sibling are the same entity");

        const Hierarchy* siblingLink = TryHierarchy(sibling);
        const Entity parent = siblingLink != nullptr ? siblingLink->Parent : Entity::Null;

        VE_ASSERT(parent.IsNull() || !IsDescendantOf(parent, child),
                  "MoveBefore: sibling's parent is a descendant of child (cycle)");

        UnlinkFromSiblings(child);

        Hierarchy& childLink = HierarchyOf(child);
        childLink.Parent = parent;

        const Hierarchy& sib = HierarchyOf(sibling);
        const Entity prev = sib.PrevSibling;

        HierarchyOf(child).PrevSibling = prev;
        HierarchyOf(child).NextSibling = sibling;
        HierarchyOf(sibling).PrevSibling = child;

        if (prev.IsNull())
        {
            if (!parent.IsNull())
            {
                HierarchyOf(parent).FirstChild = child;
            }
        }
        else
        {
            HierarchyOf(prev).NextSibling = child;
        }

        BumpSpatial();
    }

    Entity Scene::GetParent(Entity entity) const
    {
        VE_ASSERT(IsAlive(entity), "GetParent on a dead or stale entity");
        const Hierarchy* link = TryHierarchy(entity);
        return link != nullptr ? link->Parent : Entity::Null;
    }

    void Scene::ForEachChild(Entity entity, const function<void(Entity)>& fn) const
    {
        VE_ASSERT(IsAlive(entity), "ForEachChild on a dead or stale entity");
        const Hierarchy* link = TryHierarchy(entity);
        if (link == nullptr)
        {
            return;
        }
        for (Entity child = link->FirstChild; !child.IsNull();)
        {
            const Hierarchy* childLink = TryHierarchy(child);
            const Entity next = childLink != nullptr ? childLink->NextSibling : Entity::Null;
            fn(child);
            child = next;
        }
    }

    bool Scene::IsAlive(Entity entity) const
    {
        if (entity.IsNull() || entity.Index >= m_Slots.size())
        {
            return false;
        }
        const EntitySlot& slot = m_Slots[entity.Index];
        return slot.Alive && slot.Generation == entity.Generation;
    }

    Entity Scene::GetLiveEntityAtIndex(const u32 index) const
    {
        if (index >= m_Slots.size())
        {
            return Entity::Null;
        }
        const EntitySlot& slot = m_Slots[index];
        if (!slot.Alive)
        {
            return Entity::Null;
        }
        return Entity{.Index = index, .Generation = slot.Generation};
    }

    void Scene::ForEachEntity(const function<void(Entity)>& fn) const
    {
        for (u32 index = 0; index < m_Slots.size(); ++index)
        {
            const EntitySlot& slot = m_Slots[index];
            if (slot.Alive)
            {
                fn(Entity{.Index = index, .Generation = slot.Generation});
            }
        }
    }

    void* Scene::AddRaw(Entity entity, TypeId id)
    {
        if (IsSpatialId(id))
        {
            BumpSpatial();
        }
        return PoolFor(id).Add(entity);
    }

    void Scene::RemoveRaw(Entity entity, TypeId id)
    {
        if (ComponentPool* pool = TryPoolFor(id))
        {
            if (IsSpatialId(id))
            {
                BumpSpatial();
            }
            pool->Remove(entity);
        }
    }

    void* Scene::TryGetRaw(Entity entity, TypeId id)
    {
        // A non-const access is a potential in-place edit the ECS never sees,
        // so bump the version conservatively (over-bump, never under).
        if (IsSpatialId(id))
        {
            BumpSpatial();
        }
        if (ComponentPool* pool = TryPoolFor(id))
        {
            return pool->TryGet(entity);
        }
        return nullptr;
    }

    const void* Scene::TryGetRaw(Entity entity, TypeId id) const
    {
        if (const ComponentPool* pool = TryPoolFor(id))
        {
            return pool->TryGet(entity);
        }
        return nullptr;
    }

    bool Scene::HasRaw(Entity entity, TypeId id) const
    {
        const ComponentPool* pool = TryPoolFor(id);
        return pool != nullptr && pool->Contains(entity);
    }

    void Scene::ForEachComponent(Entity entity, const function<void(TypeId, void*)>& fn)
    {
        VE_ASSERT(IsAlive(entity), "ForEachComponent on a dead or stale entity");

        for (auto& [id, pool] : m_Pools)
        {
            if (void* component = pool->TryGet(entity))
            {
                // The erased pointer is a mutable edit funnel (the inspector's),
                // so visiting a spatial pool bumps the version like a non-const
                // access.
                if (IsSpatialId(id))
                {
                    BumpSpatial();
                }
                fn(id, component);
            }
        }
    }

    usize Scene::PoolCount(TypeId id) const
    {
        const ComponentPool* pool = TryPoolFor(id);
        return pool != nullptr ? pool->Count() : 0;
    }

    const Entity* Scene::DensePtr(TypeId id) const
    {
        const ComponentPool* pool = TryPoolFor(id);
        return pool != nullptr ? pool->DenseData() : nullptr;
    }

    Scene::ComponentPool& Scene::PoolFor(TypeId id)
    {
        const auto it = m_Pools.find(id);
        if (it != m_Pools.end())
        {
            return *it->second;
        }

        VE_ASSERT(m_Registry->IsRegistered(id), "component TypeId {:#018x} is not registered", id);

        auto pool = Unique<ComponentPool>(new ComponentPool(m_Registry->Info(id)));
        ComponentPool& ref = *pool;
        m_Pools.emplace(id, std::move(pool));
        return ref;
    }

    Scene::ComponentPool* Scene::TryPoolFor(TypeId id)
    {
        const auto it = m_Pools.find(id);
        return it != m_Pools.end() ? it->second.get() : nullptr;
    }

    const Scene::ComponentPool* Scene::TryPoolFor(TypeId id) const
    {
        const auto it = m_Pools.find(id);
        return it != m_Pools.end() ? it->second.get() : nullptr;
    }
}
