#pragma once

#include <Veng/Veng.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Reflection/TypeRegistry.h>

namespace Veng
{
    // Type-erased sparse-set storage for one component type. Component types are
    // runtime-registered, so the pool stores raw bytes sized by TypeInfo::Size
    // and manipulates them through the type's lifecycle thunks (default-
    // construct, destruct, move-construct). Internal to the engine — Scene's
    // templated façade is the only caller.
    //
    // Classic sparse-set:
    //   - m_Sparse:  entity index → dense slot (Tombstone if absent)
    //   - m_Dense:   packed entity list, the iteration order
    //   - m_Data:    packed component bytes, Count * Info.Size, parallel to Dense
    // Remove is swap-and-pop: move the tail element into the hole, destruct the
    // tail, patch the sparse entries.
    class Scene::ComponentPool
    {
    public:
        explicit ComponentPool(const TypeInfo& info);
        ~ComponentPool();

        ComponentPool(const ComponentPool&) = delete;
        ComponentPool& operator=(const ComponentPool&) = delete;

        // Default-constructs a component for the entity and returns its storage.
        // Asserts the entity has no component of this type yet.
        void* Add(Entity entity);

        // Destructs + swap-and-pops the entity's component. No-op if absent.
        void Remove(Entity entity);

        [[nodiscard]] bool Contains(Entity entity) const;

        // Storage pointer for the entity's component, or nullptr if absent.
        [[nodiscard]] void* TryGet(Entity entity);
        [[nodiscard]] const void* TryGet(Entity entity) const;

        [[nodiscard]] usize Count() const { return m_Dense.size(); }

    private:
        static constexpr u32 Tombstone = ~0u;

        // Byte address of dense slot `index`.
        [[nodiscard]] void* DataAt(usize index);

        const TypeInfo& m_Info;

        vector<u32> m_Sparse;       // entity index → dense slot (Tombstone if none)
        vector<Entity> m_Dense;     // dense slot → entity
        vector<std::byte> m_Data;   // dense slot → component bytes (Count * Size)
    };
}
