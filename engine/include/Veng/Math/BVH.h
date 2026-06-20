#pragma once

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
    /// Query() returns leaf ids whose tight box intersects a frustum. Rebuilt from scratch
    /// when its source set changes; no incremental maintenance.
    class BVH
    {
    public:
        /// @brief One BVH leaf: a tight world-space box and a caller-supplied payload id.
        struct Leaf
        {
            AABB Box;
            u32 Id;
        };

        /// @brief Builds the tree over `leaves`, replacing any prior contents.
        ///
        /// O(N log N) top-down: recursively splits the set by the surface-area-minimizing
        /// partition along the longest centroid axis. Empty input yields an empty tree.
        void Build(std::span<const Leaf> leaves);

        /// @brief Appends the ids of every leaf whose box intersects `frustum` to `out` (not cleared).
        ///
        /// Descends internal nodes by their enclosing box; tests leaves by their tight box.
        void Query(const Frustum& frustum, vector<u32>& out) const;

        /// @brief Returns the number of leaf nodes.
        [[nodiscard]] u32  GetLeafCount() const { return m_LeafCount; }

        /// @brief Returns the total number of nodes (internal + leaf).
        [[nodiscard]] u32  GetNodeCount() const { return static_cast<u32>(m_Nodes.size()); }

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

        /// @brief Recursively builds the subtree over `leaves`; returns the subtree root index.
        i32 BuildRange(std::span<Leaf> leaves);

        /// @brief Flat pool of all nodes, indexed by i32; order is build-time push_back order.
        vector<Node> m_Nodes;
        /// @brief Index of the root node; `NullNode` when the tree is empty.
        i32 m_Root      = NullNode;
        /// @brief Number of leaf nodes in the tree.
        u32 m_LeafCount = 0;
    };
}
