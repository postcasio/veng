#include <Veng/Scene/Scene.h>

#include <Veng/Assert.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Diagnostics/Profiler.h>
#include <Veng/Net/LagCompensation.h>
#include <Veng/Net/SeatRelease.h>
#include <Veng/Physics/PhysicsWorld.h>
#include <Veng/Physics/PoseResolver.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/EffectPool.h>
#include <Veng/Scene/PresentationScope.h>
#include <Veng/Scene/SceneClone.h>
#include <Veng/Scene/SceneSimulation.h>
#include <Veng/Scene/Transforms.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <utility>

namespace Veng
{
    void RemapComponentReferences(void* obj, const TypeInfo& type, const TypeRegistry& registry,
                                  const EntityRemap& remap, const AssetHandleFixup& assetHandle,
                                  const EntityReferenceDiagnostic& diagnose)
    {
        for (const FieldDescriptor& field : type.Fields)
        {
            void* fieldPtr = static_cast<u8*>(obj) + field.Offset;

            switch (field.Class)
            {
            case FieldClass::Reference:
            {
                Entity& entity = *static_cast<Entity*>(fieldPtr);
                // The inspector sees the source-space target, before remap rewrites it.
                if (diagnose)
                {
                    diagnose(field, entity);
                }
                entity = remap(entity);
                break;
            }

            case FieldClass::AssetHandle:
            {
                assetHandle(fieldPtr);
                break;
            }

            case FieldClass::Struct:
            {
                const TypeInfo& nested = registry.Info(field.Type);
                RemapComponentReferences(fieldPtr, nested, registry, remap, assetHandle, diagnose);
                break;
            }

            case FieldClass::Variant:
            {
                // Descend into the active alternative so a Reference or AssetHandle
                // inside it is reached like one in a nested struct; an empty variant
                // has nothing to fix up.
                const TypeInfo& info = registry.Info(field.Type);
                void* memberPtr = info.VariantActivePtr(fieldPtr);
                if (memberPtr != nullptr)
                {
                    const TypeId active = info.VariantActiveType(fieldPtr);
                    RemapComponentReferences(memberPtr, registry.Info(active), registry, remap,
                                             assetHandle, diagnose);
                }
                break;
            }

            case FieldClass::Array:
            {
                // Each element is fixed up by its own class: an AssetHandle element is handed
                // to assetHandle directly (an array of context references), a struct element
                // recurses so a nested reference or handle is reached.
                const TypeInfo& element = registry.Info(field.ElementType);
                const usize count = field.ArraySize(fieldPtr);
                for (usize i = 0; i < count; ++i)
                {
                    void* elementPtr = field.ArrayElement(fieldPtr, i);
                    if (element.Class == FieldClass::AssetHandle)
                    {
                        assetHandle(elementPtr);
                    }
                    else if (element.Class == FieldClass::Reference)
                    {
                        Entity& entity = *static_cast<Entity*>(elementPtr);
                        // The array field carries any opt-out; report against it per element.
                        if (diagnose)
                        {
                            diagnose(field, entity);
                        }
                        entity = remap(entity);
                    }
                    else if (element.Class == FieldClass::Struct)
                    {
                        RemapComponentReferences(elementPtr, element, registry, remap, assetHandle,
                                                 diagnose);
                    }
                }
                break;
            }

            default:
                break;
            }
        }
    }

    bool Scene::IsSpatialId(TypeId id)
    {
        // These three pools decide draw candidacy and world bounds; checking compile-time
        // TypeId constants (not a registry lookup) keeps mutation-path overhead minimal.
        return id == TypeIdOf<Transform>() || id == TypeIdOf<Hierarchy>() ||
               id == TypeIdOf<MeshRenderer>();
    }

    bool Scene::IsTopologyId(TypeId id)
    {
        // Whether an entity carries one of these decides whether the world-transform pass visits it.
        return id == TypeIdOf<Transform>() || id == TypeIdOf<Hierarchy>();
    }

    Scene::ComponentPool::AccessVersion Scene::AccessVersionOf(const TypeId id)
    {
        // A non-const access is a potential in-place edit the ECS never sees, so it moves the
        // version conservatively (over-bump, never under). A Hierarchy edit can reparent, so it
        // moves the topology too.
        if (id == TypeIdOf<Hierarchy>())
        {
            return ComponentPool::AccessVersion::Topology;
        }
        return IsSpatialId(id) ? ComponentPool::AccessVersion::Spatial
                               : ComponentPool::AccessVersion::None;
    }

    void Scene::NoteMutableAccess(const TypeId id)
    {
        switch (AccessVersionOf(id))
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

    Scene::Scene(TypeRegistry& registry) : m_Registry(&registry)
    {
        // Atomic because a level may be spawned into a scene built on a worker.
        static std::atomic<u64> s_NextInstanceSerial{1};
        m_InstanceSerial = s_NextInstanceSerial.fetch_add(1, std::memory_order_relaxed);
    }

    // Out-of-line so every forward-declared type the scene owns through a Unique is complete at its
    // destruction site; the scene owns nothing else needing a hand-written teardown.
    Scene::~Scene() = default;

    Unique<Scene> Scene::Create(TypeRegistry& registry)
    {
        // Private constructor: raw new, not CreateUnique.
        return Unique<Scene>(new Scene(registry));
    }

    void Scene::SetSimulation(Unique<SceneSimulation> simulation)
    {
        m_Simulation = std::move(simulation);
    }

    bool Scene::IsSimulationPaused() const
    {
        return m_Simulation && m_Simulation->IsPaused();
    }

    void Scene::SetPhysicsWorld(Unique<PhysicsWorld> world)
    {
        m_PhysicsWorld = std::move(world);
    }

    void Scene::SetPhysicsPoseResolver(Unique<PhysicsPoseResolver> resolver)
    {
        m_PhysicsPoseResolver = std::move(resolver);
    }

    void Scene::SetPoseHistory(Unique<PoseHistory> history)
    {
        m_PoseHistory = std::move(history);
    }

    void Scene::SetSeatReleaseLog(Unique<SeatReleaseLog> log)
    {
        m_SeatReleaseLog = std::move(log);
    }

    void Scene::SetEffectPool(Unique<EffectPool> pool)
    {
        m_EffectPool = std::move(pool);
    }

    void Scene::SetPresentationScope(Unique<PresentationScope> scope)
    {
        m_PresentationScope = std::move(scope);
    }

    void Scene::RenewPresentationScope(const SystemContext& context)
    {
        if (m_PresentationScope)
        {
            m_PresentationScope->Renew(context.View.has_value());
        }
    }

    void Scene::StartSimulation(const SystemContext& context)
    {
        if (m_Simulation)
        {
            m_Simulation->Start(*this, context);
        }
    }

    void Scene::TickSimulation(const f32 delta, const SystemContext& context)
    {
        // Stamp every in-place edit this tick makes with the tick number, so the net layer can tell
        // what changed since a connection last acked.
        SetChangeTick(context.Tick);
        RenewPresentationScope(context);
        if (m_Simulation)
        {
            m_Simulation->Update(*this, delta, context);
        }
        // This tick's Sim systems have read the releases recorded before it.
        if (m_SeatReleaseLog)
        {
            m_SeatReleaseLog->Clear();
        }
    }

    void Scene::TickSimulationPhase(const SceneSystem::Phase phase, const f32 delta,
                                    const SystemContext& context, const bool recordHistory)
    {
        SetChangeTick(context.Tick);
        // Renewed before any View system runs, by the phase rather than by a system, so a level
        // listing no audio or haptics system still holds and releases what its Sim systems started.
        if (phase == SceneSystem::Phase::View)
        {
            RenewPresentationScope(context);
        }
        if (m_Simulation)
        {
            m_Simulation->UpdatePhase(*this, phase, delta, context);
        }

        // A completed Sim tick finalizes this tick's spatial state; snapshot it so the render gather
        // and View systems interpolate between the last two ticks. The View phase derives from that
        // finalized state and writes no new tick, so it does not snapshot.
        if (phase == SceneSystem::Phase::Sim)
        {
            if (recordHistory)
            {
                SnapshotTransformHistory();
            }
            // This tick's Sim systems have read the releases recorded before it.
            if (m_SeatReleaseLog)
            {
                m_SeatReleaseLog->Clear();
            }
        }
    }

    void Scene::CaptureTransforms(TransformHistoryBuffer& out)
    {
        // Zero marks a never-captured buffer, so the stamp skips it on wrap.
        if (++m_HistoryCapture == 0)
        {
            ++m_HistoryCapture;
        }
        out.Capture = m_HistoryCapture;
        if (out.Entries.size() < m_Slots.size())
        {
            out.Entries.resize(m_Slots.size());
        }
        for (auto [entity, transform] : std::as_const(*this).View<Transform>())
        {
            out.Entries[entity.Index] = TransformSnapshot{.Position = transform.Position,
                                                          .Rotation = transform.Rotation,
                                                          .Scale = transform.Scale,
                                                          .Generation = entity.Generation,
                                                          .Capture = m_HistoryCapture};
        }
    }

    void Scene::SnapshotTransformHistory()
    {
        VE_PROFILE_SCOPE("Scene/SnapshotTransforms");
        const u64 version = GetSpatialVersion();
        if (version == m_HistoryVersion)
        {
            // Nothing spatial moved since the last snapshot. If the ring still holds two differing
            // ticks, converge it once (prev = cur) so a now-static entity stops interpolating; a
            // scene already at rest copies nothing.
            if (m_HistoryDirty)
            {
                m_TransformPrev = m_TransformCur;
                m_HistoryDirty = false;
            }
            return;
        }

        // Something moved: roll the ring so the prior current becomes previous, then recapture the
        // live transforms into current. The blend interpolates previous → current.
        std::swap(m_TransformPrev, m_TransformCur);
        CaptureTransforms(m_TransformCur);
        m_HistoryVersion = version;
        m_HistoryDirty = true;
    }

    mat4 Scene::InterpolatedLocalMatrix(const Entity entity, const f32 alpha) const
    {
        return InterpolatedLocalMatrix(entity, alpha, TryPoolOf<Transform>(),
                                       TryPoolOf<ViewPose>());
    }

    mat4 Scene::InterpolatedLocalMatrix(const Entity entity, const f32 alpha,
                                        const ComponentPool* transforms,
                                        const ComponentPool* viewPoses) const
    {
        // A ViewPose transform is authored per frame, after the tick snapshot: its live pose is
        // already this frame's pose, and the history ring holds earlier frames' writes — blending
        // those would render the entity a frame stale against the anchor it follows.
        const TransformSnapshot* prev = m_TransformPrev.Find(entity);
        const TransformSnapshot* cur = m_TransformCur.Find(entity);
        if (prev != nullptr && cur != nullptr &&
            (viewPoses == nullptr || !viewPoses->Contains(entity)))
        {
            const Transform from{
                .Position = prev->Position, .Rotation = prev->Rotation, .Scale = prev->Scale};
            const Transform to{
                .Position = cur->Position, .Rotation = cur->Rotation, .Scale = cur->Scale};
            return LocalMatrix(InterpolateTransform(from, to, alpha));
        }

        // No two-tick history for this entity (first snapshot, or spawned since), or a live
        // ViewPose: use the live pose.
        if (const auto* transform = transforms != nullptr
                                        ? static_cast<const Transform*>(transforms->TryGet(entity))
                                        : nullptr)
        {
            return LocalMatrix(*transform);
        }
        return mat4(1.0f);
    }

    mat4 Scene::GetInterpolatedWorldTransform(const Entity entity, const f32 alpha) const
    {
        if (const mat4* world = FindInterpolatedWorldMatrix(entity, alpha))
        {
            return *world;
        }

        // Walk the Hierarchy chain entity → root with the same cycle/dead-entity checks WorldMatrix
        // runs, then compose root → entity from each level's interpolated local matrix.
        vector<Entity> chain;
        Entity current = entity;
        while (!current.IsNull())
        {
            VE_ASSERT(IsAlive(current),
                      "GetInterpolatedWorldTransform: Hierarchy references a dead or stale entity");

            for (const Entity seen : chain)
            {
                VE_ASSERT(seen != current,
                          "GetInterpolatedWorldTransform: Hierarchy chain forms a cycle");
            }
            chain.push_back(current);

            if (const auto* hierarchy = TryGet<Hierarchy>(current))
            {
                current = hierarchy->Parent;
            }
            else
            {
                current = Entity::Null;
            }
        }

        mat4 world(1.0f);
        for (usize i = chain.size(); i-- > 0;)
        {
            world = world * InterpolatedLocalMatrix(chain[i], alpha);
        }
        return world;
    }

    void Scene::StopSimulation(const SystemContext& context)
    {
        if (m_Simulation)
        {
            m_Simulation->Stop(*this, context);
        }
    }

    Unique<Scene> Scene::Clone() const
    {
        const TypeRegistry& registry = *m_Registry;
        Unique<Scene> clone = Create(*m_Registry);

        // 1. Recreate every live entity first, so a Reference field resolving
        //    forward always lands on a created handle. The destination is empty, so
        //    its slot order mirrors the source's live-slot order — but the remap is
        //    keyed on the source handle, not on slot identity.
        unordered_map<Entity, Entity> remap;
        remap.reserve(m_LiveCount);
        ForEachEntity([&](Entity source) { remap.emplace(source, clone->CreateEntity()); });

        const EntityRemap remapFn = [&remap](Entity source) -> Entity
        {
            if (source.IsNull())
            {
                return Entity::Null;
            }
            const auto it = remap.find(source);
            return it != remap.end() ? it->second : Entity::Null;
        };

        // 2. Copy every component. WriteFields/ReadFields round-trips the value
        //    bytes; the post-pass remaps Reference fields to the cloned handles and
        //    deep-copies AssetHandle fields directly. A serialized AssetHandle keeps
        //    only the AssetId, so a runtime-adopted (id-less) handle would round-trip
        //    to empty — copy the live cache-entry Ref straight across instead, so a
        //    cloned scene still renders its already-resident meshes.
        vector<u8> record;
        for (const Unique<ComponentPool>& pool : m_Pools)
        {
            const TypeInfo& typeInfo = pool->GetInfo();
            const TypeId typeId = typeInfo.Id;

            // Hierarchy is a derived component: only its Parent edge persists, and the
            // sibling/child links are rebuilt from the parent edges in pass 3. Copying
            // it here would pre-set Parent without consistent links, so the SetParent
            // rebuild would unlink against a corrupt list. Skip it and rebuild instead.
            if (typeId == TypeIdOf<Hierarchy>())
            {
                continue;
            }

            const usize count = pool->Count();
            const Entity* dense = pool->DenseData();

            for (usize i = 0; i < count; ++i)
            {
                const Entity source = dense[i];
                const Entity target = remap.at(source);
                const void* sourceComponent = pool->SlotData(static_cast<u32>(i));

                record.clear();
                WriteFields(record, sourceComponent, typeInfo, registry);

                void* targetComponent = clone->AddComponent(target, typeId);
                ReadFields(record, targetComponent, typeInfo, registry).value();

                const AssetHandleFixup copyHandle = [&](void* targetField)
                {
                    // The matching source field sits at the same offset within the
                    // identically-laid-out source component; copy the whole handle
                    // (AssetId + cache-entry Ref) so a resident handle survives.
                    const usize offset =
                        static_cast<u8*>(targetField) - static_cast<u8*>(targetComponent);
                    const auto* sourceField = static_cast<const u8*>(sourceComponent) + offset;

                    AssetId id{};
                    std::memcpy(&id, sourceField, sizeof(id));
                    const auto* sourceEntry = reinterpret_cast<const Ref<Detail::AssetCacheEntry>*>(
                        sourceField + Detail::AssetHandleEntryOffset);
                    Detail::RehydrateHandleField(targetField, id, *sourceEntry);
                };

                RemapComponentReferences(targetComponent, typeInfo, registry, remapFn, copyHandle);
            }
        }

        // 3. Rebuild the intrusive sibling/child links from the remapped parent
        //    edges, in source order so SetParent appends children in their original
        //    order — the same derived-link rebuild Prefab::SpawnInto performs.
        ForEachEntity(
            [&](Entity source)
            {
                const Entity target = remap.at(source);
                const Entity sourceParent = GetParent(source);
                if (!sourceParent.IsNull())
                {
                    clone->SetParent(target, remap.at(sourceParent));
                }
            });

        return clone;
    }

    Entity Scene::CreateEntity()
    {
        u32 index;
        if (!m_FreeIndices.empty())
        {
            index = m_FreeIndices.back();
            m_FreeIndices.pop_back();
        }
        else
        {
            index = static_cast<u32>(m_Slots.size());
            m_Slots.push_back(EntitySlot{});
        }

        EntitySlot& slot = m_Slots[index];
        slot.Alive = true;
        ++m_LiveCount;

        return Entity{.Index = index, .Generation = slot.Generation};
    }

    Entity Scene::CreateEntityAt(const Entity entity)
    {
        VE_ASSERT(!entity.IsNull(), "CreateEntityAt with a null handle");

        // Grow the slot table to cover the index; every slot skipped over is unallocated,
        // so push it onto the free list to keep CreateEntity's recycling correct.
        if (entity.Index >= m_Slots.size())
        {
            for (u32 index = static_cast<u32>(m_Slots.size()); index < entity.Index; ++index)
            {
                m_Slots.push_back(EntitySlot{});
                m_FreeIndices.push_back(index);
            }
            m_Slots.push_back(EntitySlot{.Generation = entity.Generation, .Alive = false});
        }

        EntitySlot& slot = m_Slots[entity.Index];
        VE_ASSERT(!slot.Alive, "CreateEntityAt on a live slot {}", entity.Index);

        // The slot may be sitting on the free list (a prior DestroyEntity pushed it). Drop it
        // so a later CreateEntity does not hand the same index out a second time.
        std::erase(m_FreeIndices, entity.Index);

        // Restore the slot to the requested generation: DestroyEntity bumped it past the handle
        // being respawned, so the captured handle (the one other stack entries and Reference
        // fields hold) is generation-stale until set back here. This is the exact-handle respawn.
        slot.Generation = entity.Generation;
        slot.Alive = true;
        ++m_LiveCount;

        return entity;
    }

    void Scene::DestroyEntity(Entity entity)
    {
        VE_ASSERT(IsAlive(entity), "DestroyEntity on a {} entity", Detail::NotAliveKind(entity));

        // Detach the destroyed root from any surviving parent's child list first,
        // so the siblings that outlive this call stay consistent.
        UnlinkFromSiblings(entity);

        // Destroying an entity destroys its whole subtree. Walk the FirstChild →
        // NextSibling links to collect it in O(subtree), then tear down — never
        // iterating-and-destroying a pool (a structural change mid-iteration is
        // illegal).
        vector<Entity> collected;
        collected.push_back(entity);
        const ComponentPool* pool = TryPoolOf<Hierarchy>();
        for (usize scanned = 0; pool != nullptr && scanned < collected.size(); ++scanned)
        {
            const auto* link = static_cast<const Hierarchy*>(pool->TryGet(collected[scanned]));
            if (link == nullptr)
            {
                continue;
            }
            for (Entity child = link->FirstChild; !child.IsNull();)
            {
                const auto* childLink = static_cast<const Hierarchy*>(pool->TryGet(child));
                collected.push_back(child);
                child = childLink != nullptr ? childLink->NextSibling : Entity::Null;
            }
        }

        bool spatialTouched = false;
        for (const Entity dead : collected)
        {
            for (const Unique<ComponentPool>& pool : m_Pools)
            {
                if (pool->Contains(dead))
                {
                    spatialTouched = spatialTouched || IsSpatialId(pool->GetInfo().Id);
                    pool->Remove(dead);
                }
            }

            EntitySlot& slot = m_Slots[dead.Index];
            slot.Alive = false;
            // Bump so any surviving handle to this slot is detectably stale.
            ++slot.Generation;
            --m_LiveCount;
            m_FreeIndices.push_back(dead.Index);
        }

        if (spatialTouched)
        {
            BumpTopology();
        }
    }

    Hierarchy& Scene::HierarchyOf(Entity entity)
    {
        // Resolve through the pool directly, not the templated TryGet/Add, so the
        // structural ops bump the spatial version exactly once each (explicitly),
        // never per link touched.
        if (ComponentPool* pool = TryPoolOf<Hierarchy>())
        {
            if (void* slot = pool->TryGet(entity))
            {
                return *static_cast<Hierarchy*>(slot);
            }
        }
        return *static_cast<Hierarchy*>(PoolFor(TypeIdOf<Hierarchy>()).Add(entity));
    }

    const Hierarchy* Scene::TryHierarchy(Entity entity) const
    {
        if (const ComponentPool* pool = TryPoolOf<Hierarchy>())
        {
            return static_cast<const Hierarchy*>(pool->TryGet(entity));
        }
        return nullptr;
    }

    void Scene::UnlinkFromSiblings(Entity child)
    {
        Hierarchy* link = nullptr;
        if (ComponentPool* pool = TryPoolOf<Hierarchy>())
        {
            link = static_cast<Hierarchy*>(pool->TryGet(child));
        }
        if (link == nullptr)
        {
            return;
        }

        const Entity prev = link->PrevSibling;
        const Entity next = link->NextSibling;

        if (!prev.IsNull())
        {
            HierarchyOf(prev).NextSibling = next;
        }
        else if (!link->Parent.IsNull())
        {
            // child was the head of its parent's list; promote the next sibling.
            HierarchyOf(link->Parent).FirstChild = next;
        }

        if (!next.IsNull())
        {
            HierarchyOf(next).PrevSibling = prev;
        }

        link->PrevSibling = Entity::Null;
        link->NextSibling = Entity::Null;
    }

    bool Scene::IsDescendantOf(Entity candidate, Entity entity) const
    {
        Entity current = candidate;
        while (!current.IsNull())
        {
            if (current == entity)
            {
                return true;
            }
            const Hierarchy* link = TryHierarchy(current);
            current = link != nullptr ? link->Parent : Entity::Null;
        }
        return false;
    }

    void Scene::SetParent(Entity child, Entity parent)
    {
        VE_ASSERT(IsAlive(child), "SetParent on a {} child", Detail::NotAliveKind(child));
        VE_ASSERT(parent.IsNull() || IsAlive(parent), "SetParent: parent is dead or stale");
        VE_ASSERT(child != parent, "SetParent: an entity cannot parent itself");
        // A descendant adopting an ancestor would form a cycle — API misuse.
        VE_ASSERT(parent.IsNull() || !IsDescendantOf(parent, child),
                  "SetParent: parent is a descendant of child (cycle)");

        UnlinkFromSiblings(child);

        Hierarchy& childLink = HierarchyOf(child);
        childLink.Parent = parent;

        if (!parent.IsNull())
        {
            Hierarchy& parentLink = HierarchyOf(parent);
            if (parentLink.FirstChild.IsNull())
            {
                parentLink.FirstChild = child;
            }
            else
            {
                // Append to the tail so children stay in insertion order.
                Entity tail = parentLink.FirstChild;
                while (true)
                {
                    Hierarchy& tailLink = HierarchyOf(tail);
                    if (tailLink.NextSibling.IsNull())
                    {
                        tailLink.NextSibling = child;
                        // HierarchyOf(child) may have moved the pool; re-fetch.
                        Hierarchy& reChild = HierarchyOf(child);
                        reChild.PrevSibling = tail;
                        break;
                    }
                    tail = tailLink.NextSibling;
                }
            }
        }

        BumpTopology();
    }

    void Scene::Detach(Entity child)
    {
        SetParent(child, Entity::Null);
    }

    void Scene::MoveBefore(Entity child, Entity sibling)
    {
        VE_ASSERT(IsAlive(child), "MoveBefore on a {} child", Detail::NotAliveKind(child));
        VE_ASSERT(!sibling.IsNull() && IsAlive(sibling),
                  "MoveBefore: sibling is null, dead, or stale");
        VE_ASSERT(child != sibling, "MoveBefore: child and sibling are the same entity");

        const Hierarchy* siblingLink = TryHierarchy(sibling);
        const Entity parent = siblingLink != nullptr ? siblingLink->Parent : Entity::Null;

        VE_ASSERT(parent.IsNull() || !IsDescendantOf(parent, child),
                  "MoveBefore: sibling's parent is a descendant of child (cycle)");

        UnlinkFromSiblings(child);

        Hierarchy& childLink = HierarchyOf(child);
        childLink.Parent = parent;

        const Hierarchy& sib = HierarchyOf(sibling);
        const Entity prev = sib.PrevSibling;

        HierarchyOf(child).PrevSibling = prev;
        HierarchyOf(child).NextSibling = sibling;
        HierarchyOf(sibling).PrevSibling = child;

        if (prev.IsNull())
        {
            if (!parent.IsNull())
            {
                HierarchyOf(parent).FirstChild = child;
            }
        }
        else
        {
            HierarchyOf(prev).NextSibling = child;
        }

        BumpTopology();
    }

    Entity Scene::GetParent(Entity entity) const
    {
        VE_ASSERT(IsAlive(entity), "GetParent on a {} entity", Detail::NotAliveKind(entity));
        const Hierarchy* link = TryHierarchy(entity);
        return link != nullptr ? link->Parent : Entity::Null;
    }

    void Scene::ForEachChild(Entity entity, const function<void(Entity)>& fn) const
    {
        VE_ASSERT(IsAlive(entity), "ForEachChild on a {} entity", Detail::NotAliveKind(entity));
        const Hierarchy* link = TryHierarchy(entity);
        if (link == nullptr)
        {
            return;
        }
        for (Entity child = link->FirstChild; !child.IsNull();)
        {
            const Hierarchy* childLink = TryHierarchy(child);
            const Entity next = childLink != nullptr ? childLink->NextSibling : Entity::Null;
            fn(child);
            child = next;
        }
    }

    Entity Scene::GetLiveEntityAtIndex(const u32 index) const
    {
        if (index >= m_Slots.size())
        {
            return Entity::Null;
        }
        const EntitySlot& slot = m_Slots[index];
        if (!slot.Alive)
        {
            return Entity::Null;
        }
        return Entity{.Index = index, .Generation = slot.Generation};
    }

    void Scene::ForEachEntity(const function<void(Entity)>& fn) const
    {
        for (u32 index = 0; index < m_Slots.size(); ++index)
        {
            const EntitySlot& slot = m_Slots[index];
            if (slot.Alive)
            {
                fn(Entity{.Index = index, .Generation = slot.Generation});
            }
        }
    }

    void* Scene::AddRaw(Entity entity, TypeId id)
    {
        if (IsTopologyId(id))
        {
            BumpTopology();
        }
        else if (IsSpatialId(id))
        {
            BumpSpatial();
        }
        ComponentPool& pool = PoolFor(id);
        void* slot = pool.Add(entity);
        // Adding a component is a write: stamp it with the current tick so it reads dirty.
        pool.StampSlot(static_cast<u32>(pool.Count() - 1), m_ChangeTick);
        return slot;
    }

    TypeId Scene::FindRequirer(const Entity entity, const TypeId id) const
    {
        VE_ASSERT(IsAlive(entity), "FindRequirer on a {} entity", Detail::NotAliveKind(entity));

        for (const Unique<ComponentPool>& pool : m_Pools)
        {
            const TypeInfo& info = pool->GetInfo();
            if (info.Id == id || !pool->Contains(entity))
            {
                continue;
            }
            if (std::ranges::find(info.Requires, id) != info.Requires.end())
            {
                return info.Id;
            }
        }
        return InvalidTypeId;
    }

    VoidResult Scene::RemoveRaw(Entity entity, TypeId id)
    {
        ComponentPool* pool = TryPoolFor(id);
        if (pool == nullptr)
        {
            return {};
        }

        // Refuse while a sibling declares this component required, so the requirer is never left
        // resolving a component that has gone. Only a component the entity actually carries can be
        // refused — removing one it lacks is the documented no-op and breaks nobody. DestroyEntity
        // tears its pools down directly and is deliberately not routed through here: the requirer
        // goes with the entity.
        if (pool->Contains(entity))
        {
            if (const TypeId requirer = FindRequirer(entity, id); requirer != InvalidTypeId)
            {
                return std::unexpected(fmt::format(
                    "cannot remove '{}': '{}' on the same entity requires it",
                    m_Registry->Info(id).QualifiedName, m_Registry->Info(requirer).QualifiedName));
            }
        }

        if (IsTopologyId(id))
        {
            BumpTopology();
        }
        else if (IsSpatialId(id))
        {
            BumpSpatial();
        }
        pool->Remove(entity);
        return {};
    }

    void Scene::ForEachComponent(Entity entity, const function<void(TypeId, void*)>& fn)
    {
        VE_ASSERT(IsAlive(entity), "ForEachComponent on a {} entity", Detail::NotAliveKind(entity));

        for (const Unique<ComponentPool>& pool : m_Pools)
        {
            const u32 slot = pool->FindSlot(entity);
            if (slot != ComponentPool::Absent)
            {
                // The erased pointer is a mutable edit funnel (the inspector's), so a visit moves
                // the versions and stamps the change tick exactly as a non-const access does.
                NoteMutableAccess(*pool);
                pool->StampSlot(slot, m_ChangeTick);
                fn(pool->GetInfo().Id, pool->SlotData(slot));
            }
        }
    }

    u64 Scene::GetComponentChangeTick(Entity entity, TypeId id) const
    {
        VE_ASSERT(IsAlive(entity), "GetComponentChangeTick on a {} entity",
                  Detail::NotAliveKind(entity));
        const ComponentPool* pool = TryPoolFor(id);
        return pool != nullptr ? pool->ChangeTick(entity) : 0;
    }

    usize Scene::PoolCount(TypeId id) const
    {
        const ComponentPool* pool = TryPoolFor(id);
        return pool != nullptr ? pool->Count() : 0;
    }

    const Entity* Scene::DensePtr(TypeId id) const
    {
        const ComponentPool* pool = TryPoolFor(id);
        return pool != nullptr ? pool->DenseData() : nullptr;
    }

    Scene::ComponentPool& Scene::PoolFor(TypeId id)
    {
        const TypeInfo& info = m_Registry->Info(id);
        if (info.Ordinal >= m_PoolTable.size())
        {
            m_PoolTable.resize(std::max<usize>(info.Ordinal + 1, m_Registry->Count()), nullptr);
        }

        ComponentPool*& entry = m_PoolTable[info.Ordinal];
        if (entry == nullptr)
        {
            m_Pools.push_back(std::make_unique<ComponentPool>(info, AccessVersionOf(id)));
            entry = m_Pools.back().get();
        }
        return *entry;
    }
}
