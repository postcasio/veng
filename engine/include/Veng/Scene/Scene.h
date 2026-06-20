#pragma once

#include <Veng/Veng.h>
#include <Veng/Assert.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Reflection/TypeRegistry.h>

#include <array>
#include <utility>

namespace Veng
{
    template <class... Ts>
    class SceneView;

    class Scene;
    struct AABB;
    struct VisibleMesh;
    void ComputeWorldMatrices(const Scene& scene, vector<mat4>& out);
    AABB SceneBounds(const Scene& scene);
    void GatherMeshes(const Scene& scene, vector<VisibleMesh>& out, AABB& outBounds);

    /// @brief Runtime ECS world: a generational entity free-list plus one type-erased sparse-set pool per component type.
    ///
    /// The templated Add/Remove/Get/Has façade resolves T to TypeId through the
    /// TypeRegistry and forwards to the erased pool, created lazily on first Add
    /// of a type. Scene is Unique — single owner; the app owns it and a renderer
    /// reads it per frame as a `const Scene&`. The TypeRegistry it was created
    /// with must outlive it and must already have every component type registered.
    class Scene
    {
        /// @brief Defined in the impl TU; the public surface never names it.
        class ComponentPool;

        /// @brief Slot in the entity table tracking generation and liveness.
        struct EntitySlot
        {
            /// @brief Bumped each time this slot is recycled.
            u32 Generation = 0;
            /// @brief True when an entity occupies this slot.
            bool Alive = false;
        };

    public:
        /// @brief Creates a new Scene backed by the given TypeRegistry.
        static Unique<Scene> Create(TypeRegistry& registry);

        Scene(const Scene&) = delete;
        Scene& operator=(const Scene&) = delete;
        /// @brief Destroys all component pools and entity state.
        ~Scene();

        /// @brief Creates a new entity and returns its handle.
        [[nodiscard]] Entity CreateEntity();

        /// @brief Destroys the entity and all components it holds, recycling its slot.
        ///
        /// Bumps the slot's generation so existing handles to it go stale.
        /// Also recursively destroys the entity's Parent subtree.
        void DestroyEntity(Entity entity);

        /// @brief Returns true if the entity handle is live (not destroyed or stale).
        [[nodiscard]] bool IsAlive(Entity entity) const;

        /// @brief Returns the number of live entities.
        [[nodiscard]] usize EntityCount() const { return m_LiveCount; }

        /// @brief Monotonic counter bumped whenever a spatial pool (Transform, Parent, MeshRenderer) changes.
        ///
        /// A broadphase compares it against the version it last built against:
        /// equal means nothing spatial moved; changed means rebuild. A non-const
        /// access bumps it even when it was a read, so the bump never misses a
        /// write. Read-only consumers use the const View/Each path to avoid bumping.
        [[nodiscard]] u64 GetSpatialVersion() const { return m_SpatialVersion; }

        /// @brief Returns the TypeRegistry this scene was created with.
        ///
        /// The registry its components' descriptors are resolved against; prefab
        /// spawning walks descriptors through it.
        [[nodiscard]] TypeRegistry& GetTypeRegistry() const { return *m_Registry; }

        /// @brief Type-erased add: default-constructs a component of the given TypeId onto the entity.
        ///
        /// The templated Add\<T\> resolves T to TypeId and forwards here; prefab
        /// spawning, which only knows a component's TypeId, calls it directly.
        /// @return Pointer to the new component slot.
        /// @pre The entity must be alive and id must name a registered type.
        void* AddComponent(Entity entity, TypeId id)
        {
            VE_ASSERT(IsAlive(entity), "AddComponent on a dead or stale entity");
            return AddRaw(entity, id);
        }

        /// @brief Adds component T (initialized from value) to the entity and returns a reference to it.
        template <class T>
        T& Add(Entity entity, T value = {})
        {
            VE_ASSERT(IsAlive(entity), "Add on a dead or stale entity");
            void* slot = AddRaw(entity, m_Registry->IdOf<T>());
            T& component = *static_cast<T*>(slot);
            component = std::move(value);
            return component;
        }

        /// @brief Removes component T from the entity.
        template <class T>
        void Remove(Entity entity)
        {
            VE_ASSERT(IsAlive(entity), "Remove on a dead or stale entity");
            RemoveRaw(entity, m_Registry->IdOf<T>());
        }

        /// @brief Returns a pointer to component T on the entity, or nullptr if absent.
        template <class T>
        [[nodiscard]] T* TryGet(Entity entity)
        {
            VE_ASSERT(IsAlive(entity), "TryGet on a dead or stale entity");
            return static_cast<T*>(TryGetRaw(entity, m_Registry->IdOf<T>()));
        }

        /// @brief Returns a const pointer to component T on the entity, or nullptr if absent.
        template <class T>
        [[nodiscard]] const T* TryGet(Entity entity) const
        {
            VE_ASSERT(IsAlive(entity), "TryGet on a dead or stale entity");
            return static_cast<const T*>(TryGetRaw(entity, m_Registry->IdOf<T>()));
        }

        /// @brief Returns a reference to component T on the entity; fatal assert if absent.
        template <class T>
        [[nodiscard]] T& Get(Entity entity)
        {
            T* component = TryGet<T>(entity);
            VE_ASSERT(component != nullptr, "Get on an entity that lacks the component");
            return *component;
        }

        /// @brief Returns true if the entity holds component T.
        template <class T>
        [[nodiscard]] bool Has(Entity entity) const
        {
            VE_ASSERT(IsAlive(entity), "Has on a dead or stale entity");
            return HasRaw(entity, m_Registry->IdOf<T>());
        }

        /// @brief Visits every entity holding all of Ts..., calling fn(entity, Ts&...).
        ///
        /// Drives from the smallest participating pool (no archetype bookkeeping).
        /// Iteration order is the driver pool's dense order. Mutating a component
        /// through its Ts& reference is fine; structural changes (adding/removing
        /// components or destroying entities) during iteration are illegal.
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

                // Fetch each component's storage; resolving all Ts uniformly
                // (including the driver) keeps the code flat.
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

        /// @brief Read-only Each: visits every entity holding all of Ts..., calling fn(entity, const Ts&...).
        ///
        /// Routes through the const TryGetRaw overload only, so a const iteration
        /// never bumps the spatial version. Same intersection and in-iteration
        /// structural-change constraints as the non-const Each.
        template <class... Ts, class Fn>
        void Each(Fn&& fn) const
        {
            static_assert(sizeof...(Ts) > 0, "Each requires at least one component type");

            const std::array<TypeId, sizeof...(Ts)> ids = {m_Registry->IdOf<Ts>()...};

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

                std::array<const void*, sizeof...(Ts)> slots{};
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

                InvokeEachConst<Ts...>(fn, entity, slots, std::index_sequence_for<Ts...>{});
            }
        }

        /// @brief Calls fn(typeId, componentPtr) for every component the entity holds, across all pools.
        ///
        /// Type-erased: the caller resolves each TypeId through the registry to
        /// walk the component's fields without knowing its C++ type at compile time.
        /// Iteration order over pools is unspecified.
        void ForEachComponent(Entity entity, const function<void(TypeId, void*)>& fn);

        /// @brief Range-for form of Each, supporting break/early-out.
        ///
        /// Usage: `for (auto [entity, a, b] : scene.View<A, B>()) { … }`
        /// Same intersection semantics and in-iteration structural-change
        /// constraint as Each.
        template <class... Ts>
        [[nodiscard]] SceneView<Ts...> View()
        {
            return SceneView<Ts...>(*this);
        }

        /// @brief Read-only View: yields (Entity, const Ts&...) without bumping the spatial version.
        template <class... Ts>
        [[nodiscard]] SceneView<const Ts...> View() const
        {
            return SceneView<const Ts...>(*this);
        }

    private:
        explicit Scene(TypeRegistry& registry);

        /// @brief Unpacks slots into typed references and invokes fn.
        template <class... Ts, class Fn, usize... Is>
        static void InvokeEach(Fn&& fn, Entity entity,
                               const std::array<void*, sizeof...(Ts)>& slots,
                               std::index_sequence<Is...>)
        {
            fn(entity, *static_cast<Ts*>(slots[Is])...);
        }

        /// @brief Const overload of InvokeEach: unpacks slots into const typed references.
        template <class... Ts, class Fn, usize... Is>
        static void InvokeEachConst(Fn&& fn, Entity entity,
                                    const std::array<const void*, sizeof...(Ts)>& slots,
                                    std::index_sequence<Is...>)
        {
            fn(entity, *static_cast<const Ts*>(slots[Is])...);
        }

        /// @brief Returns the element count of the pool for id, or 0 if no pool exists.
        ///
        /// Keyed by TypeId so the impl-only ComponentPool stays out of this header.
        [[nodiscard]] usize PoolCount(TypeId id) const;
        /// @brief Returns a pointer to the dense entity array for the pool of id, or nullptr if absent.
        [[nodiscard]] const Entity* DensePtr(TypeId id) const;

        // Type-erased façade; templated members resolve T → TypeId and forward here.
        // IsAlive is asserted by the caller before each of these.
        void* AddRaw(Entity entity, TypeId id);
        void RemoveRaw(Entity entity, TypeId id);
        void* TryGetRaw(Entity entity, TypeId id);
        [[nodiscard]] const void* TryGetRaw(Entity entity, TypeId id) const;
        [[nodiscard]] bool HasRaw(Entity entity, TypeId id) const;

        /// @brief Resolves (creating on first use) the pool for a registered TypeId.
        ComponentPool& PoolFor(TypeId id);
        /// @brief Returns the pool for id, or nullptr if none exists.
        ComponentPool* TryPoolFor(TypeId id);
        /// @brief Returns the pool for id, or nullptr if none exists (const overload).
        const ComponentPool* TryPoolFor(TypeId id) const;

        /// @brief Returns true if id names a spatial pool (Transform, Parent, or MeshRenderer).
        [[nodiscard]] static bool IsSpatialId(TypeId id);
        /// @brief Advances the spatial version counter.
        void BumpSpatial() { ++m_SpatialVersion; }

        /// @brief Borrowed registry; must outlive this Scene.
        TypeRegistry* m_Registry;

        /// @brief Indexed by entity slot index.
        vector<EntitySlot> m_Slots;
        /// @brief Recycled slot indices awaiting reuse.
        vector<u32> m_FreeIndices;
        /// @brief Number of currently live entities.
        usize m_LiveCount = 0;
        /// @brief Monotonic counter for spatial-pool changes.
        u64 m_SpatialVersion = 0;

        /// @brief Component pools, keyed by TypeId, created lazily.
        unordered_map<TypeId, Unique<ComponentPool>> m_Pools;

        template <class...>
        friend class SceneView;

        friend void ComputeWorldMatrices(const Scene& scene, vector<mat4>& out);
        friend AABB SceneBounds(const Scene& scene);
        friend void GatherMeshes(const Scene& scene, vector<VisibleMesh>& out, AABB& outBounds);
    };

    /// @brief Range iterable returned by Scene::View\<Ts...\>().
    ///
    /// begin()/end() yield a forward iterator visiting exactly the entities
    /// holding all of Ts..., in the smallest-pool driver's dense order,
    /// dereferencing to `(Entity, Ts&...)` — so `auto [e, a, b]` works and
    /// `break` stops early. The same in-iteration structural-change constraint
    /// as Each applies.
    ///
    /// Ts may be const-qualified: Scene::View\<Ts...\>() const yields
    /// SceneView\<const Ts...\>, which resolves each component through the const
    /// TryGetRaw and dereferences to `const Ts&` — so a const iteration never
    /// bumps the spatial version.
    template <class... Ts>
    class SceneView
    {
        static_assert(sizeof...(Ts) > 0, "View requires at least one component type");

        // const Scene when any Ts is const (the read-only path binds the const
        // TryGetRaw); a mutable Scene otherwise.
        static constexpr bool AnyConst = (std::is_const_v<Ts> || ...);
        using SceneRef = std::conditional_t<AnyConst, const Scene, Scene>;

    public:
        /// @brief Constructs the view and picks the smallest pool as the iteration driver.
        explicit SceneView(SceneRef& scene) :
            m_Scene(&scene),
            m_Ids{scene.m_Registry->template IdOf<std::remove_const_t<Ts>>()...}
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

        /// @brief Forward iterator over entities matching all Ts... component types.
        class Iterator
        {
        public:
            /// @brief Constructs and skips to the first matching entity.
            Iterator(const SceneView* view, usize index) :
                m_View(view),
                m_Index(index)
            {
                SkipToMatch();
            }

            /// @brief Dereferences to (Entity, Ts&...).
            std::tuple<Entity, Ts&...> operator*() const
            {
                const Entity entity = m_View->m_Dense[m_Index];
                return Resolve(entity, std::index_sequence_for<Ts...>{});
            }

            /// @brief Advances to the next matching entity.
            Iterator& operator++()
            {
                ++m_Index;
                SkipToMatch();
                return *this;
            }

            /// @brief Returns true when the iterators are not at the same position.
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

        /// @brief Returns an iterator to the first matching entity.
        [[nodiscard]] Iterator begin() const { return Iterator(this, 0); }
        /// @brief Returns the past-the-end iterator.
        [[nodiscard]] Iterator end() const { return Iterator(this, m_Count); }

    private:
        /// @brief Returns true if the entity holds all Ts... component types.
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

        /// @brief The scene being iterated.
        SceneRef* m_Scene;
        /// @brief TypeId of each Ts (unqualified).
        std::array<TypeId, sizeof...(Ts)> m_Ids;
        /// @brief The smallest pool's TypeId, driving iteration.
        TypeId m_Driver = InvalidTypeId;
        /// @brief Driver pool element count.
        usize m_Count = 0;
        /// @brief Driver pool's dense entity array.
        const Entity* m_Dense = nullptr;
    };
}
