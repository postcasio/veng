#include <Veng/Math/BVH.h>

#include <Veng/Assert.h>

#include <algorithm>
#include <array>

namespace Veng
{
    namespace
    {
        // Surface area of a box's extents — the SAH cost weight. A negative
        // extent (the Empty() sentinel) clamps to zero so an empty side costs
        // nothing rather than scoring a partition with a bogus negative area.
        f32 SurfaceArea(const AABB& box)
        {
            const vec3 size = glm::max(box.Size(), vec3(0.0f));
            return 2.0f * (size.x * size.y + size.y * size.z + size.z * size.x);
        }
    }

    namespace Detail
    {
        BucketSplit BestBucketSplit(const std::span<const AABB, BVH::BucketCount> boxes,
                                    const std::span<const u32, BVH::BucketCount> counts)
        {
            constexpr i32 Buckets = BVH::BucketCount;

            // Right side of each boundary: rightArea[s] and rightCount[s] cover buckets s..end.
            f32 rightArea[Buckets] = {};
            u32 rightCount[Buckets] = {};
            AABB right = AABB::Empty();
            u32 rightLeaves = 0;
            for (i32 b = Buckets - 1; b > 0; --b)
            {
                right.Expand(boxes[b]);
                rightLeaves += counts[b];
                rightArea[b] = SurfaceArea(right);
                rightCount[b] = rightLeaves;
            }

            // Grow the left side one bucket at a time and cost each boundary against its suffix.
            BucketSplit best;
            AABB left = AABB::Empty();
            u32 leftLeaves = 0;
            for (i32 split = 1; split < Buckets; ++split)
            {
                left.Expand(boxes[split - 1]);
                leftLeaves += counts[split - 1];
                if (leftLeaves == 0 || rightCount[split] == 0)
                {
                    continue;
                }

                const f32 cost = SurfaceArea(left) * static_cast<f32>(leftLeaves) +
                                 rightArea[split] * static_cast<f32>(rightCount[split]);
                if (cost < best.Cost)
                {
                    best = BucketSplit{.Split = split, .Cost = cost};
                }
            }
            return best;
        }
    }

    void BVH::Build(std::span<const Leaf> leaves)
    {
        m_Nodes.clear();
        m_Root = NullNode;
        m_LeafCount = static_cast<u32>(leaves.size());

        if (leaves.empty())
        {
            m_Cost = 0.0f;
            m_BuildCost = 0.0f;
            return;
        }

        // Copy into scratch BuildRange partitions in place, with each centroid computed once rather
        // than at every level that bins it. The node pool is sized to the exact internal+leaf count
        // (2N-1 for N leaves) so no reallocation invalidates an index mid-build.
        m_Nodes.reserve(2 * leaves.size() - 1);
        m_BuildScratch.resize(leaves.size());
        for (usize i = 0; i < leaves.size(); ++i)
        {
            m_BuildScratch[i] = BuildLeaf{.Item = leaves[i], .Centroid = leaves[i].Box.Center()};
        }
        m_Root = BuildRange(m_BuildScratch);

        UpdateCost();
        m_BuildCost = m_Cost;
    }

    void BVH::Refit(const std::span<const AABB> boxes)
    {
        // Children precede their parents in the pool, so one forward pass is bottom-up.
        for (Node& node : m_Nodes)
        {
            if (node.IsLeaf())
            {
                VE_ASSERT(node.Id < boxes.size(), "BVH::Refit: leaf id {} has no box ({} given)",
                          node.Id, boxes.size());
                node.Box = boxes[node.Id];
            }
            else
            {
                node.Box = Union(m_Nodes[node.Child1].Box, m_Nodes[node.Child2].Box);
            }
        }
        UpdateCost();
    }

    void BVH::UpdateCost()
    {
        m_Cost = 0.0f;
        if (m_Root == NullNode)
        {
            return;
        }

        const f32 rootArea = SurfaceArea(m_Nodes[static_cast<usize>(m_Root)].Box);
        if (rootArea <= 0.0f)
        {
            return;
        }

        f32 internalArea = 0.0f;
        for (const Node& node : m_Nodes)
        {
            if (!node.IsLeaf())
            {
                internalArea += SurfaceArea(node.Box);
            }
        }
        m_Cost = internalArea / rootArea;
    }

    i32 BVH::BuildRange(std::span<BuildLeaf> leaves)
    {
        if (leaves.size() == 1)
        {
            const i32 index = static_cast<i32>(m_Nodes.size());
            m_Nodes.push_back(Node{.Box = leaves[0].Item.Box,
                                   .Child1 = NullNode,
                                   .Child2 = NullNode,
                                   .Id = leaves[0].Item.Id});
            return index;
        }

        // Centroid bounds pick the split axis; the longest centroid axis spreads
        // the leaves widest, giving the SAH sweep the most separation to work with.
        AABB centroidBounds = AABB::Empty();
        for (const BuildLeaf& leaf : leaves)
        {
            centroidBounds.Expand(leaf.Centroid);
        }

        const vec3 centroidSize = centroidBounds.Size();
        i32 axis = 0;
        if (centroidSize.y > centroidSize.x)
        {
            axis = 1;
        }
        if (centroidSize.z > centroidSize[axis])
        {
            axis = 2;
        }

        const auto byAxis = [axis](const BuildLeaf& a, const BuildLeaf& b)
        { return a.Centroid[axis] < b.Centroid[axis]; };

        const usize median = leaves.size() / 2;
        usize mid = median;

        if (centroidSize[axis] <= 0.0f)
        {
            // Every centroid coincides on the split axis — the SAH sweep has no
            // separation to score, so split at the median to keep the tree balanced.
            std::nth_element(leaves.begin(), leaves.begin() + median, leaves.end(), byAxis);
        }
        else
        {
            // Bucket SAH sweep: bin each leaf by its centroid's position along the
            // axis, then pick the split between buckets that minimizes the summed
            // child surface area weighted by leaf count (the standard SAH cost).
            const f32 axisMin = centroidBounds.Min[axis];
            const f32 axisInv = static_cast<f32>(BucketCount) / centroidSize[axis];

            std::array<AABB, BucketCount> bucketBox;
            std::array<u32, BucketCount> bucketCount = {};
            bucketBox.fill(AABB::Empty());

            const auto bucketOf = [&](const BuildLeaf& leaf)
            {
                const i32 b = static_cast<i32>((leaf.Centroid[axis] - axisMin) * axisInv);
                return std::clamp(b, 0, BucketCount - 1);
            };

            for (const BuildLeaf& leaf : leaves)
            {
                const i32 b = bucketOf(leaf);
                bucketBox[b].Expand(leaf.Item.Box);
                ++bucketCount[b];
            }

            const Detail::BucketSplit best = Detail::BestBucketSplit(bucketBox, bucketCount);
            if (best.Split < 0)
            {
                // Every leaf fell in one bucket despite a nonzero centroid spread
                // (float binning collapse) — median-split to make progress.
                std::nth_element(leaves.begin(), leaves.begin() + median, leaves.end(), byAxis);
            }
            else
            {
                // The SAH skip of empty-side splits guarantees both groups are
                // non-empty, so the partition boundary is a valid interior split.
                const auto boundary = std::ranges::partition(
                    leaves, [&](const BuildLeaf& leaf) { return bucketOf(leaf) < best.Split; });
                mid = static_cast<usize>(boundary.begin() - leaves.begin());
            }
        }

        const std::span<BuildLeaf> left = leaves.subspan(0, mid);
        const std::span<BuildLeaf> right = leaves.subspan(mid);
        const i32 child1 = BuildRange(left);
        const i32 child2 = BuildRange(right);

        const i32 index = static_cast<i32>(m_Nodes.size());
        m_Nodes.push_back(Node{.Box = Union(m_Nodes[child1].Box, m_Nodes[child2].Box),
                               .Child1 = child1,
                               .Child2 = child2,
                               .Id = 0});
        return index;
    }

    void BVH::Query(const Frustum& frustum, vector<u32>& out) const
    {
        if (m_Root == NullNode)
        {
            return;
        }

        // Explicit-stack descent: a node whose box misses the frustum prunes its
        // subtree; a leaf is accepted on its tight box, so the result is exact.
        i32 stack[64];
        i32 top = 0;
        stack[top++] = m_Root;

        while (top > 0)
        {
            const Node& node = m_Nodes[stack[--top]];
            if (!Intersects(frustum, node.Box))
            {
                continue;
            }

            if (node.IsLeaf())
            {
                out.push_back(node.Id);
            }
            else
            {
                stack[top++] = node.Child1;
                stack[top++] = node.Child2;
            }
        }
    }

    void BVH::QueryBits(const Frustum& frustum, const std::span<u64> bits) const
    {
        if (m_Root == NullNode)
        {
            return;
        }

        i32 stack[64];
        i32 top = 0;
        stack[top++] = m_Root;

        while (top > 0)
        {
            const Node& node = m_Nodes[stack[--top]];
            if (!Intersects(frustum, node.Box))
            {
                continue;
            }

            if (node.IsLeaf())
            {
                VE_ASSERT(node.Id / 64 < bits.size(), "BVH::QueryBits: leaf {} past the bit span",
                          node.Id);
                bits[node.Id / 64] |= u64{1} << (node.Id % 64);
            }
            else
            {
                stack[top++] = node.Child1;
                stack[top++] = node.Child2;
            }
        }
    }

    i32 BVH::GetHeight() const
    {
        if (m_Root == NullNode)
        {
            return 0;
        }

        // Iterative post-order over the index pool: a node's height is
        // 1 + max(child heights), a leaf's is 0.
        vector<i32> height(m_Nodes.size(), 0);
        for (usize i = 0; i < m_Nodes.size(); ++i)
        {
            const Node& node = m_Nodes[i];
            if (!node.IsLeaf())
            {
                height[i] = 1 + std::max(height[node.Child1], height[node.Child2]);
            }
        }
        return height[static_cast<usize>(m_Root)];
    }

    AABB BVH::GetRootBounds() const
    {
        if (m_Root == NullNode)
        {
            return AABB::Empty();
        }
        return m_Nodes[static_cast<usize>(m_Root)].Box;
    }
}
