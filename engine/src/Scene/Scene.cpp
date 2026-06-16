#include <Veng/Scene/Scene.h>

#include <Veng/Assert.h>

#include "ComponentPool.h"

namespace Veng
{
    Scene::Scene(TypeRegistry& registry) :
        m_Registry(&registry)
    {
    }

    Scene::~Scene() = default;

    Unique<Scene> Scene::Create(TypeRegistry& registry)
    {
        // Private constructor: construct in place rather than via CreateUnique.
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

        return Entity{index, slot.Generation};
    }

    void Scene::DestroyEntity(Entity entity)
    {
        VE_ASSERT(IsAlive(entity), "DestroyEntity on a dead or stale entity");

        for (auto& [id, pool] : m_Pools)
        {
            pool->Remove(entity);
        }

        EntitySlot& slot = m_Slots[entity.Index];
        slot.Alive = false;
        // Bump so any surviving handle to this slot is detectably stale.
        ++slot.Generation;
        --m_LiveCount;
        m_FreeIndices.push_back(entity.Index);
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

    void* Scene::AddRaw(Entity entity, TypeId id)
    {
        return PoolFor(id).Add(entity);
    }

    void Scene::RemoveRaw(Entity entity, TypeId id)
    {
        if (ComponentPool* pool = TryPoolFor(id))
        {
            pool->Remove(entity);
        }
    }

    void* Scene::TryGetRaw(Entity entity, TypeId id)
    {
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

        VE_ASSERT(m_Registry->IsRegistered(id),
                  "component TypeId {:#018x} is not registered", id);

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
