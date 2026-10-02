#pragma once

#include <limits>
#include <span>

#include <Veng/Veng.h>
#include <Veng/Math/AABB.h>
#include <Veng/Math/Frustum.h>

namespace Veng
{
    /// @brief Bounding volume hierarchy over world-space AABBs.
    ///
    /// glm-only device-free value type (no ownership rule), so it stays inside the
    /// public/backend include-hygiene split. Build() constructs the tree from a leaf array;
    /// Query() returns leaf ids whose tight box intersects a frustum. Refit() keeps the
    /// topology and moves the boxes, for a leaf set whose members moved but did not change;
    /// the tree records its surface-area cost at build so the owner can tell when refits have
    /// degraded it enough to rebuild.
    class BVH
    {
    public:
        /// @brief Centroid buckets the SAH split evaluation bins each node's leaves into.
        static constexpr i32 BucketCount = 12;

        /// @brief One BVH leaf: a tight world-space box and a caller-supplied payload id.
        struct Leaf
        {
            /// @brief Tight world-space bounding box.
            AABB Box;
            /// @brief Caller-supplied payload id returned by queries.
            u32 Id;
        };

        /// @brief Builds the tree over `leaves`, replacing any prior contents.
        ///
        /// O(N log N) top-down: recursively splits the set by the surface-area-minimizing
        /// partition along the longest centroid axis. Empty input yields an empty tree.
        void Build(std::span<const Leaf> leaves);

        /// @brief Moves every node's box to fit new leaf boxes, keeping the tree's topology.
        ///
        /// One bottom-up pass: each leaf takes its new box and each internal node the union of its
        /// children's. Queries stay exact, since a leaf is still tested on its tight box; only the
        /// tree's quality can suffer, which GetCost() reports.
        /// @param boxes  The new tight box of each leaf, indexed by the leaf's Id; every Id the tree
        ///               holds must index it.
        void Refit(std::span<const AABB> boxes);

        /// @brief Returns the tree's surface-area cost: its internal nodes' areas over the root's.
        ///
        /// The expected number of internal nodes a random ray visits, the standard measure of a
        /// tree's quality. 0 for a tree of at most one leaf or a zero-area root.
        [[nodiscard]] f32 GetCost() const { return m_Cost; }

        /// @brief Returns the surface-area cost the tree had when it was last built.
        [[nodiscard]] f32 GetBuildCost() const { return m_BuildCost; }

        /// @brief Appends the ids of every leaf whose box intersects `frustum` to `out` (not cleared).
        ///
        /// Descends internal nodes by their enclosing box; tests leaves by their tight box.
        void Query(const Frustum& frustum, vector<u32>& out) const;

        /// @brief Sets the bit of every leaf whose box intersects `frustum` (bits not cleared).
        ///
        /// The same descent as Query, recording membership instead of appending, so a caller
        /// wanting the survivors in id order walks the words rather than sorting.
        /// @param frustum The frustum to test against.
        /// @param bits    One bit per leaf id, low bit first within each word; must hold every id.
        void QueryBits(const Frustum& frustum, std::span<u64> bits) const;

        /// @brief Returns the number of leaf nodes.
        [[nodiscard]] u32 GetLeafCount() const { return m_LeafCount; }

        /// @brief Returns the total number of nodes (internal + leaf).
        [[nodiscard]] u32 GetNodeCount() const { return static_cast<u32>(m_Nodes.size()); }

        /// @brief Returns the root height; 0 when the tree is empty or has one leaf.
        [[nodiscard]] i32 GetHeight() const;

        /// @brief Returns the bounding box of all leaves (Empty() when no leaves).
        [[nodiscard]] AABB GetRootBounds() const;

    private:
        /// @brief Sentinel index meaning "no node"; stored in leaf child slots and an empty root.
        static constexpr i32 NullNode = -1;

        struct Node
        {
            /// @brief Enclosing box (for a leaf, the tight box).
            AABB Box;
            /// @brief Children for internal nodes; both NullNode on a leaf.
            i32 Child1, Child2;
            /// @brief Leaf payload (unused on internal nodes).
            u32 Id;
            /// @brief Returns true when this node is a leaf (both children are `NullNode`).
            [[nodiscard]] bool IsLeaf() const { return Child1 == NullNode; }
        };

        /// @brief A leaf being built, with its centroid computed once.
        struct BuildLeaf
        {
            /// @brief The leaf.
            Leaf Item;
            /// @brief The centre of the leaf's box.
            vec3 Centroid;
        };

        /// @brief Recursively builds the subtree over `leaves`; returns the subtree root index.
        i32 BuildRange(std::span<BuildLeaf> leaves);

        /// @brief Recomputes m_Cost from the current node boxes.
        void UpdateCost();

        /// @brief Flat pool of all nodes, indexed by i32; order is build-time push_back order.
        ///
        /// A node is pushed after both its children, so every child precedes its parent and one
        /// forward pass visits the tree bottom-up.
        vector<Node> m_Nodes;
        /// @brief Build scratch, reused so a rebuild of a same-size set allocates nothing.
        vector<BuildLeaf> m_BuildScratch;
        /// @brief Index of the root node; `NullNode` when the tree is empty.
        i32 m_Root = NullNode;
        /// @brief Number of leaf nodes in the tree.
        u32 m_LeafCount = 0;
        /// @brief The current surface-area cost.
        f32 m_Cost = 0.0f;
        /// @brief The surface-area cost at the last Build.
        f32 m_BuildCost = 0.0f;
    };

    namespace Detail
    {
        /// @brief The split a node's SAH bucket sweep chose.
        struct BucketSplit
        {
            /// @brief Buckets below this index go left; -1 when no split leaves both sides non-empty.
            i32 Split = -1;
            /// @brief The split's cost: each side's surface area times its leaf count, summed.
            f32 Cost = std::numeric_limits<f32>::infinity();
        };

        /// @brief Picks the bucket boundary with the least SAH cost, in one pass each way.
        ///
        /// Suffix bounds and counts are accumulated right to left, then the left side is grown left to
        /// right and each boundary is costed against the suffix, so the sweep is linear in the bucket
        /// count. A boundary leaving either side empty is skipped.
        /// @param boxes   The union of each bucket's leaf boxes.
        /// @param counts  The number of leaves in each bucket.
        /// @return The least-cost boundary, or Split -1 when every leaf is in one bucket.
        [[nodiscard]] BucketSplit BestBucketSplit(std::span<const AABB, BVH::BucketCount> boxes,
                                                  std::span<const u32, BVH::BucketCount> counts);
    }
}
