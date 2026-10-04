#pragma once

#include <Veng/Veng.h>
#include <Veng/Assert.h>
#include <Veng/Result.h>
#include <Veng/Scene/ComponentPool.h>
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
    class PhysicsWorld;
    class PoseHistory;
    class SeatReleaseLog;
    class EffectPool;
    struct PhysicsPoseResolver;
    struct SystemContext;
    struct AABB;
    struct Hierarchy;
    struct VisibleMesh;
    mat4 WorldMatrix(const Scene& scene, Entity entity);
    void ComputeWorldMatrices(const Scene& scene, vector<mat4>& out);
    AABB SceneBounds(const Scene& scene);
    void GatherMeshes(const Scene& scene, vector<VisibleMesh>& out, AABB& outBounds, Entity exclude,
                      u32 layerMask);

    namespace Detail
    {
        /// @brief Why a handle failed an IsAlive assert, for its message: a null handle is a caller
        /// that never had an entity, a dead or stale one a caller holding a handle past its entity.
        [[nodiscard]] inline const char* NotAliveKind(const Entity entity)
        {
            return entity.IsNull() ? "null" : "dead or stale";
        }
    }

    /// @brief Runtime ECS world: a generational entity free-list plus one type-erased sparse-set pool per component type.
    ///
    /// The templated Add/Remove/Get/Has façade finds T's pool by T's ordinal in the TypeRegistry
    /// (an array index, no hash) and works on the erased pool, created lazily on first Add of a
    /// type. Scene is Unique — single owner; the app owns it and a renderer reads it per frame as a
    /// `const Scene&`. The TypeRegistry it was created with must outlive it and must already have
    /// every component type registered.
    class Scene
    {
        /// @brief The erased per-type store; Scene is its only user.
        using ComponentPool = Detail::ComponentPool;

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
        [[nodiscard]] bool IsAlive(const Entity entity) const
        {
            if (entity.Index >= m_Slots.size())
            {
                return false;
            }
            const EntitySlot& slot = m_Slots[entity.Index];
            return slot.Alive && slot.Generation == entity.Generation;
        }

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
        /// write; a non-const View or Each over a spatial type bumps it once, when it
        /// is created over a non-empty driving pool, not once per entity it visits.
        /// Read-only consumers use the const View/Each path to avoid bumping.
        [[nodiscard]] u64 GetSpatialVersion() const { return m_SpatialVersion; }

        /// @brief The lowest tick a write can stamp; a Scene's change tick never falls below it.
        ///
        /// Tick zero is reserved to mean *before any tick*: it is what a component the entity does
        /// not carry reports, and what a replication baseline holds before anything is acked. The
        /// floor keeps a real write out of that value, so an edit made before the world drive's
        /// first tick is still strictly newer than a fresh baseline and reads as dirty exactly once.
        static constexpr u64 MinChangeTick = 1;

        /// @brief Sets the sim tick that a non-const component access stamps as its change tick.
        ///
        /// The world drive sets this to SystemContext::Tick each phase, so an in-place edit during a
        /// tick stamps that tick onto the touched (entity, component). The value floors at
        /// MinChangeTick, so an edit before the first tick — level load, editor authoring — stamps
        /// one rather than the reserved zero. The net layer's dirty query keys off the resulting
        /// per-entity change ticks.
        /// @param tick  The tick value non-const accesses now stamp; raised to MinChangeTick if lower.
        void SetChangeTick(u64 tick) { m_ChangeTick = tick < MinChangeTick ? MinChangeTick : tick; }

        /// @brief Returns the sim tick non-const accesses currently stamp (see SetChangeTick).
        ///
        /// Never zero — the value floors at MinChangeTick from construction onward, so no write ever
        /// produces the reserved before-any-tick value.
        [[nodiscard]] u64 GetChangeTick() const { return m_ChangeTick; }

        /// @brief Returns the last tick component @p id on @p entity was stamped at, or 0 if absent.
        ///
        /// Zero is reserved for *before any tick* and here means only that the entity does not carry
        /// the component, so nothing has ever stamped it. A component the entity does carry was
        /// stamped when it was added, at a tick of at least MinChangeTick, so a real stamp reads
        /// non-zero however early it was written.
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

        /// @brief Attaches (or replaces) the rigid-body simulation space this scene's bodies live in.
        ///
        /// A Scene optionally owns a PhysicsWorld the way it optionally owns a SceneSimulation, and
        /// it owns none by default — a scene with no physics instantiates nothing and costs
        /// nothing. Passing a null pointer detaches and destroys the held one, which destroys every
        /// body with it. Clone() does not copy it.
        /// @param world  The world to own, or null to detach.
        void SetPhysicsWorld(Unique<PhysicsWorld> world);

        /// @brief Returns the attached physics world, or null when the scene has none.
        [[nodiscard]] PhysicsWorld* GetPhysicsWorld() const { return m_PhysicsWorld.get(); }

        /// @brief Installs (or replaces) the mapping between this scene's Transform chain and its
        ///        physics world's frame.
        ///
        /// Installed beside the physics world because it describes the same pairing: the gameplay
        /// systems that reason in the solver's space read poses through it, so a consumer whose
        /// authoritative positions live outside the f32 Transform makes interaction focus and vehicle
        /// enter/exit work in the frame the solver integrates in. A scene with none installed
        /// composes poses up the Transform chain, which is exactly right while the two share an
        /// origin. Passing null detaches the held one; Clone() does not copy it.
        /// @param resolver  The resolver to own, or null to detach.
        void SetPhysicsPoseResolver(Unique<PhysicsPoseResolver> resolver);

        /// @brief Returns the installed pose resolver, or null when the scene has none.
        [[nodiscard]] PhysicsPoseResolver* GetPhysicsPoseResolver() const
        {
            return m_PhysicsPoseResolver.get();
        }

        /// @brief Installs (or replaces) the record of this scene's recent body poses.
        ///
        /// The server-side history a lag-compensated query rewinds through (see
        /// Veng/Net/LagCompensation.h): it is scene-owned rather than held on the entities it
        /// records, so it never serializes and never replicates. Passing null detaches and destroys
        /// the held one; Clone() does not copy it.
        /// @param history  The history to own, or null to detach.
        void SetPoseHistory(Unique<PoseHistory> history);

        /// @brief Returns the installed pose history, or null when the scene has none.
        [[nodiscard]] PoseHistory* GetPoseHistory() const { return m_PoseHistory.get(); }

        /// @brief Installs (or replaces) the record of seats the host released from this scene.
        ///
        /// The host records into it before destroying a released seat, and this scene clears it at
        /// the end of every Sim tick (see Veng/Net/SeatRelease.h), so a Sim system reads each release
        /// on the tick after it happened, once. Passing null detaches and destroys the held one;
        /// Clone() does not copy it.
        /// @param log  The log to own, or null to detach.
        void SetSeatReleaseLog(Unique<SeatReleaseLog> log);

        /// @brief Returns the installed seat release log, or null when the scene has none.
        [[nodiscard]] SeatReleaseLog* GetSeatReleaseLog() const { return m_SeatReleaseLog.get(); }

        /// @brief Installs (or replaces) the pool this scene's short-lived effects are drawn from.
        ///
        /// Scene-owned so its bound is per scene, and so FlipbookSystem can retire the pool's
        /// finished effects without a caller driving it; SpawnTransientEffect installs a default
        /// one on first use. Replacing or detaching a pool leaves the entities it stood in the
        /// scene, no longer pooled. Passing null detaches and destroys the held one; Clone() does
        /// not copy it.
        /// @param pool  The pool to own, or null to detach.
        void SetEffectPool(Unique<EffectPool> pool);

        /// @brief Returns the installed effect pool, or null when the scene has none.
        [[nodiscard]] EffectPool* GetEffectPool() const { return m_EffectPool.get(); }

        /// @brief Starts the attached simulation over this scene; a no-op when none is attached.
        ///
        /// Forwards to SceneSimulation::Start(*this, context) — calls OnStart on each system.
        /// @param context  Per-tick services forwarded to each system.
        void StartSimulation(const SystemContext& context);

        /// @brief Advances the attached simulation one tick over this scene; a no-op when none.
        ///
        /// Forwards to SceneSimulation::Update(*this, delta, context) — the Sim-then-View phase pass —
        /// then clears the seat release log the tick has read.
        /// @param delta    Time in seconds since the previous tick.
        /// @param context  Per-tick services forwarded to each system.
        void TickSimulation(f32 delta, const SystemContext& context);

        /// @brief Runs one phase of the attached simulation, snapshotting transform history after Sim.
        ///
        /// The fixed-timestep drive calls this once per fixed step for Phase::Sim (advancing the tick)
        /// and once per frame for Phase::View (carrying the interpolation alpha). After a Sim phase it
        /// snapshots the scene's spatial state into the transform-history ring, so the render gather
        /// and View systems can interpolate between the last two ticks, and clears the seat release log
        /// the tick has read. A no-op when no simulation is attached (the snapshot and the clear still
        /// run, capturing the static pose).
        /// @param phase          The phase whose systems run.
        /// @param delta          Time in seconds forwarded to each system's OnUpdate.
        /// @param context        Per-tick services forwarded to each system.
        /// @param recordHistory  Whether a Sim phase snapshots transform history. Interpolation reads
        ///                       only a frame's final two ticks, so a drive that knows which steps
        ///                       those are (SimStepInfo::RecordsHistory) skips the snapshot on the
        ///                       others; ignored for the View phase.
        void TickSimulationPhase(SceneSystem::Phase phase, f32 delta, const SystemContext& context,
                                 bool recordHistory = true);

        /// @brief Snapshots every Transform entity's local TRS into the two-tick history ring.
        ///
        /// Called at the end of a Sim tick whose pose interpolation may read (through
        /// TickSimulationPhase). Rolls the previous snapshot to make room for the current one, keyed
        /// off the spatial version so a static scene copies nothing after it converges.
        /// GetInterpolatedWorldTransform blends the two by a frame's alpha.
        void SnapshotTransformHistory();

        /// @brief Returns an entity's world matrix, interpolating its TRS between the last two Sim ticks.
        ///
        /// Walks the Hierarchy chain like WorldMatrix, but composes each level's local matrix from the
        /// two-tick history blended by @p alpha (previous → current). An entity with no history entry
        /// (never snapshotted) falls back to its live Transform, so the result equals WorldMatrix when
        /// no interpolation applies; a ViewPose level resolves its live Transform (a per-frame authored
        /// pose is already this frame's — see ViewPose). A View system (a camera rig) reads this so the
        /// camera and the meshes it frames share one interpolated pose. While the interpolated pass is
        /// current at @p alpha (UpdateInterpolatedWorldTransforms) this reads its entry instead of
        /// walking.
        ///
        /// **The exemption is the ViewPose tag and nothing else.** An entity whose Transform is written
        /// per frame but which carries no tag is blended from history like any other, which resolves a
        /// pose one frame stale — so a View system that authors a pose owes it the tag, and a caller
        /// reading a *vantage* back out (a camera, a rig's anchor) must pass the frame's alpha rather
        /// than reach for WorldMatrix. Neither error is visible while the eye is still: both scale with
        /// how far the pose travels within a frame, so they present as content sliding against the view
        /// under motion and vanish when it stops.
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

        /// @brief Brings the scene's world-matrix pass current with its transforms.
        ///
        /// Computes every world matrix once, into an array indexed by entity, in an order where a
        /// parent precedes its children, so each entity costs one multiply onto its parent's entry.
        /// Afterwards WorldMatrix reads an entry instead of walking the Hierarchy chain. The pass is
        /// skipped when nothing spatial moved since the last one, and the parent-first order is
        /// rebuilt only when the topology changed: a Transform or Hierarchy added or removed, a
        /// reparent, a destroy, or a non-const Hierarchy access. A spatial change after the pass makes
        /// it stale, and WorldMatrix walks again until the next one, so a reader never sees a stale
        /// matrix.
        ///
        /// It is const because the pass is a cache derived from the scene rather than scene state: it
        /// moves no spatial version and stamps no change tick, so a read-only consumer such as the
        /// render gather can bring it current. It does write that cache, so it must not run while
        /// another thread updates or reads the same scene's world matrices.
        void UpdateWorldTransforms() const;

        /// @brief Returns true while the world-matrix pass is current, so WorldMatrix reads it.
        [[nodiscard]] bool AreWorldTransformsCurrent() const;

        /// @brief Brings the interpolated world-matrix pass current at @p alpha.
        ///
        /// The same parent-first pass as UpdateWorldTransforms, composing each level from the two-tick
        /// history blended by @p alpha exactly as GetInterpolatedWorldTransform does. Afterwards
        /// GetInterpolatedWorldTransform at the same alpha reads an entry. Skipped when nothing spatial
        /// moved, the history did not roll, and the alpha is unchanged. The concurrency rule of
        /// UpdateWorldTransforms applies.
        /// @param alpha  The interpolation fraction in [0, 1] (0 = previous tick, 1 = current).
        void UpdateInterpolatedWorldTransforms(f32 alpha) const;

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
            VE_ASSERT(IsAlive(entity), "AddComponent on a {} entity", Detail::NotAliveKind(entity));
            return AddRaw(entity, id);
        }

        /// @brief Type-erased remove: removes the component of the given TypeId from the entity.
        ///
        /// The templated Remove\<T\> resolves T to TypeId and forwards here; the
        /// editor inspector, which only knows a component's TypeId, calls it
        /// directly. A no-op when the entity lacks the component.
        ///
        /// **Refused** while a sibling on the same entity declares this type required (VE_REQUIRES):
        /// the component stays and the error names both types. Removing the requirer first is what
        /// a caller that means to dismantle the pair does; FindRequirer answers the question ahead
        /// of the call. DestroyEntity is not gated — a whole entity going away breaks no sibling.
        /// @param entity  The entity to remove from.
        /// @param id      The component type to remove.
        /// @return Success, or an error naming the sibling that requires this component.
        /// @pre The entity must be alive.
        VoidResult RemoveComponent(Entity entity, TypeId id)
        {
            VE_ASSERT(IsAlive(entity), "RemoveComponent on a {} entity",
                      Detail::NotAliveKind(entity));
            return RemoveRaw(entity, id);
        }

        /// @brief Returns the component on the entity that declares @p id required, if any.
        ///
        /// The question RemoveComponent answers by refusing: a tool offering removal asks first and
        /// reports the reason (or hides the affordance) rather than issuing a call it knows fails.
        /// With several requirers the first in pool order is returned — one is enough to refuse.
        /// @param entity  The entity to search.
        /// @param id      The component type whose requirers are sought.
        /// @return The requiring component's TypeId, or InvalidTypeId when the removal is free.
        /// @pre The entity must be alive.
        [[nodiscard]] TypeId FindRequirer(Entity entity, TypeId id) const;

        /// @brief Adds component T (initialized from value) to the entity and returns a reference to it.
        template <class T>
        T& Add(Entity entity, T value = {})
        {
            VE_ASSERT(IsAlive(entity), "Add on a {} entity", Detail::NotAliveKind(entity));
            void* slot = AddRaw(entity, m_Registry->IdOf<T>());
            T& component = *static_cast<T*>(slot);
            component = std::move(value);
            return component;
        }

        /// @brief Removes component T from the entity, refusing while a sibling requires it.
        ///
        /// @see RemoveComponent for the requirement gate and the error it reports.
        /// @tparam T  The component type to remove.
        /// @param entity  The entity to remove from.
        /// @return Success, or an error naming the sibling that requires T.
        template <class T>
        VoidResult Remove(Entity entity)
        {
            VE_ASSERT(IsAlive(entity), "Remove on a {} entity", Detail::NotAliveKind(entity));
            return RemoveRaw(entity, m_Registry->IdOf<T>());
        }

        /// @brief Returns a pointer to component T on the entity, or nullptr if absent.
        ///
        /// A non-const access: it moves the spatial version when T is a spatial type and stamps the
        /// component's change tick when present.
        template <class T>
        [[nodiscard]] T* TryGet(Entity entity)
        {
            VE_ASSERT(IsAlive(entity), "TryGet on a {} entity", Detail::NotAliveKind(entity));
            return static_cast<T*>(AccessMutable(TryPoolOf<T>(), m_Registry->IdOf<T>(), entity));
        }

        /// @brief Returns a const pointer to component T on the entity, or nullptr if absent.
        template <class T>
        [[nodiscard]] const T* TryGet(Entity entity) const
        {
            VE_ASSERT(IsAlive(entity), "TryGet on a {} entity", Detail::NotAliveKind(entity));
            const ComponentPool* pool = TryPoolOf<T>();
            return pool != nullptr ? static_cast<const T*>(pool->TryGet(entity)) : nullptr;
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
            ComponentPool* pool = TryPoolOf<T>();
            if (pool == nullptr || pool->Count() == 0)
            {
                return nullptr;
            }
            NoteMutableAccess(*pool);
            pool->StampSlot(0, m_ChangeTick);
            return static_cast<T*>(pool->SlotData(0));
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
            const ComponentPool* pool = TryPoolOf<T>();
            if (pool == nullptr || pool->Count() == 0)
            {
                return nullptr;
            }
            return static_cast<const T*>(pool->SlotData(0));
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
            VE_ASSERT(IsAlive(entity), "Has on a {} entity", Detail::NotAliveKind(entity));
            const ComponentPool* pool = TryPoolOf<T>();
            return pool != nullptr && pool->Contains(entity);
        }

        /// @brief Visits every entity holding all of Ts..., calling fn(entity, Ts&...).
        ///
        /// Drives from the smallest participating pool (no archetype bookkeeping).
        /// Iteration order is the driver pool's dense order. Mutating a component
        /// through its Ts& reference is fine; structural changes (adding/removing
        /// components or destroying entities) during iteration are illegal. The pools
        /// are resolved once, the spatial version moves once (when a T is spatial and the
        /// driving pool is non-empty), and each visited component's change tick is stamped.
        template <class... Ts, class Fn>
        void Each(Fn&& fn)
        {
            static_assert(sizeof...(Ts) > 0, "Each requires at least one component type");
            constexpr usize Arity = sizeof...(Ts);

            const std::array<ComponentPool*, Arity> pools = {TryPoolOf<Ts>()...};
            const usize driver = Detail::SelectDriver(pools);
            if (driver == Arity || pools[driver]->Count() == 0)
            {
                return;
            }
            for (const ComponentPool* pool : pools)
            {
                NoteMutableAccess(*pool);
            }

            const usize count = pools[driver]->Count();
            const Entity* dense = pools[driver]->DenseData();
            std::array<u32, Arity> slots{};
            for (usize i = 0; i < count; ++i)
            {
                const Entity entity = dense[i];
                if (!Detail::FindQuerySlots(pools, driver, static_cast<u32>(i), entity, slots))
                {
                    continue;
                }
                for (usize t = 0; t < Arity; ++t)
                {
                    pools[t]->StampSlot(slots[t], m_ChangeTick);
                }
                InvokeEach<Ts...>(fn, entity, pools, slots, std::index_sequence_for<Ts...>{});
            }
        }

        /// @brief Read-only Each: visits every entity holding all of Ts..., calling fn(entity, const Ts&...).
        ///
        /// Reads through the const pool path only, so a const iteration never bumps the
        /// spatial version or stamps a change tick. Same intersection and in-iteration
        /// structural-change constraints as the non-const Each.
        template <class... Ts, class Fn>
        void Each(Fn&& fn) const
        {
            static_assert(sizeof...(Ts) > 0, "Each requires at least one component type");
            constexpr usize Arity = sizeof...(Ts);

            const std::array<const ComponentPool*, Arity> pools = {TryPoolOf<Ts>()...};
            const usize driver = Detail::SelectDriver(pools);
            if (driver == Arity)
            {
                return;
            }

            const usize count = pools[driver]->Count();
            const Entity* dense = pools[driver]->DenseData();
            std::array<u32, Arity> slots{};
            for (usize i = 0; i < count; ++i)
            {
                const Entity entity = dense[i];
                if (Detail::FindQuerySlots(pools, driver, static_cast<u32>(i), entity, slots))
                {
                    InvokeEach<const Ts...>(fn, entity, pools, slots,
                                            std::index_sequence_for<Ts...>{});
                }
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
            VE_ASSERT(IsAlive(entity), "TryGetComponent on a {} entity",
                      Detail::NotAliveKind(entity));
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
            VE_ASSERT(IsAlive(entity), "TryGetComponent on a {} entity",
                      Detail::NotAliveKind(entity));
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

        /// @brief Unpacks the slots a query found into typed references and invokes fn.
        /// @tparam Ts     The component types, const-qualified for a read-only query.
        /// @tparam Fn     The visitor's type.
        /// @tparam Pools  The query's pool array, const-qualified pools for a read-only query.
        /// @param fn      The visitor.
        /// @param entity  The visited entity.
        /// @param pools   The query's pools.
        /// @param slots   The entity's dense slot in each pool.
        template <class... Ts, class Fn, class Pools, usize... Is>
        static void InvokeEach(Fn& fn, const Entity entity, const Pools& pools,
                               const std::array<u32, sizeof...(Ts)>& slots,
                               std::index_sequence<Is...>)
        {
            fn(entity, *static_cast<Ts*>(pools[Is]->SlotData(slots[Is]))...);
        }

        /// @brief Returns the element count of the pool for id, or 0 if no pool exists.
        [[nodiscard]] usize PoolCount(TypeId id) const;
        /// @brief Returns a pointer to the dense entity array for the pool of id, or nullptr if absent.
        [[nodiscard]] const Entity* DensePtr(TypeId id) const;

        // Type-erased façade for the TypeId-keyed public members.
        // IsAlive is asserted by the caller before each of these.
        void* AddRaw(Entity entity, TypeId id);
        VoidResult RemoveRaw(Entity entity, TypeId id);

        /// @brief Non-const erased fetch: moves the versions and stamps the change tick like TryGet\<T\>.
        void* TryGetRaw(const Entity entity, const TypeId id)
        {
            return AccessMutable(TryPoolFor(id), id, entity);
        }

        /// @brief Const erased fetch: the component's storage, or nullptr; moves and stamps nothing.
        [[nodiscard]] const void* TryGetRaw(const Entity entity, const TypeId id) const
        {
            const ComponentPool* pool = TryPoolFor(id);
            return pool != nullptr ? pool->TryGet(entity) : nullptr;
        }

        /// @brief The non-const access path: moves the versions @p pool's type moves, then stamps
        ///        and returns the entity's component.
        /// @param pool    The type's pool, or null when the scene has none.
        /// @param id      The type's TypeId, read only when @p pool is null.
        /// @param entity  The entity to fetch.
        /// @return The component's storage, or nullptr when the entity lacks it.
        void* AccessMutable(ComponentPool* pool, const TypeId id, const Entity entity)
        {
            if (pool == nullptr)
            {
                NoteMutableAccess(id);
                return nullptr;
            }
            NoteMutableAccess(*pool);
            const u32 slot = pool->FindSlot(entity);
            if (slot == ComponentPool::Absent)
            {
                return nullptr;
            }
            pool->StampSlot(slot, m_ChangeTick);
            return pool->SlotData(slot);
        }

        /// @brief Moves the versions a non-const access to @p pool's components moves.
        void NoteMutableAccess(const ComponentPool& pool)
        {
            switch (pool.GetAccessVersion())
            {
            case ComponentPool::AccessVersion::Spatial:
                BumpSpatial();
                break;
            case ComponentPool::AccessVersion::Topology:
                BumpTopology();
                break;
            case ComponentPool::AccessVersion::None:
                break;
            }
        }

        /// @brief Moves the versions a non-const access to type @p id moves, for a type with no pool.
        void NoteMutableAccess(TypeId id);

        /// @brief Returns how a non-const access to a component of type @p id moves the versions.
        [[nodiscard]] static ComponentPool::AccessVersion AccessVersionOf(TypeId id);

        /// @brief Resolves (creating on first use) the pool for a registered TypeId.
        ComponentPool& PoolFor(TypeId id);

        /// @brief Returns the pool at @p ordinal, or nullptr when the scene has none for it.
        ///
        /// Const and handing out a mutable pool, because the const and non-const paths share it;
        /// a const member only reads through the result.
        /// @param ordinal  A type ordinal from the scene's registry, or InvalidTypeOrdinal.
        [[nodiscard]] ComponentPool* PoolAt(const u32 ordinal) const
        {
            return ordinal < m_PoolTable.size() ? m_PoolTable[ordinal] : nullptr;
        }

        /// @brief Returns T's pool, or nullptr if none exists. The per-access lookup: no hash.
        /// @tparam T  The component type.
        template <class T>
        [[nodiscard]] ComponentPool* TryPoolOf() const
        {
            return PoolAt(m_Registry->OrdinalOf<std::remove_const_t<T>>());
        }

        /// @brief Returns the pool for id, or nullptr if none exists; resolves the ordinal by TypeId.
        [[nodiscard]] ComponentPool* TryPoolFor(const TypeId id) const
        {
            return PoolAt(m_Registry->OrdinalOf(id));
        }

        /// @brief Returns true if id names a spatial pool (Transform, Hierarchy, or MeshRenderer).
        [[nodiscard]] static bool IsSpatialId(TypeId id);
        /// @brief Returns true if id names a pool that decides world-transform topology (Transform, Hierarchy).
        [[nodiscard]] static bool IsTopologyId(TypeId id);
        /// @brief Advances the spatial version counter.
        void BumpSpatial() { ++m_SpatialVersion; }
        /// @brief Advances the topology version, and the spatial version with it.
        ///
        /// For a change that can alter which entities the world-transform pass visits or who parents
        /// whom, so the pass rebuilds its parent-first order.
        void BumpTopology()
        {
            ++m_TopologyVersion;
            ++m_SpatialVersion;
        }

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
        /// @brief Monotonic counter for changes to which entities carry a world transform or a parent.
        u64 m_TopologyVersion = 0;
        /// @brief The sim tick a non-const component access stamps as the touched component's change tick.
        ///
        /// Starts at the floor rather than zero, which is reserved for *before any tick*.
        u64 m_ChangeTick = MinChangeTick;

        /// @brief Component pools in creation order, created lazily; owns them.
        vector<Unique<ComponentPool>> m_Pools;
        /// @brief The pools indexed by type ordinal; null where the scene pools nothing of a type.
        vector<ComponentPool*> m_PoolTable;

        /// @brief One entity's local TRS captured for a history tick, with the capture it belongs to.
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
            /// @brief The generation of the entity captured into this slot.
            u32 Generation = 0;
            /// @brief The capture that wrote this entry; an entry from an older capture is absent.
            u32 Capture = 0;
        };

        /// @brief One history tick: an entry per entity slot, valid where its capture is the buffer's.
        ///
        /// Indexed by Entity::Index, so a lookup is an index plus a generation and capture check, and
        /// a capture overwrites only the slots holding a Transform without clearing the rest.
        struct TransformHistoryBuffer
        {
            /// @brief Entries indexed by entity slot; grown to the slot count, never shrunk.
            vector<TransformSnapshot> Entries;
            /// @brief The capture this buffer holds; 0 for a buffer never captured into.
            u32 Capture = 0;

            /// @brief Returns @p entity's entry in this buffer, or null when it was not captured.
            /// @param entity  The entity to look up.
            [[nodiscard]] const TransformSnapshot* Find(const Entity entity) const
            {
                if (Capture == 0 || entity.Index >= Entries.size())
                {
                    return nullptr;
                }
                const TransformSnapshot& entry = Entries[entity.Index];
                return entry.Capture == Capture && entry.Generation == entity.Generation ? &entry
                                                                                         : nullptr;
            }
        };

        /// @brief Captures every Transform entity's local TRS into @p out, in pool order.
        ///
        /// Iterates the const Transform view (no spatial-version bump); the history snapshot fills the
        /// current ring slot through this.
        /// @param out  Destination buffer, stamped with a fresh capture then filled.
        void CaptureTransforms(TransformHistoryBuffer& out);

        /// @brief Returns an entity's interpolated local matrix from the history ring, or its live one.
        ///
        /// The per-level step of GetInterpolatedWorldTransform: blends the entity's previous/current
        /// snapshots by @p alpha when both exist, else falls back to its live Transform (identity when
        /// it has none).
        /// @param entity  The entity whose local matrix to resolve.
        /// @param alpha   The interpolation fraction in [0, 1].
        /// @return The entity's interpolated (or live) local matrix.
        [[nodiscard]] mat4 InterpolatedLocalMatrix(Entity entity, f32 alpha) const;

        /// @brief InterpolatedLocalMatrix with the Transform and ViewPose pools already resolved.
        /// @param entity      The entity whose local matrix to resolve.
        /// @param alpha       The interpolation fraction in [0, 1].
        /// @param transforms  The Transform pool, or null when there is none.
        /// @param viewPoses   The ViewPose pool, or null when there is none.
        /// @return The entity's interpolated (or live) local matrix.
        [[nodiscard]] mat4 InterpolatedLocalMatrix(Entity entity, f32 alpha,
                                                   const ComponentPool* transforms,
                                                   const ComponentPool* viewPoses) const;

        /// @brief Sentinel parent slot of a root in the world-transform order.
        static constexpr u32 NoWorldParent = ~0u;

        /// @brief One entity in the parent-first world-transform order.
        struct WorldPassNode
        {
            /// @brief The entity whose world matrix this node computes.
            Entity Owner;
            /// @brief The parent's entity slot, or NoWorldParent for a root.
            u32 Parent = NoWorldParent;
        };

        /// @brief The world-transform pass's bookkeeping for one entity slot.
        struct WorldPassSlot
        {
            /// @brief The generation of the entity the order placed in this slot.
            u32 Generation = 0;
            /// @brief The order build that placed this slot; a slot from an older build is absent.
            u32 Order = 0;
            /// @brief Depth below the entity's root; the order sorts by it.
            u32 Depth = 0;
            /// @brief The parent's entity slot, or NoWorldParent for a root.
            u32 Parent = NoWorldParent;
        };

        /// @brief The world-transform pass: the parent-first order and the matrices it computes.
        ///
        /// A cache derived from the scene's transforms, keyed by the spatial and topology versions, so
        /// the const update paths may refresh it. Every vector grows to the slot count and is reused,
        /// so a steady-state pass allocates nothing.
        struct WorldTransformCache
        {
            /// @brief Entities with a world transform, parents first.
            vector<WorldPassNode> Order;
            /// @brief Bookkeeping per entity slot.
            vector<WorldPassSlot> Slots;
            /// @brief Current-tick world matrices, per entity slot.
            vector<mat4> World;
            /// @brief Interpolated world matrices, per entity slot.
            vector<mat4> Interpolated;
            /// @brief Order-build scratch: the unplaced chain of one upward walk.
            vector<Entity> Walk;
            /// @brief Order-build scratch: entities in placement order, before the depth sort.
            vector<Entity> Placed;
            /// @brief Order-build scratch: the counting sort's per-depth offsets.
            vector<u32> DepthOffsets;
            /// @brief The current order build; 0 is never stamped.
            u32 OrderBuild = 0;
            /// @brief The topology version the order was built at.
            u64 OrderVersion = ~0ULL;
            /// @brief The spatial version World was computed at.
            u64 WorldVersion = ~0ULL;
            /// @brief The spatial version Interpolated was computed at.
            u64 InterpolatedVersion = ~0ULL;
            /// @brief The alpha Interpolated was computed at.
            f32 InterpolatedAlpha = 0.0f;
            /// @brief The previous-tick history capture Interpolated was computed from.
            u32 InterpolatedPrevCapture = 0;
            /// @brief The current-tick history capture Interpolated was computed from.
            u32 InterpolatedCurCapture = 0;
        };

        /// @brief Rebuilds the parent-first order over every entity with a Transform or a Hierarchy.
        void RebuildWorldOrder() const;
        /// @brief Returns the pass's slot record for @p entity, or null when the order does not hold it.
        [[nodiscard]] const WorldPassSlot* FindWorldPassSlot(Entity entity) const;
        /// @brief Returns @p entity's world matrix from a current pass, or null to walk instead.
        [[nodiscard]] const mat4* FindWorldMatrix(Entity entity) const;
        /// @brief Returns @p entity's interpolated world matrix from a pass current at @p alpha, or null.
        [[nodiscard]] const mat4* FindInterpolatedWorldMatrix(Entity entity, f32 alpha) const;

        /// @brief The world-transform pass; mutable because it is a cache the const paths refresh.
        mutable WorldTransformCache m_WorldCache;

        /// @brief Previous Sim tick's transform snapshot.
        TransformHistoryBuffer m_TransformPrev;
        /// @brief Current Sim tick's transform snapshot.
        TransformHistoryBuffer m_TransformCur;
        /// @brief The last capture number stamped on a history buffer; 0 is never stamped.
        u32 m_HistoryCapture = 0;
        /// @brief The spatial version the last history snapshot was taken at; != any real version initially.
        u64 m_HistoryVersion = ~0ULL;
        /// @brief True while the ring holds two differing ticks (motion to interpolate); false once converged.
        bool m_HistoryDirty = false;

        /// @brief The simulation driving this scene's systems, or null when none is attached.
        Unique<SceneSimulation> m_Simulation;

        /// @brief The rigid-body simulation space this scene's bodies live in, or null when none.
        Unique<PhysicsWorld> m_PhysicsWorld;

        /// @brief The Transform-chain-to-solver-frame mapping, or null when the two share an origin.
        Unique<PhysicsPoseResolver> m_PhysicsPoseResolver;

        /// @brief The recent body poses a lag-compensated query rewinds through, or null when none.
        Unique<PoseHistory> m_PoseHistory;

        /// @brief The seats released from this scene since its last Sim tick, or null when none.
        Unique<SeatReleaseLog> m_SeatReleaseLog;

        /// @brief The pool this scene's short-lived effects are drawn from, or null when none.
        Unique<EffectPool> m_EffectPool;

        template <class...>
        friend class SceneView;

        friend mat4 WorldMatrix(const Scene& scene, Entity entity);
        friend void ComputeWorldMatrices(const Scene& scene, vector<mat4>& out);
        friend AABB SceneBounds(const Scene& scene);
        friend void GatherMeshes(const Scene& scene, vector<VisibleMesh>& out, AABB& outBounds,
                                 Entity exclude, u32 layerMask);
    };

    /// @brief Range iterable returned by Scene::View\<Ts...\>().
    ///
    /// begin()/end() yield a forward iterator visiting exactly the entities
    /// holding all of Ts..., in the smallest-pool driver's dense order,
    /// dereferencing to `(Entity, Ts&...)` — so `auto [e, a, b]` works and
    /// `break` stops early. The same in-iteration structural-change constraint
    /// as Each applies.
    ///
    /// The view resolves each type's pool once, at construction; the iterator keeps
    /// the dense slots its match test found, so dereferencing looks nothing up again.
    /// A mutable view moves the spatial version once at construction (when a T is
    /// spatial and the driving pool is non-empty) and stamps each component's change
    /// tick as it is dereferenced.
    ///
    /// Ts may be const-qualified: Scene::View\<Ts...\>() const yields
    /// SceneView\<const Ts...\>, which reads through the const pool path and
    /// dereferences to `const Ts&` — so a const iteration never bumps the spatial
    /// version.
    template <class... Ts>
    class SceneView
    {
        static_assert(sizeof...(Ts) > 0, "View requires at least one component type");

        /// @brief The number of component types the view intersects.
        static constexpr usize Arity = sizeof...(Ts);

        // const Scene when any Ts is const (the read-only path); a mutable Scene otherwise.
        static constexpr bool AnyConst = (std::is_const_v<Ts> || ...);
        static_assert(!AnyConst || (std::is_const_v<Ts> && ...),
                      "A View is read-only when any component is const: name every component const "
                      "to read, or none to write");
        using SceneRef = std::conditional_t<AnyConst, const Scene, Scene>;
        using Pool =
            std::conditional_t<AnyConst, const Detail::ComponentPool, Detail::ComponentPool>;

    public:
        /// @brief Constructs the view, resolving each pool and picking the smallest as the driver.
        explicit SceneView(SceneRef& scene)
            : m_Pools{scene.template TryPoolOf<Ts>()...}, m_Tick(scene.GetChangeTick())
        {
            // A missing pool leaves the range empty.
            m_Driver = Detail::SelectDriver(m_Pools);
            if (m_Driver == Arity)
            {
                return;
            }
            m_Count = m_Pools[m_Driver]->Count();
            m_Dense = m_Pools[m_Driver]->DenseData();
            if constexpr (!AnyConst)
            {
                if (m_Count > 0)
                {
                    for (const Pool* pool : m_Pools)
                    {
                        scene.NoteMutableAccess(*pool);
                    }
                }
            }
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

            /// @brief Dereferences to (Entity, Ts&...), stamping each component's change tick for a mutable view.
            std::tuple<Entity, Ts&...> operator*() const
            {
                if constexpr (!AnyConst)
                {
                    for (usize t = 0; t < Arity; ++t)
                    {
                        m_View->m_Pools[t]->StampSlot(m_Slots[t], m_View->m_Tick);
                    }
                }
                return Resolve(m_View->m_Dense[m_Index], std::index_sequence_for<Ts...>{});
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
                while (m_Index < m_View->m_Count &&
                       !Detail::FindQuerySlots(m_View->m_Pools, m_View->m_Driver,
                                               static_cast<u32>(m_Index), m_View->m_Dense[m_Index],
                                               m_Slots))
                {
                    ++m_Index;
                }
            }

            template <usize... Is>
            std::tuple<Entity, Ts&...> Resolve(Entity entity, std::index_sequence<Is...>) const
            {
                return std::tuple<Entity, Ts&...>(
                    entity, *static_cast<Ts*>(m_View->m_Pools[Is]->SlotData(m_Slots[Is]))...);
            }

            /// @brief The view being iterated.
            const SceneView* m_View;
            /// @brief The current dense index in the driving pool.
            usize m_Index;
            /// @brief The current entity's dense slot in each pool, as the match test found them.
            std::array<u32, Arity> m_Slots{};
        };

        /// @brief Returns an iterator to the first matching entity.
        [[nodiscard]] Iterator begin() const { return Iterator(this, 0); }
        /// @brief Returns the past-the-end iterator.
        [[nodiscard]] Iterator end() const { return Iterator(this, m_Count); }

    private:
        /// @brief Each Ts's pool, resolved once; all non-null unless the view is empty.
        std::array<Pool*, Arity> m_Pools;
        /// @brief The change tick a mutable dereference stamps.
        u64 m_Tick;
        /// @brief Index of the driving (smallest) pool in m_Pools; Arity when a pool is missing.
        usize m_Driver = Arity;
        /// @brief Driver pool element count.
        usize m_Count = 0;
        /// @brief Driver pool's dense entity array.
        const Entity* m_Dense = nullptr;
    };
}
