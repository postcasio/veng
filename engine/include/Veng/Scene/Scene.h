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
    class SceneSimulation;
    struct SystemContext;
    struct AABB;
    struct Hierarchy;
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

        /// @brief Deep-copies this scene into a new, independent Scene.
        ///
        /// Recreates every live entity, copies every component via the reflection
        /// serializer, remaps intra-scene Entity reference fields from old to new
        /// handles, and rebuilds the Hierarchy parent/child/sibling links so the
        /// topology matches exactly. AssetHandle fields are deep-copied directly, so
        /// a runtime-adopted (id-less) handle stays resident in the clone. The clone
        /// borrows the same TypeRegistry and is wholly independent of this scene.
        [[nodiscard]] Unique<Scene> Clone() const;

        Scene(const Scene&) = delete;
        Scene& operator=(const Scene&) = delete;
        /// @brief Destroys all component pools and entity state.
        ~Scene();

        /// @brief Creates a new entity and returns its handle.
        [[nodiscard]] Entity CreateEntity();

        /// @brief Destroys the entity and all components it holds, recycling its slot.
        ///
        /// Bumps the slot's generation so existing handles to it go stale.
        /// Recursively destroys the entity's whole Hierarchy subtree, walking the
        /// FirstChild → NextSibling links in O(subtree). Detaches the destroyed
        /// root from any surviving parent's child list first, so siblings stay
        /// consistent.
        void DestroyEntity(Entity entity);

        /// @brief Reparents `child` under `parent`, appending it to `parent`'s child list.
        ///
        /// Detaches `child` from its current sibling list, then links it as the
        /// last child of `parent`, maintaining all four Hierarchy links in O(1).
        /// Adds a Hierarchy component to `child` (and to `parent` when non-null) if
        /// absent. Passing Entity::Null as `parent` reparents `child` to the root
        /// (clears its up-link and detaches it from siblings). Bumps the spatial
        /// version.
        /// @param child   The entity to reparent; must be alive.
        /// @param parent  The new parent, or Entity::Null for the root; must be alive when non-null.
        /// @pre `parent` is not a descendant of `child`.
        /// @warning A cycle (`parent` a descendant of `child`) is API misuse and a fatal VE_ASSERT.
        void SetParent(Entity child, Entity parent);

        /// @brief Detaches `child` from its parent, reparenting it to the root.
        ///
        /// Equivalent to SetParent(child, Entity::Null): clears the up-link and
        /// unlinks `child` from its sibling list. Bumps the spatial version.
        /// @param child  The entity to detach; must be alive.
        void Detach(Entity child);

        /// @brief Re-links `child` immediately before `sibling` in `sibling`'s parent's child list.
        ///
        /// The editor's drag-reorder / insert-at primitive: reparents `child`
        /// under `sibling`'s parent if they differ, then inserts it directly
        /// before `sibling` in the ordered child list, maintaining all links in
        /// O(1). Bumps the spatial version.
        /// @param child    The entity to move; must be alive.
        /// @param sibling  The entity to insert before; must be alive and non-null.
        /// @pre `sibling`'s parent is not a descendant of `child`.
        /// @warning A cycle is API misuse and a fatal VE_ASSERT.
        void MoveBefore(Entity child, Entity sibling);

        /// @brief Returns the parent of `entity`, or Entity::Null if it is a root or has no Hierarchy.
        /// @param entity  The entity to query; must be alive.
        [[nodiscard]] Entity GetParent(Entity entity) const;

        /// @brief Visits each direct child of `entity` in insertion order, calling fn(child).
        ///
        /// Walks the sibling list FirstChild → NextSibling…; O(children). Visits
        /// nothing for a leaf or an entity with no Hierarchy. The visitor must not
        /// mutate the topology of `entity`'s child list during the walk.
        /// @param entity  The entity whose children to visit; must be alive.
        /// @param fn      Visitor invoked once per direct child.
        void ForEachChild(Entity entity, const function<void(Entity)>& fn) const;

        /// @brief Returns true if the entity handle is live (not destroyed or stale).
        [[nodiscard]] bool IsAlive(Entity entity) const;

        /// @brief Visits every live entity, calling fn(entity) in slot-index order.
        ///
        /// Enumerates entities regardless of which components they hold — the
        /// whole-world walk a hierarchy view needs, distinct from the
        /// component-keyed View/Each. The visitor must not create or destroy
        /// entities; structural changes during the walk are illegal.
        /// @param fn  Visitor invoked once per live entity.
        void ForEachEntity(const function<void(Entity)>& fn) const;

        /// @brief Returns the number of live entities.
        [[nodiscard]] usize EntityCount() const { return m_LiveCount; }

        /// @brief Monotonic counter bumped whenever a spatial pool (Transform, Hierarchy, MeshRenderer) changes.
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

        /// @brief Attaches (or replaces) the simulation that drives this scene's systems.
        ///
        /// A Scene optionally owns the SceneSimulation that runs over it: Level::LoadInto builds
        /// one from the level's ordered system set and attaches it here, and the editor's Play
        /// clone attaches its own. Passing a null pointer detaches and destroys the held one. The
        /// scene drives it through Start/Tick/StopSimulation, which forward `*this`.
        /// @param simulation  The simulation to own, or null to detach.
        void SetSimulation(Unique<SceneSimulation> simulation);

        /// @brief Returns the attached simulation, or null when the scene has none.
        [[nodiscard]] SceneSimulation* GetSimulation() const { return m_Simulation.get(); }

        /// @brief Starts the attached simulation over this scene; a no-op when none is attached.
        ///
        /// Forwards to SceneSimulation::Start(*this, context) — calls OnStart on each system.
        /// @param context  Per-tick services forwarded to each system.
        void StartSimulation(const SystemContext& context);

        /// @brief Advances the attached simulation one tick over this scene; a no-op when none.
        ///
        /// Forwards to SceneSimulation::Update(*this, delta, context) — the Sim-then-View phase pass.
        /// @param delta    Time in seconds since the previous tick.
        /// @param context  Per-tick services forwarded to each system.
        void TickSimulation(f32 delta, const SystemContext& context);

        /// @brief Stops the attached simulation over this scene; a no-op when none is attached.
        ///
        /// Forwards to SceneSimulation::Stop(*this, context) — calls OnStop on each system.
        /// @param context  Per-tick services forwarded to each system.
        void StopSimulation(const SystemContext& context);

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

        /// @brief Type-erased remove: removes the component of the given TypeId from the entity.
        ///
        /// The templated Remove\<T\> resolves T to TypeId and forwards here; the
        /// editor inspector, which only knows a component's TypeId, calls it
        /// directly. A no-op when the entity lacks the component.
        /// @pre The entity must be alive.
        void RemoveComponent(Entity entity, TypeId id)
        {
            VE_ASSERT(IsAlive(entity), "RemoveComponent on a dead or stale entity");
            RemoveRaw(entity, id);
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

        /// @brief Type-erased component fetch: the storage for `id` on `entity`, or nullptr if absent.
        ///
        /// The TypeId sibling of TryGet\<T\>; used by the spawn-resolve pass, which
        /// fetches a component fresh by TypeId at fire time (a resolver may Add a
        /// component, dangling a held pool pointer across the pool growth).
        /// @param entity  The entity to query; must be alive.
        /// @param id      The TypeId of the component to fetch.
        /// @return The component's storage, or nullptr if the entity lacks it.
        [[nodiscard]] void* TryGetComponent(Entity entity, TypeId id)
        {
            VE_ASSERT(IsAlive(entity), "TryGetComponent on a dead or stale entity");
            return TryGetRaw(entity, id);
        }

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

        /// @brief Returns true if id names a spatial pool (Transform, Hierarchy, or MeshRenderer).
        [[nodiscard]] static bool IsSpatialId(TypeId id);
        /// @brief Advances the spatial version counter.
        void BumpSpatial() { ++m_SpatialVersion; }

        /// @brief Returns the entity's Hierarchy component, creating it if absent.
        ///
        /// Used by the topology operations to materialize the link record on first
        /// attach. Bumps the spatial version when it adds the component.
        Hierarchy& HierarchyOf(Entity entity);
        /// @brief Returns the entity's Hierarchy component, or nullptr if it has none.
        [[nodiscard]] const Hierarchy* TryHierarchy(Entity entity) const;
        /// @brief Unlinks `child` from its current parent's child list, leaving its Parent edge intact.
        void UnlinkFromSiblings(Entity child);
        /// @brief Returns true if `candidate` is `entity` or one of its descendants.
        [[nodiscard]] bool IsDescendantOf(Entity candidate, Entity entity) const;

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

        /// @brief The simulation driving this scene's systems, or null when none is attached.
        Unique<SceneSimulation> m_Simulation;

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
        explicit SceneView(SceneRef& scene)
            : m_Scene(&scene), m_Ids{scene.m_Registry->template IdOf<std::remove_const_t<Ts>>()...}
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
            Iterator(const SceneView* view, usize index) : m_View(view), m_Index(index)
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
                while (m_Index < m_View->m_Count && !m_View->Matches(m_View->m_Dense[m_Index]))
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
