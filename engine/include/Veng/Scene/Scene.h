#pragma once

#include <Veng/Veng.h>
#include <Veng/Assert.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Reflection/TypeRegistry.h>

#include <utility>

namespace Veng
{
    // A runtime ECS world: a generational entity free-list plus one type-erased
    // sparse-set pool per component type that has been Added to an entity. The
    // templated Add/Remove/Get/Has façade resolves T → TypeId through the
    // engine-owned TypeRegistry and forwards to the erased pool, which is
    // created lazily on first Add of a type.
    //
    // Scene is Unique — single owner. Nothing holds a Ref to it; the app owns it
    // and a renderer reads it per frame as a const Scene&. The TypeRegistry it
    // is created with must outlive it and must already have every component type
    // registered.
    class Scene
    {
        // Defined in the impl TU; the public surface never names it.
        class ComponentPool;

        struct EntitySlot
        {
            u32 Generation = 0;
            bool Alive = false;
        };

    public:
        static Unique<Scene> Create(TypeRegistry& registry);

        Scene(const Scene&) = delete;
        Scene& operator=(const Scene&) = delete;
        ~Scene();

        [[nodiscard]] Entity CreateEntity();

        // Removes every component the entity holds (across all pools) and
        // recycles its slot, bumping the slot's generation so existing handles
        // to it go stale.
        void DestroyEntity(Entity entity);

        [[nodiscard]] bool IsAlive(Entity entity) const;

        [[nodiscard]] usize EntityCount() const { return m_LiveCount; }

        template <class T>
        T& Add(Entity entity, T value = {})
        {
            VE_ASSERT(IsAlive(entity), "Add on a dead or stale entity");
            void* slot = AddRaw(entity, m_Registry->IdOf<T>());
            T& component = *static_cast<T*>(slot);
            component = std::move(value);
            return component;
        }

        template <class T>
        void Remove(Entity entity)
        {
            VE_ASSERT(IsAlive(entity), "Remove on a dead or stale entity");
            RemoveRaw(entity, m_Registry->IdOf<T>());
        }

        template <class T>
        [[nodiscard]] T* TryGet(Entity entity)
        {
            VE_ASSERT(IsAlive(entity), "TryGet on a dead or stale entity");
            return static_cast<T*>(TryGetRaw(entity, m_Registry->IdOf<T>()));
        }

        template <class T>
        [[nodiscard]] T& Get(Entity entity)
        {
            T* component = TryGet<T>(entity);
            VE_ASSERT(component != nullptr, "Get on an entity that lacks the component");
            return *component;
        }

        template <class T>
        [[nodiscard]] bool Has(Entity entity) const
        {
            VE_ASSERT(IsAlive(entity), "Has on a dead or stale entity");
            return HasRaw(entity, m_Registry->IdOf<T>());
        }

    private:
        explicit Scene(TypeRegistry& registry);

        // The type-erased façade implementation, defined where ComponentPool is
        // complete. The templated members above resolve T → TypeId and forward
        // here; the IsAlive guard is asserted by the caller.
        void* AddRaw(Entity entity, TypeId id);
        void RemoveRaw(Entity entity, TypeId id);
        void* TryGetRaw(Entity entity, TypeId id);
        [[nodiscard]] bool HasRaw(Entity entity, TypeId id) const;

        // Resolves (creating on first use) the pool for a registered TypeId.
        ComponentPool& PoolFor(TypeId id);
        ComponentPool* TryPoolFor(TypeId id);
        const ComponentPool* TryPoolFor(TypeId id) const;

        TypeRegistry* m_Registry;

        vector<EntitySlot> m_Slots;
        vector<u32> m_FreeIndices;
        usize m_LiveCount = 0;

        // Keyed by TypeId; a Unique pool created lazily on first Add of a type.
        unordered_map<TypeId, Unique<ComponentPool>> m_Pools;
    };
}
