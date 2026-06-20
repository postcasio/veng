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
        return id == TypeIdOf<Transform>() || id == TypeIdOf<Parent>() ||
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

        // Destroying a parent destroys its whole subtree. Parent is an up-link
        // only (no child index), so collect the subtree first by a breadth-first
        // closure over the up-links, then tear down — never iterating-and-
        // destroying a pool (a structural change mid-iteration is illegal).
        const TypeId parentId = TypeIdOf<Parent>();

        vector<Entity> collected;
        collected.push_back(entity);
        for (usize scanned = 0; scanned < collected.size(); ++scanned)
        {
            const Entity ancestor = collected[scanned];
            if (const ComponentPool* parents = TryPoolFor(parentId))
            {
                const usize count = parents->Count();
                const Entity* dense = parents->DenseData();
                for (usize i = 0; i < count; ++i)
                {
                    const Entity child = dense[i];
                    const auto* link = static_cast<const Parent*>(parents->TryGet(child));
                    if (link == nullptr || link->Value != ancestor)
                    {
                        continue;
                    }

                    bool alreadyCollected = false;
                    for (const Entity seen : collected)
                    {
                        if (seen == child)
                        {
                            alreadyCollected = true;
                            break;
                        }
                    }
                    if (!alreadyCollected)
                    {
                        collected.push_back(child);
                    }
                }
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
