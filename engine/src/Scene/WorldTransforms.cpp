// The scene's world-transform pass: every world matrix computed once, parents first, into an array
// indexed by entity, and the same pass over the interpolated two-tick history.

#include <Veng/Scene/Scene.h>

#include <Veng/Assert.h>
#include <Veng/Diagnostics/Profiler.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Transforms.h>

#include <algorithm>

namespace Veng
{
    namespace
    {
        /// Marks a slot whose depth is still being resolved; meeting it again on a walk is a cycle.
        constexpr u32 DepthInProgress = ~0u;
    }

    void Scene::RebuildWorldOrder() const
    {
        VE_PROFILE_SCOPE("Scene/WorldOrder");
        WorldTransformCache& cache = m_WorldCache;

        // Zero marks a slot no build placed, so the stamp skips it on wrap.
        if (++cache.OrderBuild == 0)
        {
            ++cache.OrderBuild;
        }
        const u32 build = cache.OrderBuild;

        if (cache.Slots.size() < m_Slots.size())
        {
            cache.Slots.resize(m_Slots.size());
            cache.World.resize(m_Slots.size());
            cache.Interpolated.resize(m_Slots.size());
        }
        cache.Placed.clear();

        const ComponentPool* hierarchies = TryPoolOf<Hierarchy>();
        u32 maxDepth = 0;

        // Places an entity and every unplaced ancestor above it. The walk stops at a root or at an
        // ancestor an earlier walk placed, so each entity is visited once per build; the chain it
        // collected then takes depths counting down from that stop.
        const auto place = [&](const Entity entity)
        {
            cache.Walk.clear();
            Entity current = entity;
            u32 topDepth = 0;
            while (true)
            {
                WorldPassSlot& slot = cache.Slots[current.Index];
                if (slot.Order == build)
                {
                    VE_ASSERT(slot.Depth != DepthInProgress,
                              "UpdateWorldTransforms: Hierarchy chain forms a cycle");
                    topDepth = slot.Depth + 1;
                    break;
                }

                slot = WorldPassSlot{.Generation = current.Generation,
                                     .Order = build,
                                     .Depth = DepthInProgress,
                                     .Parent = NoWorldParent};
                cache.Walk.push_back(current);

                const auto* link = hierarchies != nullptr
                                       ? static_cast<const Hierarchy*>(hierarchies->TryGet(current))
                                       : nullptr;
                if (link == nullptr || link->Parent.IsNull())
                {
                    topDepth = 0;
                    break;
                }
                VE_ASSERT(IsAlive(link->Parent),
                          "UpdateWorldTransforms: Hierarchy references a dead or stale entity");
                slot.Parent = link->Parent.Index;
                current = link->Parent;
            }

            // Walk is entity-first, so its last element is the chain's top.
            u32 depth = topDepth;
            for (usize i = cache.Walk.size(); i-- > 0;)
            {
                cache.Slots[cache.Walk[i].Index].Depth = depth;
                cache.Placed.push_back(cache.Walk[i]);
                maxDepth = std::max(maxDepth, depth);
                ++depth;
            }
        };

        const auto placePool = [&](const ComponentPool* pool)
        {
            if (pool == nullptr)
            {
                return;
            }
            const Entity* dense = pool->DenseData();
            for (usize i = 0; i < pool->Count(); ++i)
            {
                if (cache.Slots[dense[i].Index].Order != build)
                {
                    place(dense[i]);
                }
            }
        };
        placePool(TryPoolOf<Transform>());
        placePool(hierarchies);

        // Counting sort by depth: every parent lands before its children, and placement order
        // breaks ties so the order is deterministic.
        cache.DepthOffsets.assign(static_cast<usize>(maxDepth) + 2, 0);
        for (const Entity entity : cache.Placed)
        {
            ++cache.DepthOffsets[cache.Slots[entity.Index].Depth + 1];
        }
        for (usize d = 1; d < cache.DepthOffsets.size(); ++d)
        {
            cache.DepthOffsets[d] += cache.DepthOffsets[d - 1];
        }
        cache.Order.resize(cache.Placed.size());
        for (const Entity entity : cache.Placed)
        {
            const WorldPassSlot& slot = cache.Slots[entity.Index];
            cache.Order[cache.DepthOffsets[slot.Depth]++] =
                WorldPassNode{.Owner = entity, .Parent = slot.Parent};
        }

        cache.OrderVersion = m_TopologyVersion;
    }

    void Scene::UpdateWorldTransforms() const
    {
        WorldTransformCache& cache = m_WorldCache;
        if (cache.WorldVersion == m_SpatialVersion)
        {
            return;
        }

        VE_PROFILE_SCOPE("Scene/WorldMatrices");
        if (cache.OrderVersion != m_TopologyVersion)
        {
            RebuildWorldOrder();
        }

        const ComponentPool* transforms = TryPoolOf<Transform>();
        for (const WorldPassNode& node : cache.Order)
        {
            const auto* transform =
                transforms != nullptr
                    ? static_cast<const Transform*>(transforms->TryGet(node.Owner))
                    : nullptr;
            const mat4 local = transform != nullptr ? LocalMatrix(*transform) : mat4(1.0f);
            cache.World[node.Owner.Index] =
                node.Parent == NoWorldParent ? local : cache.World[node.Parent] * local;
        }
        cache.WorldVersion = m_SpatialVersion;
    }

    bool Scene::AreWorldTransformsCurrent() const
    {
        return m_WorldCache.WorldVersion == m_SpatialVersion;
    }

    void Scene::UpdateInterpolatedWorldTransforms(const f32 alpha) const
    {
        WorldTransformCache& cache = m_WorldCache;
        if (cache.InterpolatedVersion == m_SpatialVersion && cache.InterpolatedAlpha == alpha &&
            cache.InterpolatedPrevCapture == m_TransformPrev.Capture &&
            cache.InterpolatedCurCapture == m_TransformCur.Capture)
        {
            return;
        }

        if (cache.OrderVersion != m_TopologyVersion)
        {
            RebuildWorldOrder();
        }

        const ComponentPool* transforms = TryPoolOf<Transform>();
        const ComponentPool* viewPoses = TryPoolOf<ViewPose>();
        for (const WorldPassNode& node : cache.Order)
        {
            const mat4 local = InterpolatedLocalMatrix(node.Owner, alpha, transforms, viewPoses);
            cache.Interpolated[node.Owner.Index] =
                node.Parent == NoWorldParent ? local : cache.Interpolated[node.Parent] * local;
        }
        cache.InterpolatedVersion = m_SpatialVersion;
        cache.InterpolatedAlpha = alpha;
        cache.InterpolatedPrevCapture = m_TransformPrev.Capture;
        cache.InterpolatedCurCapture = m_TransformCur.Capture;
    }

    const Scene::WorldPassSlot* Scene::FindWorldPassSlot(const Entity entity) const
    {
        const WorldTransformCache& cache = m_WorldCache;
        if (cache.OrderVersion != m_TopologyVersion || entity.Index >= cache.Slots.size())
        {
            return nullptr;
        }
        const WorldPassSlot& slot = cache.Slots[entity.Index];
        return slot.Order == cache.OrderBuild && slot.Generation == entity.Generation ? &slot
                                                                                      : nullptr;
    }

    const mat4* Scene::FindWorldMatrix(const Entity entity) const
    {
        if (m_WorldCache.WorldVersion != m_SpatialVersion || FindWorldPassSlot(entity) == nullptr)
        {
            return nullptr;
        }
        return &m_WorldCache.World[entity.Index];
    }

    const mat4* Scene::FindInterpolatedWorldMatrix(const Entity entity, const f32 alpha) const
    {
        const WorldTransformCache& cache = m_WorldCache;
        if (cache.InterpolatedVersion != m_SpatialVersion || cache.InterpolatedAlpha != alpha ||
            cache.InterpolatedPrevCapture != m_TransformPrev.Capture ||
            cache.InterpolatedCurCapture != m_TransformCur.Capture ||
            FindWorldPassSlot(entity) == nullptr)
        {
            return nullptr;
        }
        return &cache.Interpolated[entity.Index];
    }
}
