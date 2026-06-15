#include "ComponentPool.h"

#include <Veng/Assert.h>

namespace Veng
{
    Scene::ComponentPool::ComponentPool(const TypeInfo& info) :
        m_Info(info)
    {
        VE_ASSERT(m_Info.Size > 0,
                  "Component type '{}' has zero size", m_Info.Name);
    }

    Scene::ComponentPool::~ComponentPool()
    {
        for (usize i = 0; i < m_Dense.size(); ++i)
        {
            m_Info.Destruct(DataAt(i));
        }
    }

    void* Scene::ComponentPool::DataAt(usize index)
    {
        return m_Data.data() + index * m_Info.Size;
    }

    void* Scene::ComponentPool::Add(Entity entity)
    {
        VE_ASSERT(!Contains(entity),
                  "entity already has a '{}' component", m_Info.Name);

        if (entity.Index >= m_Sparse.size())
        {
            m_Sparse.resize(entity.Index + 1, Tombstone);
        }

        const u32 dense = static_cast<u32>(m_Dense.size());
        m_Sparse[entity.Index] = dense;
        m_Dense.push_back(entity);
        m_Data.resize(m_Data.size() + m_Info.Size);

        void* slot = DataAt(dense);
        m_Info.DefaultConstruct(slot);
        return slot;
    }

    void Scene::ComponentPool::Remove(Entity entity)
    {
        if (!Contains(entity))
        {
            return;
        }

        const u32 dense = m_Sparse[entity.Index];
        const u32 last = static_cast<u32>(m_Dense.size() - 1);

        if (dense != last)
        {
            // Swap-and-pop: move the tail component into the hole, then patch the
            // sparse mapping for the entity that owned the tail.
            void* hole = DataAt(dense);
            void* tail = DataAt(last);
            m_Info.Destruct(hole);
            m_Info.MoveConstruct(hole, tail);

            const Entity moved = m_Dense[last];
            m_Dense[dense] = moved;
            m_Sparse[moved.Index] = dense;

            m_Info.Destruct(tail);
        }
        else
        {
            m_Info.Destruct(DataAt(dense));
        }

        m_Sparse[entity.Index] = Tombstone;
        m_Dense.pop_back();
        m_Data.resize(m_Data.size() - m_Info.Size);
    }

    bool Scene::ComponentPool::Contains(Entity entity) const
    {
        if (entity.Index >= m_Sparse.size())
        {
            return false;
        }
        const u32 dense = m_Sparse[entity.Index];
        return dense != Tombstone && dense < m_Dense.size()
            && m_Dense[dense] == entity;
    }

    void* Scene::ComponentPool::TryGet(Entity entity)
    {
        if (!Contains(entity))
        {
            return nullptr;
        }
        return DataAt(m_Sparse[entity.Index]);
    }

    const void* Scene::ComponentPool::TryGet(Entity entity) const
    {
        if (!Contains(entity))
        {
            return nullptr;
        }
        return const_cast<ComponentPool*>(this)->DataAt(m_Sparse[entity.Index]);
    }
}
