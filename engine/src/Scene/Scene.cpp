#include <Veng/Scene/Scene.h>

#include <Veng/Assert.h>
#include <Veng/Scene/Components.h>

#include "ComponentPool.h"

namespace Veng
{
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
