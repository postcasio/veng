#include <Veng/Scene/ComponentPool.h>

#include <Veng/Assert.h>

#include <new>

namespace Veng::Detail
{
    ComponentPool::ComponentPool(const TypeInfo& info, const AccessVersion access)
        : m_Info(info), m_Stride(info.Size), m_Access(access)
    {
        VE_ASSERT(m_Info.Size > 0, "Component type '{}' has zero size", m_Info.Name);
    }

    ComponentPool::~ComponentPool()
    {
        for (u32 i = 0; i < m_Dense.size(); ++i)
        {
            m_Info.Destruct(SlotData(i));
        }
        if (m_Data != nullptr)
        {
            ::operator delete(m_Data, std::align_val_t{m_Info.Align});
        }
    }

    void ComponentPool::Reserve(const usize capacity)
    {
        auto* const data = static_cast<std::byte*>(
            ::operator new(capacity * m_Stride, std::align_val_t{m_Info.Align}));

        // Relocate through the type's own move constructor: a byte copy is only valid for a
        // trivially relocatable type, and a component may hold a pointer into itself.
        for (usize i = 0; i < m_Dense.size(); ++i)
        {
            void* const from = m_Data + (i * m_Stride);
            m_Info.MoveConstruct(data + (i * m_Stride), from);
            m_Info.Destruct(from);
        }
        if (m_Data != nullptr)
        {
            ::operator delete(m_Data, std::align_val_t{m_Info.Align});
        }
        m_Data = data;
        m_Capacity = capacity;
    }

    void* ComponentPool::Add(const Entity entity)
    {
        VE_ASSERT(!Contains(entity), "entity already has a '{}' component", m_Info.Name);

        if (entity.Index >= m_Sparse.size())
        {
            m_Sparse.resize(entity.Index + 1, Absent);
        }

        const auto dense = static_cast<u32>(m_Dense.size());
        if (dense == m_Capacity)
        {
            Reserve(m_Capacity == 0 ? 8 : m_Capacity * 2);
        }

        m_Sparse[entity.Index] = dense;
        m_Dense.push_back(entity);
        m_ChangeTicks.push_back(0);

        void* slot = SlotData(dense);
        m_Info.DefaultConstruct(slot);
        return slot;
    }

    void ComponentPool::Remove(const Entity entity)
    {
        const u32 dense = FindSlot(entity);
        if (dense == Absent)
        {
            return;
        }

        const auto last = static_cast<u32>(m_Dense.size() - 1);

        if (dense != last)
        {
            // Swap-and-pop: move the tail component into the hole, then patch the
            // sparse mapping for the entity that owned the tail.
            void* hole = SlotData(dense);
            void* tail = SlotData(last);
            m_Info.Destruct(hole);
            m_Info.MoveConstruct(hole, tail);

            const Entity moved = m_Dense[last];
            m_Dense[dense] = moved;
            m_Sparse[moved.Index] = dense;
            m_ChangeTicks[dense] = m_ChangeTicks[last];

            m_Info.Destruct(tail);
        }
        else
        {
            m_Info.Destruct(SlotData(dense));
        }

        m_Sparse[entity.Index] = Absent;
        m_Dense.pop_back();
        m_ChangeTicks.pop_back();
    }
}
