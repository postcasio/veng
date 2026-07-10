#pragma once

#include <Veng/Veng.h>
#include <Veng/Assert.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Scene/SceneSystem.h>
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

        /// @brief Recreates a live entity occupying the exact slot index and generation of @p entity.
        ///
        /// The respawn counterpart of DestroyEntity for an undo stack: it restores the precise
        /// handle a prior DestroyEntity recycled (the bumped generation included), so any captured
        /// handle to that entity — held by another undo entry, or by a Reference field pointing at
        /// it — stays valid across a destroy→undo→redo cycle. A fresh-handle CreateEntity would
        /// leave those captures dangling. The slot must be free (dead or never allocated); its
        /// generation is set to @p entity's, undoing the bump DestroyEntity applied — so a handle
        /// this scene once handed out and then destroyed becomes live again exactly as captured.
        /// Grows the slot table and skips intermediate slots onto the free list if the index runs
        /// past the current table. Adds no components — the caller repopulates them — so it bumps
        /// no spatial version on its own.
        /// @param entity  The exact handle (slot index + generation) to recreate; its slot must be free.
        /// @return @p entity, now alive.
        Entity CreateEntityAt(Entity entity);

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

        /// @brief Returns the live entity occupying slot @p index, or Entity::Null if none.
        ///
        /// Resolves a bare slot index (as carried by an id-buffer pick readback) back to a
        /// generational handle: the live occupant with the slot's current generation, or
        /// Entity::Null when the index is out of range or the slot is dead. The pick id is the
        /// packed index + 1, so picking subtracts 1 and resolves through this, validating
        /// liveness late — a recycled slot resolves to its live occupant or to none.
        /// @param index  The slot index to resolve.
        /// @return The live entity at the slot, or Entity::Null.
        [[nodiscard]] Entity GetLiveEntityAtIndex(u32 index) const;

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

        /// @brief Sets the sim tick that a non-const component access stamps as its change tick.
        ///
        /// The world drive sets this to SystemContext::Tick each phase, so an in-place edit during a
        /// tick stamps that tick onto the touched (entity, component). Defaults to zero (edits before
        /// the first tick — level load, editor authoring — stamp tick zero). The net layer's dirty
        /// query keys off the resulting per-entity change ticks.
        /// @param tick  The tick value non-const accesses now stamp.
        void SetChangeTick(u64 tick) { m_ChangeTick = tick; }

        /// @brief Returns the sim tick non-const accesses currently stamp (see SetChangeTick).
        [[nodiscard]] u64 GetChangeTick() const { return m_ChangeTick; }

        /// @brief Returns the last tick component @p id on @p entity was stamped at, or 0 if absent.
        ///
        /// A component enters a snapshot for a connection when this exceeds the connection's last-acked
        /// tick (the send-until-acked dirty rule). A const query — it never stamps.
        /// @param entity  The entity to query; must be alive.
        /// @param id      The component TypeId to query.
        /// @return The component's change tick, or 0 when the entity lacks the component.
        [[nodiscard]] u64 GetComponentChangeTick(Entity entity, TypeId id) const;

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

        /// @brief Runs one phase of the attached simulation, snapshotting transform history after Sim.
        ///
        /// The fixed-timestep drive calls this once per fixed step for Phase::Sim (advancing the tick)
        /// and once per frame for Phase::View (carrying the interpolation alpha). After a Sim phase it
        /// snapshots the scene's spatial state into the transform-history ring, so the render gather
        /// and View systems can interpolate between the last two ticks. A no-op when no simulation is
        /// attached (the snapshot still runs, capturing the static pose).
        /// @param phase    The phase whose systems run.
        /// @param delta    Time in seconds forwarded to each system's OnUpdate.
        /// @param context  Per-tick services forwarded to each system.
        void TickSimulationPhase(SceneSystem::Phase phase, f32 delta, const SystemContext& context);

        /// @brief Snapshots every Transform entity's local TRS into the two-tick history ring.
        ///
        /// Called at the end of each Sim tick (through TickSimulationPhase). Rolls the previous
        /// snapshot to make room for the current one, keyed off the spatial version so a static scene
        /// copies nothing after it converges. GetInterpolatedWorldTransform blends the two by a
        /// frame's alpha.
        void SnapshotTransformHistory();

        /// @brief Returns an entity's world matrix, interpolating its TRS between the last two Sim ticks.
        ///
        /// Walks the Hierarchy chain like WorldMatrix, but composes each level's local matrix from the
        /// two-tick history blended by @p alpha (previous → current). An entity with no history entry
        /// (never snapshotted) falls back to its live Transform, so the result equals WorldMatrix when
        /// no interpolation applies. A View system (a camera rig) reads this so the camera and the
        /// meshes it frames share one interpolated pose.
        /// @param entity  The entity to resolve.
        /// @param alpha   The interpolation fraction in [0, 1] (0 = previous tick, 1 = current).
        /// @return The interpolated world matrix.
        [[nodiscard]] mat4 GetInterpolatedWorldTransform(Entity entity, f32 alpha) const;

        /// @brief Returns whether the history holds motion to interpolate (false for a static scene).
        ///
        /// True from the first Sim tick that moved a transform until the scene converges to rest; the
        /// render gather skips its interpolation copy when this is false, keeping a static scene's
        /// draw byte-identical to the un-interpolated path.
        [[nodiscard]] bool HasTransformInterpolation() const { return m_HistoryDirty; }

        /// @brief Stops the attached simulation over this scene; a no-op when none is attached.
        ///
        /// Forwards to SceneSimulation::Stop(*this, context) — calls OnStop on each system.
        /// @param context  Per-tick services forwarded to each system.
        void StopSimulation(const SystemContext& context);

        /// @brief Binds this scene to a simulation drive-list it self-unregisters from on destruction.
        ///
        /// Application::RegisterSimulation calls this after appending this scene's pointer to its
        /// drive-list; ~Scene then erases that pointer, order-preserving. Registering an
        /// already-registered scene is a fatal assert (the back-reference would leak the prior
        /// membership). The back-reference is not a component and is not copied by Clone, so a clone
        /// starts unregistered.
        /// @param driveList  The Application simulation drive-list this scene now belongs to.
        /// @pre This scene is not already attached to a simulation drive-list.
        void AttachToSimDriveList(vector<Scene*>& driveList);

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

        /// @brief Returns the first component of type T in the scene, or nullptr if none exists.
        ///
        /// The lookup for world-scoped config held on an unspecified "settings" entity: a consumer
        /// queries the component **type**, not a well-known entity, so the config can live on any
        /// entity (a level seeds one, a prefab authors one). One such component is the expected
        /// case and is returned; with several, the first in pool order wins and the rest are
        /// ignored — a loose convention, not an enforced singleton. O(1).
        /// @tparam T  The component type to find.
        /// @return Pointer to the first T, or nullptr when the scene has none.
        template <class T>
        [[nodiscard]] T* TryGetFirst()
        {
            const TypeId id = m_Registry->IdOf<T>();
            if (PoolCount(id) == 0)
            {
                return nullptr;
            }
            return static_cast<T*>(TryGetRaw(DensePtr(id)[0], id));
        }

        /// @brief Returns a const pointer to the first component of type T, or nullptr if none.
        ///
        /// The const counterpart of the mutable TryGetFirst; it never bumps the spatial version,
        /// so a read-only consumer querying world config each frame forces no broadphase rebuild.
        /// @tparam T  The component type to find.
        /// @return Const pointer to the first T, or nullptr when the scene has none.
        template <class T>
        [[nodiscard]] const T* TryGetFirst() const
        {
            const TypeId id = m_Registry->IdOf<T>();
            if (PoolCount(id) == 0)
            {
                return nullptr;
            }
            return static_cast<const T*>(TryGetRaw(DensePtr(id)[0], id));
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

        /// @brief Const type-erased component fetch: the storage for `id` on `entity`, or nullptr if absent.
        ///
        /// The read-only sibling of the mutable TryGetComponent; routes through the const pool path,
        /// so it never stamps a change tick. The snapshot encoder reads each replicated component
        /// through it, gathering wire state without dirtying the very components it inspects.
        /// @param entity  The entity to query; must be alive.
        /// @param id      The TypeId of the component to fetch.
        /// @return The component's const storage, or nullptr if the entity lacks it.
        [[nodiscard]] const void* TryGetComponent(Entity entity, TypeId id) const
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
        /// @brief The sim tick a non-const component access stamps as the touched component's change tick.
        u64 m_ChangeTick = 0;

        /// @brief Component pools, keyed by TypeId, created lazily.
        unordered_map<TypeId, Unique<ComponentPool>> m_Pools;

        /// @brief One entity's local TRS captured for a history tick (Transform without the reflection weight).
        ///
        /// A plain value the history ring stores so Scene.h needs no Components.h; it mirrors
        /// Transform's Position/Rotation/Scale and converts to it for the interpolation blend.
        struct TransformSnapshot
        {
            /// @brief Local position in parent space.
            vec3 Position{0.0f};
            /// @brief Local rotation in parent space.
            quat Rotation{1.0f, 0.0f, 0.0f, 0.0f};
            /// @brief Local scale in parent space.
            vec3 Scale{1.0f};
        };

        /// @brief Captures every Transform entity's local TRS into @p out, in pool order.
        ///
        /// Iterates the const Transform view (no spatial-version bump); the history snapshot fills the
        /// current ring slot through this.
        /// @param out  Destination map, cleared then filled.
        void CaptureTransforms(unordered_map<Entity, TransformSnapshot>& out) const;

        /// @brief Returns an entity's interpolated local matrix from the history ring, or its live one.
        ///
        /// The per-level step of GetInterpolatedWorldTransform: blends the entity's previous/current
        /// snapshots by @p alpha when both exist, else falls back to its live Transform (identity when
        /// it has none).
        /// @param entity  The entity whose local matrix to resolve.
        /// @param alpha   The interpolation fraction in [0, 1].
        /// @return The entity's interpolated (or live) local matrix.
        [[nodiscard]] mat4 InterpolatedLocalMatrix(Entity entity, f32 alpha) const;

        /// @brief Previous Sim tick's transform snapshot, keyed by entity.
        unordered_map<Entity, TransformSnapshot> m_TransformPrev;
        /// @brief Current Sim tick's transform snapshot, keyed by entity.
        unordered_map<Entity, TransformSnapshot> m_TransformCur;
        /// @brief The spatial version the last history snapshot was taken at; != any real version initially.
        u64 m_HistoryVersion = ~0ULL;
        /// @brief True while the ring holds two differing ticks (motion to interpolate); false once converged.
        bool m_HistoryDirty = false;

        /// @brief The simulation driving this scene's systems, or null when none is attached.
        Unique<SceneSimulation> m_Simulation;

        /// @brief The Application simulation drive-list this scene is registered into; null when unregistered.
        ///
        /// Set by AttachToSimDriveList; ~Scene erases this scene's pointer from it. Not a component,
        /// so Clone (which recreates entities and copies components) never copies it.
        vector<Scene*>* m_SimDriveList = nullptr;

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
