#include <Veng/Math/BVH.h>

#include <algorithm>

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

        constexpr i32 BucketCount = 12;
    }

    void BVH::Build(std::span<const Leaf> leaves)
    {
        m_Nodes.clear();
        m_Root = NullNode;
        m_LeafCount = static_cast<u32>(leaves.size());

        if (leaves.empty())
            return;

        // Copy into scratch BuildRange partitions in place. The node pool is
        // sized to the exact internal+leaf count (2N-1 for N leaves) so no
        // reallocation invalidates an index mid-build.
        m_Nodes.reserve(2 * leaves.size() - 1);
        vector<Leaf> scratch(leaves.begin(), leaves.end());
        m_Root = BuildRange(scratch);
    }

    i32 BVH::BuildRange(std::span<Leaf> leaves)
    {
        if (leaves.size() == 1)
        {
            const i32 index = static_cast<i32>(m_Nodes.size());
            m_Nodes.push_back(Node{leaves[0].Box, NullNode, NullNode, leaves[0].Id});
            return index;
        }

        // Centroid bounds pick the split axis; the longest centroid axis spreads
        // the leaves widest, giving the SAH sweep the most separation to work with.
        AABB centroidBounds = AABB::Empty();
        for (const Leaf& leaf : leaves)
            centroidBounds.Expand(leaf.Box.Center());

        const vec3 centroidSize = centroidBounds.Size();
        i32 axis = 0;
        if (centroidSize.y > centroidSize.x)
            axis = 1;
        if (centroidSize.z > centroidSize[axis])
            axis = 2;

        const auto centroidAxis = [axis](const Leaf& leaf) { return leaf.Box.Center()[axis]; };

        const usize median = leaves.size() / 2;
        usize mid = median;

        if (centroidSize[axis] <= 0.0f)
        {
            // Every centroid coincides on the split axis — the SAH sweep has no
            // separation to score, so split at the median to keep the tree balanced.
            std::nth_element(leaves.begin(), leaves.begin() + median, leaves.end(),
                             [&](const Leaf& a, const Leaf& b)
                             { return centroidAxis(a) < centroidAxis(b); });
        }
        else
        {
            // Bucket SAH sweep: bin each leaf by its centroid's position along the
            // axis, then pick the split between buckets that minimizes the summed
            // child surface area weighted by leaf count (the standard SAH cost).
            const f32 axisMin = centroidBounds.Min[axis];
            const f32 axisInv = static_cast<f32>(BucketCount) / centroidSize[axis];

            AABB bucketBox[BucketCount];
            i32 bucketCount[BucketCount] = {};
            for (i32 b = 0; b < BucketCount; ++b)
                bucketBox[b] = AABB::Empty();

            const auto bucketOf = [&](const Leaf& leaf)
            {
                i32 b = static_cast<i32>((centroidAxis(leaf) - axisMin) * axisInv);
                return std::clamp(b, 0, BucketCount - 1);
            };

            for (const Leaf& leaf : leaves)
            {
                const i32 b = bucketOf(leaf);
                bucketBox[b].Expand(leaf.Box);
                ++bucketCount[b];
            }

            // Sweep candidate splits; SAH cost = leftArea*leftCount + rightArea*rightCount.
            f32 bestCost = std::numeric_limits<f32>::infinity();
            i32 bestSplit = -1;
            for (i32 split = 1; split < BucketCount; ++split)
            {
                AABB leftBox = AABB::Empty();
                i32 leftCount = 0;
                for (i32 b = 0; b < split; ++b)
                {
                    leftBox.Expand(bucketBox[b]);
                    leftCount += bucketCount[b];
                }

                AABB rightBox = AABB::Empty();
                i32 rightCount = 0;
                for (i32 b = split; b < BucketCount; ++b)
                {
                    rightBox.Expand(bucketBox[b]);
                    rightCount += bucketCount[b];
                }

                if (leftCount == 0 || rightCount == 0)
                    continue;

                const f32 cost = SurfaceArea(leftBox) * static_cast<f32>(leftCount) +
                                 SurfaceArea(rightBox) * static_cast<f32>(rightCount);
                if (cost < bestCost)
                {
                    bestCost = cost;
                    bestSplit = split;
                }
            }

            if (bestSplit < 0)
            {
                // Every leaf fell in one bucket despite a nonzero centroid spread
                // (float binning collapse) — median-split to make progress.
                std::nth_element(leaves.begin(), leaves.begin() + median, leaves.end(),
                                 [&](const Leaf& a, const Leaf& b)
                                 { return centroidAxis(a) < centroidAxis(b); });
            }
            else
            {
                // The SAH skip of empty-side splits guarantees both groups are
                // non-empty, so the partition boundary is a valid interior split.
                const auto boundary =
                    std::partition(leaves.begin(), leaves.end(),
                                   [&](const Leaf& leaf) { return bucketOf(leaf) < bestSplit; });
                mid = static_cast<usize>(boundary - leaves.begin());
            }
        }

        const std::span<Leaf> left = leaves.subspan(0, mid);
        const std::span<Leaf> right = leaves.subspan(mid);
        const i32 child1 = BuildRange(left);
        const i32 child2 = BuildRange(right);

        const i32 index = static_cast<i32>(m_Nodes.size());
        m_Nodes.push_back(Node{Union(m_Nodes[child1].Box, m_Nodes[child2].Box), child1, child2, 0});
        return index;
    }

    void BVH::Query(const Frustum& frustum, vector<u32>& out) const
    {
        if (m_Root == NullNode)
            return;

        // Explicit-stack descent: a node whose box misses the frustum prunes its
        // subtree; a leaf is accepted on its tight box, so the result is exact.
        i32 stack[64];
        i32 top = 0;
        stack[top++] = m_Root;

        while (top > 0)
        {
            const Node& node = m_Nodes[stack[--top]];
            if (!Intersects(frustum, node.Box))
                continue;

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

    i32 BVH::GetHeight() const
    {
        if (m_Root == NullNode)
            return 0;

        // Iterative post-order over the index pool: a node's height is
        // 1 + max(child heights), a leaf's is 0.
        vector<i32> height(m_Nodes.size(), 0);
        for (usize i = 0; i < m_Nodes.size(); ++i)
        {
            const Node& node = m_Nodes[i];
            if (!node.IsLeaf())
                height[i] = 1 + std::max(height[node.Child1], height[node.Child2]);
        }
        return height[static_cast<usize>(m_Root)];
    }

    AABB BVH::GetRootBounds() const
    {
        if (m_Root == NullNode)
            return AABB::Empty();
        return m_Nodes[static_cast<usize>(m_Root)].Box;
    }
}
