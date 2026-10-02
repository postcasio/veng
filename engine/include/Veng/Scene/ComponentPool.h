#pragma once

#include <Veng/Veng.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Entity.h>

#include <array>

namespace Veng::Detail
{
    /// @brief Type-erased sparse-set storage for one component type: the store a Scene owns per pooled type.
    ///
    /// Stores raw bytes sized by `TypeInfo::Size` and manipulates them only through the type's
    /// lifecycle thunks (default-construct, destruct, move-construct), so a component that is not
    /// trivially relocatable — one holding a pointer into itself — stays valid as the pool grows and
    /// as Remove compacts it. Scene's templated façade is the only caller; the class is
    /// header-visible so the per-access lookup inlines into the calling unit rather than costing a
    /// call per component.
    ///
    /// Layout: `m_Sparse` maps entity index → dense slot (Absent if none); `m_Dense` is the packed
    /// entity list, which is the order a query visits; the component bytes are packed in the same
    /// order (`Count * Info.Size`), aligned to the type. Remove is swap-and-pop: the tail element
    /// moves into the hole and its sparse entry is patched.
    class ComponentPool
    {
    public:
        /// @brief The dense slot FindSlot reports for an entity the pool does not hold.
        static constexpr u32 Absent = ~0u;

        /// @brief How a non-const access to this pool's components moves the owning scene's versions.
        enum class AccessVersion : u8
        {
            /// @brief The access moves no version.
            None,
            /// @brief The access moves the spatial version.
            Spatial,
            /// @brief The access moves the topology version, and the spatial version with it.
            Topology,
        };

        /// @brief Constructs an empty pool for the component type described by @p info.
        /// @param info    The component type's registered description; must outlive the pool.
        /// @param access  How a non-const access to a component here moves the scene's versions.
        ComponentPool(const TypeInfo& info, AccessVersion access);

        /// @brief Destructs every stored component through the type's destruct thunk and frees the storage.
        ~ComponentPool();

        ComponentPool(const ComponentPool&) = delete;
        ComponentPool& operator=(const ComponentPool&) = delete;
        ComponentPool(ComponentPool&&) = delete;
        ComponentPool& operator=(ComponentPool&&) = delete;

        /// @brief Default-constructs a component for the entity and returns its storage.
        ///
        /// Growing the storage move-constructs every element into the new allocation, so a pointer
        /// into a component (held by the component itself or by a caller) is invalidated, exactly as
        /// for a vector.
        /// @param entity  The entity to add the component to.
        /// @return The new component's storage.
        /// @pre The entity has no component of this type; asserts otherwise.
        void* Add(Entity entity);

        /// @brief Destructs and swap-and-pops the entity's component. No-op if absent.
        /// @param entity  The entity whose component to remove.
        void Remove(Entity entity);

        /// @brief Returns the dense slot holding the entity's component, or Absent.
        ///
        /// The one membership check every access makes: the sparse entry for the entity's index,
        /// then a compare of the whole handle stored in that dense slot, so a stale generation
        /// reads as absent.
        /// @param entity  The entity to look up.
        /// @return The entity's dense slot, or Absent when the pool does not hold it.
        [[nodiscard]] u32 FindSlot(const Entity entity) const
        {
            if (entity.Index >= m_Sparse.size())
            {
                return Absent;
            }
            const u32 slot = m_Sparse[entity.Index];
            return slot < m_Dense.size() && m_Dense[slot] == entity ? slot : Absent;
        }

        /// @brief Returns true if the entity has a component in this pool.
        /// @param entity  The entity to test.
        [[nodiscard]] bool Contains(const Entity entity) const
        {
            return FindSlot(entity) != Absent;
        }

        /// @brief Returns the storage of the component in dense slot @p slot.
        /// @param slot  A dense slot below Count().
        [[nodiscard]] void* SlotData(const u32 slot) { return m_Data + (slot * m_Stride); }

        /// @brief Returns the const storage of the component in dense slot @p slot.
        /// @param slot  A dense slot below Count().
        [[nodiscard]] const void* SlotData(const u32 slot) const
        {
            return m_Data + (slot * m_Stride);
        }

        /// @brief Returns a storage pointer for the entity's component, or nullptr if absent.
        /// @param entity  The entity to look up.
        [[nodiscard]] void* TryGet(const Entity entity)
        {
            const u32 slot = FindSlot(entity);
            return slot != Absent ? SlotData(slot) : nullptr;
        }

        /// @brief Returns a const storage pointer for the entity's component, or nullptr if absent.
        /// @param entity  The entity to look up.
        [[nodiscard]] const void* TryGet(const Entity entity) const
        {
            const u32 slot = FindSlot(entity);
            return slot != Absent ? SlotData(slot) : nullptr;
        }

        /// @brief Number of components currently stored.
        [[nodiscard]] usize Count() const { return m_Dense.size(); }

        /// @brief The packed entity list — the iteration order a query drives.
        /// @warning Pointer is invalidated by any structural change to this pool.
        [[nodiscard]] const Entity* DenseData() const { return m_Dense.data(); }

        /// @brief Records @p tick as the change tick of the component in dense slot @p slot.
        ///
        /// The per-entity twin of Scene's spatial version: a non-const access stamps the accessed
        /// component with the current sim tick, so the net layer's dirty query (change tick >
        /// last-acked tick) sends exactly what changed since a connection last acked.
        /// @param slot  A dense slot below Count().
        /// @param tick  The tick to record.
        void StampSlot(const u32 slot, const u64 tick) { m_ChangeTicks[slot] = tick; }

        /// @brief Returns the entity's component change tick, or 0 if the entity has no component here.
        /// @param entity  The entity to look up.
        [[nodiscard]] u64 ChangeTick(const Entity entity) const
        {
            const u32 slot = FindSlot(entity);
            return slot != Absent ? m_ChangeTicks[slot] : 0;
        }

        /// @brief Returns the description of the component type this pool stores.
        [[nodiscard]] const TypeInfo& GetInfo() const { return m_Info; }

        /// @brief Returns how a non-const access to a component here moves the owning scene's versions.
        [[nodiscard]] AccessVersion GetAccessVersion() const { return m_Access; }

    private:
        /// @brief Reallocates the component storage to hold at least @p capacity elements.
        ///
        /// Move-constructs each element into the new storage and destructs the original, so a
        /// component's move constructor sees every relocation.
        /// @param capacity  The element count the new storage must hold.
        void Reserve(usize capacity);

        /// @brief Borrowed reference to the component type's descriptor (size, thunks, name).
        const TypeInfo& m_Info;
        /// @brief Bytes between consecutive components: the type's size.
        usize m_Stride;
        /// @brief How a non-const access to a component here moves the owning scene's versions.
        AccessVersion m_Access;

        /// @brief entity index → dense slot (Absent if none).
        vector<u32> m_Sparse;
        /// @brief dense slot → entity.
        vector<Entity> m_Dense;
        /// @brief dense slot → component bytes, aligned to the type; holds m_Capacity elements.
        std::byte* m_Data = nullptr;
        /// @brief The element count m_Data has room for.
        usize m_Capacity = 0;
        /// @brief dense slot → last sim tick this component was stamped at (parallel to m_Dense).
        ///
        /// Swap-and-popped with m_Dense on Remove so it stays aligned.
        vector<u64> m_ChangeTicks;
    };

    /// @brief Picks the smallest of a query's pools to drive iteration.
    ///
    /// A query visits its driver's dense entries and checks the other pools for each, so the
    /// smallest pool bounds the work. A missing pool means no entity can match.
    /// @tparam Pool  ComponentPool, const-qualified or not.
    /// @tparam N     The number of pools in the query.
    /// @param pools  The query's pools, one per component type; null where the scene has none.
    /// @return The driver's index into @p pools, or N when a pool is missing.
    template <class Pool, usize N>
    [[nodiscard]] usize SelectDriver(const std::array<Pool*, N>& pools)
    {
        usize driver = 0;
        for (usize t = 0; t < N; ++t)
        {
            if (pools[t] == nullptr)
            {
                return N;
            }
            if (pools[t]->Count() < pools[driver]->Count())
            {
                driver = t;
            }
        }
        return driver;
    }

    /// @brief Finds an entity's dense slot in every pool of a query, or reports that one lacks it.
    ///
    /// The driver's slot is the dense index being visited, so only the other pools are searched.
    /// @tparam Pool  ComponentPool, const-qualified or not.
    /// @tparam N     The number of pools in the query.
    /// @param pools       The query's pools, none null.
    /// @param driver      The index of the driving pool in @p pools.
    /// @param driverSlot  The dense slot being visited in the driving pool.
    /// @param entity      The entity in that slot.
    /// @param slots       Receives the entity's slot in each pool.
    /// @return True when every pool holds the entity.
    template <class Pool, usize N>
    [[nodiscard]] bool FindQuerySlots(const std::array<Pool*, N>& pools, const usize driver,
                                      const u32 driverSlot, const Entity entity,
                                      std::array<u32, N>& slots)
    {
        for (usize t = 0; t < N; ++t)
        {
            slots[t] = t == driver ? driverSlot : pools[t]->FindSlot(entity);
            if (slots[t] == ComponentPool::Absent)
            {
                return false;
            }
        }
        return true;
    }
}
