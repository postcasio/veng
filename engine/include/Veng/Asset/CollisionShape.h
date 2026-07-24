#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetType.h>

namespace Veng
{
    /// @brief Which geometry a CollisionShape carries.
    ///
    /// Integer values are stable — persisted in cooked blobs and mirrored by
    /// CookedCollisionGeometry.
    enum class CollisionGeometry : u32
    {
        /// @brief A convex hull, stored as the hull's vertices.
        ///
        /// The cook reduces the source model to its hull vertices, so the runtime builds a hull
        /// over a small point set rather than over every vertex of a render mesh. Any body may
        /// use one.
        Convex = 0,
        /// @brief An indexed triangle mesh, for geometry the solver never integrates.
        ///
        /// A triangle mesh has no interior and no inertia, so it may back a Static or Kinematic
        /// body only; a Dynamic body carrying one is a fatal assert at body creation rather than
        /// a solver that misbehaves for reasons the consumer cannot see.
        Mesh = 1,
    };

    /// @brief Solver-neutral collision geometry, loaded by AssetId.
    ///
    /// A CPU-only asset (no GPU resource) authored from a model at cook time and referenced by a
    /// Collider. The geometry is engine-owned rather than a solver's serialized shape: the solver
    /// builds its own shape from these points at load, so bumping the solver is a rebuild rather
    /// than a re-cook of every pack, and the asset layer names no third-party format.
    ///
    /// A render mesh is the wrong input to a solver — too dense, non-manifold, and split by
    /// material — so this is a separate asset with its own source, not a view onto a Mesh.
    struct CollisionShape
    {
        /// @brief Which geometry Points and Indices describe.
        CollisionGeometry Geometry = CollisionGeometry::Convex;
        /// @brief Vertex positions in the shape's local frame, in metres.
        vector<vec3> Points;
        /// @brief Triangle indices into Points; empty under CollisionGeometry::Convex.
        vector<u32> Indices;

        /// @brief Returns the number of triangles; zero under CollisionGeometry::Convex.
        [[nodiscard]] usize GetTriangleCount() const { return Indices.size() / 3; }
    };

    /// @brief AssetTypeTrait specialization mapping CollisionShape to AssetTypes::CollisionShape.
    template <>
    struct AssetTypeTrait<CollisionShape>
    {
        /// @brief The asset type tag for CollisionShape.
        static constexpr AssetTypeId Type = AssetTypes::CollisionShape;
    };
}
