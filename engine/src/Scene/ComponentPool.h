#pragma once

#include <Veng/Veng.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Reflection/TypeRegistry.h>

namespace Veng
{
    /// @brief Type-erased sparse-set storage for one component type.
    ///
    /// Stores raw bytes sized by `TypeInfo::Size` and manipulates them through the
    /// type's lifecycle thunks (default-construct, destruct, move-construct).
    /// Internal to the engine — `Scene`'s templated façade is the only caller.
    ///
    /// Layout: `m_Sparse` maps entity index → dense slot (Tombstone if absent);
    /// `m_Dense` is the packed entity list (the query iteration order); `m_Data`
    /// is parallel packed component bytes (`Count * Info.Size`).
    /// `Remove` is swap-and-pop: tail element moves into the hole, sparse entry patched.
    class Scene::ComponentPool
    {
    public:
        /// @brief Constructs an empty pool for the component type described by @p info.
        explicit ComponentPool(const TypeInfo& info);

        /// @brief Destructs every stored component through the type's destruct thunk.
        ~ComponentPool();

        ComponentPool(const ComponentPool&) = delete;
        ComponentPool& operator=(const ComponentPool&) = delete;

        /// @brief Default-constructs a component for the entity and returns its storage.
        /// @pre The entity has no component of this type; asserts otherwise.
        void* Add(Entity entity);

        /// @brief Destructs and swap-and-pops the entity's component. No-op if absent.
        void Remove(Entity entity);

        /// @brief Returns true if the entity has a component in this pool.
        [[nodiscard]] bool Contains(Entity entity) const;

        /// @brief Returns a storage pointer for the entity's component, or nullptr if absent.
        [[nodiscard]] void* TryGet(Entity entity);
        /// @brief Returns a const storage pointer for the entity's component, or nullptr if absent.
        [[nodiscard]] const void* TryGet(Entity entity) const;

        /// @brief Number of components currently stored.
        [[nodiscard]] usize Count() const { return m_Dense.size(); }

        /// @brief The packed entity list — the iteration order a query drives.
        /// @warning Pointer is invalidated by any structural change to this pool.
        [[nodiscard]] const Entity* DenseData() const { return m_Dense.data(); }

        /// @brief Records @p tick as the change tick of the entity's component. No-op if absent.
        ///
        /// The per-entity twin of Scene's spatial version: a non-const access stamps the accessed
        /// entity's component with the current sim tick, so the net layer's dirty query
        /// (change tick > last-acked tick) sends exactly what changed since a connection last acked.
        void Stamp(Entity entity, u64 tick);

        /// @brief Returns the entity's component change tick, or 0 if the entity has no component here.
        [[nodiscard]] u64 ChangeTick(Entity entity) const;

    private:
        /// @brief Sentinel for an absent sparse entry.
        static constexpr u32 Tombstone = ~0u;

        /// @brief Byte address of dense slot `index`.
        [[nodiscard]] void* DataAt(usize index);

        /// @brief Borrowed reference to the component type's descriptor (size, thunks, name).
        const TypeInfo& m_Info;

        /// @brief entity index → dense slot (Tombstone if none)
        vector<u32> m_Sparse;
        /// @brief dense slot → entity
        vector<Entity> m_Dense;
        /// @brief dense slot → component bytes (Count * Size)
        vector<std::byte> m_Data;
        /// @brief dense slot → last sim tick this component was stamped at (parallel to m_Dense).
        ///
        /// Swap-and-popped with m_Dense on Remove so it stays aligned. Zero for a component never
        /// stamped since it was added at tick zero (before any sim tick).
        vector<u64> m_ChangeTicks;
    };
}
