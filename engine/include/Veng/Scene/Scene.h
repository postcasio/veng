#pragma once

#include <Veng/Veng.h>
#include <Veng/Assert.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Reflection/TypeRegistry.h>

#include <array>
#include <utility>

namespace Veng
{
    // Range-for view over every entity holding all of Ts..., yielding
    // (Entity, Ts&...). Defined after Scene; see Scene::View() below.
    template <class... Ts>
    class SceneView;

    class Scene;
    struct AABB;
    void ComputeWorldMatrices(const Scene& scene, vector<mat4>& out);
    AABB SceneBounds(const Scene& scene);

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

        // The registry this scene was created with — the one its components'
        // descriptors are resolved against. Prefab spawning walks descriptors
        // through it.
        [[nodiscard]] TypeRegistry& GetTypeRegistry() const { return *m_Registry; }

        // Type-erased add: adds a default-constructed component of the given
        // TypeId to the entity and returns its slot. The templated Add<T> resolves
        // T -> TypeId and forwards here; prefab spawning, which only knows a
        // component's TypeId, calls it directly. The id must name a registered
        // type the scene can pool.
        void* AddComponent(Entity entity, TypeId id)
        {
            VE_ASSERT(IsAlive(entity), "AddComponent on a dead or stale entity");
            return AddRaw(entity, id);
        }

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
        [[nodiscard]] const T* TryGet(Entity entity) const
        {
            VE_ASSERT(IsAlive(entity), "TryGet on a dead or stale entity");
            return static_cast<const T*>(TryGetRaw(entity, m_Registry->IdOf<T>()));
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

        // Visits every entity holding all of Ts..., calling fn(entity, Ts&...).
        // The driver is the smallest of the Ts... pools; membership in the rest
        // is tested through their sparse arrays (the standard sparse-set query,
        // no archetype bookkeeping). Iteration order is the driver pool's dense
        // order. Mutating a component through its Ts& is fine; structural changes
        // — adding/removing components or destroying entities — during iteration
        // are illegal (the single-threaded model offers no re-entrancy guard).
        template <class... Ts, class Fn>
        void Each(Fn&& fn)
        {
            static_assert(sizeof...(Ts) > 0, "Each requires at least one component type");

            const std::array<TypeId, sizeof...(Ts)> ids = {m_Registry->IdOf<Ts>()...};

            // Pick the smallest pool to drive iteration. A missing pool has
            // count 0, so the query is empty and visits nothing.
            TypeId driver = ids[0];
            usize best = PoolCount(ids[0]);
            for (usize i = 1; i < ids.size(); ++i)
            {
                const usize count = PoolCount(ids[i]);
                if (count < best)
                {
                    best = count;
                    driver = ids[i];
                }
            }

            const Entity* dense = DensePtr(driver);
            for (usize i = 0; i < best; ++i)
            {
                const Entity entity = dense[i];

                // Fetch each component's storage; skip the entity unless it holds
                // every Ts. The driver's own lookup is one of these and never
                // misses, but resolving all of them uniformly keeps the code flat.
                std::array<void*, sizeof...(Ts)> slots{};
                bool complete = true;
                for (usize t = 0; t < ids.size(); ++t)
                {
                    slots[t] = TryGetRaw(entity, ids[t]);
                    if (slots[t] == nullptr)
                    {
                        complete = false;
                        break;
                    }
                }
                if (!complete)
                {
                    continue;
                }

                InvokeEach<Ts...>(fn, entity, slots, std::index_sequence_for<Ts...>{});
            }
        }

        // Calls fn(typeId, componentPtr) for every component the entity holds,
        // across all pools. Type-erased: the caller resolves each TypeId through
        // the registry to walk the component's fields without knowing its C++
        // type at compile time (the inspector's driver). Iteration order over the
        // pools is unspecified.
        void ForEachComponent(Entity entity, const function<void(TypeId, void*)>& fn);

        // Range-for form of Each, for code that wants break/early-out:
        //   for (auto [entity, a, b] : scene.View<A, B>()) { … }
        // Same intersection semantics and same in-iteration structural-change
        // constraint as Each.
        template <class... Ts>
        [[nodiscard]] SceneView<Ts...> View()
        {
            return SceneView<Ts...>(*this);
        }

    private:
        explicit Scene(TypeRegistry& registry);

        template <class... Ts, class Fn, usize... Is>
        static void InvokeEach(Fn&& fn, Entity entity,
                               const std::array<void*, sizeof...(Ts)>& slots,
                               std::index_sequence<Is...>)
        {
            fn(entity, *static_cast<Ts*>(slots[Is])...);
        }

        // Dense-iteration access for the query templates, keyed by TypeId so the
        // impl-only ComponentPool stays out of this header. PoolCount is 0 and
        // DensePtr is nullptr when no pool exists for the id.
        [[nodiscard]] usize PoolCount(TypeId id) const;
        [[nodiscard]] const Entity* DensePtr(TypeId id) const;

        // The type-erased façade implementation, defined where ComponentPool is
        // complete. The templated members above resolve T → TypeId and forward
        // here; the IsAlive guard is asserted by the caller.
        void* AddRaw(Entity entity, TypeId id);
        void RemoveRaw(Entity entity, TypeId id);
        void* TryGetRaw(Entity entity, TypeId id);
        [[nodiscard]] const void* TryGetRaw(Entity entity, TypeId id) const;
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

        template <class...>
        friend class SceneView;

        // The transform walk enumerates the Transform pool's dense entity list
        // through the impl-only pool accessors.
        friend void ComputeWorldMatrices(const Scene& scene, vector<mat4>& out);

        // SceneBounds aligns the world matrices with the Transform pool's dense
        // entity list through the same impl-only pool accessors.
        friend AABB SceneBounds(const Scene& scene);
    };

    // The iterable returned by Scene::View<Ts...>(). begin()/end() yield a
    // forward iterator that visits exactly the entities holding all of Ts...,
    // in the smallest-pool driver's dense order, dereferencing to a tuple
    // (Entity, Ts&...) — so a structured binding `auto [e, a, b]` works and
    // `break` stops early. The same in-iteration structural-change constraint as
    // Each applies.
    template <class... Ts>
    class SceneView
    {
        static_assert(sizeof...(Ts) > 0, "View requires at least one component type");

    public:
        explicit SceneView(Scene& scene) :
            m_Scene(&scene),
            m_Ids{scene.m_Registry->IdOf<Ts>()...}
        {
            // Drive from the smallest pool; a missing pool (count 0) yields an
            // empty range.
            m_Driver = m_Ids[0];
            usize best = scene.PoolCount(m_Ids[0]);
            for (usize i = 1; i < m_Ids.size(); ++i)
            {
                const usize count = scene.PoolCount(m_Ids[i]);
                if (count < best)
                {
                    best = count;
                    m_Driver = m_Ids[i];
                }
            }
            m_Count = best;
            m_Dense = scene.DensePtr(m_Driver);
        }

        class Iterator
        {
        public:
            Iterator(const SceneView* view, usize index) :
                m_View(view),
                m_Index(index)
            {
                SkipToMatch();
            }

            std::tuple<Entity, Ts&...> operator*() const
            {
                const Entity entity = m_View->m_Dense[m_Index];
                return Resolve(entity, std::index_sequence_for<Ts...>{});
            }

            Iterator& operator++()
            {
                ++m_Index;
                SkipToMatch();
                return *this;
            }

            bool operator!=(const Iterator& other) const { return m_Index != other.m_Index; }

        private:
            // Advance past driver entries that lack one of the other components,
            // landing on the next full match (or on m_Count, the end).
            void SkipToMatch()
            {
                while (m_Index < m_View->m_Count
                       && !m_View->Matches(m_View->m_Dense[m_Index]))
                {
                    ++m_Index;
                }
            }

            template <usize... Is>
            std::tuple<Entity, Ts&...> Resolve(Entity entity, std::index_sequence<Is...>) const
            {
                return std::tuple<Entity, Ts&...>(
                    entity,
                    *static_cast<Ts*>(m_View->m_Scene->TryGetRaw(entity, m_View->m_Ids[Is]))...);
            }

            const SceneView* m_View;
            usize m_Index;
        };

        [[nodiscard]] Iterator begin() const { return Iterator(this, 0); }
        [[nodiscard]] Iterator end() const { return Iterator(this, m_Count); }

    private:
        [[nodiscard]] bool Matches(Entity entity) const
        {
            for (const TypeId id : m_Ids)
            {
                if (m_Scene->TryGetRaw(entity, id) == nullptr)
                {
                    return false;
                }
            }
            return true;
        }

        Scene* m_Scene;
        std::array<TypeId, sizeof...(Ts)> m_Ids;
        TypeId m_Driver = InvalidTypeId;
        usize m_Count = 0;
        const Entity* m_Dense = nullptr;
    };
}
