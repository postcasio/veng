#pragma once

#include <span>

#include <Veng/Veng.h>
#include <Veng/Math/AABB.h>
#include <Veng/Math/Frustum.h>

// BVH — a bounding volume hierarchy over world-space AABBs. The third bounds
// primitive beside AABB and Frustum: glm-only value-type math, no ownership rule,
// inside the public/backend include-hygiene split. Rebuilt from scratch when its
// source set changes; the structure is build + query, no incremental maintenance.

namespace Veng
{
    // A bounding volume hierarchy over world-space AABBs. Build() constructs the
    // tree from a leaf array (each leaf a tight box plus a caller u32 id);
    // Query() returns the ids of the leaves a frustum touches — exactly a linear
    // Intersects scan of the tight boxes, since the accept test is per-leaf and
    // tight. A node's box encloses its children's. Device-free value type;
    // rebuilt from scratch when the source set changes.
    class BVH
    {
    public:
        struct Leaf { AABB Box; u32 Id; }; // tight world box + caller payload

        // Build the tree over `leaves` (replacing any prior contents). O(N log N)
        // top-down: recursively split the set by the surface-area-minimizing
        // partition along the longest centroid axis. Empty input yields an empty
        // tree.
        void Build(std::span<const Leaf> leaves);

        // Append the ids of every leaf whose box passes Intersects(frustum,.) to
        // `out` (NOT cleared). Descends internal nodes by their enclosing box,
        // tests leaves by their tight box.
        void Query(const Frustum& frustum, vector<u32>& out) const;

        [[nodiscard]] u32  GetLeafCount() const { return m_LeafCount; }
        [[nodiscard]] u32  GetNodeCount() const { return static_cast<u32>(m_Nodes.size()); }
        [[nodiscard]] i32  GetHeight()    const; // root height; 0 when empty or one leaf
        [[nodiscard]] AABB GetRootBounds() const; // box of everything (Empty when no leaves)

    private:
        static constexpr i32 NullNode = -1;

        struct Node
        {
            AABB Box;            // enclosing (leaf: the tight box)
            i32  Child1, Child2; // internal; both NullNode on a leaf
            u32  Id;             // leaf payload (unused on internal)
            [[nodiscard]] bool IsLeaf() const { return Child1 == NullNode; }
        };

        i32 BuildRange(std::span<Leaf> leaves); // recursive; returns the subtree root

        vector<Node> m_Nodes;
        i32 m_Root      = NullNode;
        u32 m_LeafCount = 0;
    };
}
