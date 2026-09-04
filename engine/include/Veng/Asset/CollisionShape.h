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
        /// @brief A set of transformed child shapes, built into one compound solver shape.
        ///
        /// The shape is Children, each a primitive or its own hull/mesh placed by a local
        /// translation and rotation. A compound whose every child is a primitive or convex hull
        /// may back any body and be swept; one carrying a Mesh child inherits the triangle mesh's
        /// Static-or-Kinematic restriction.
        Compound = 2,
    };

    /// @brief The shape one CollisionChild carries within a Compound CollisionShape.
    ///
    /// Integer values are stable — persisted in cooked blobs and mirrored by
    /// CookedCollisionChildKind.
    enum class CollisionChildKind : u32
    {
        /// @brief An axis-aligned box before the child transform; Extents are its half sizes.
        Box = 0,
        /// @brief A sphere; Extents.x is the radius.
        Sphere = 1,
        /// @brief A capsule about local Y; Extents.x is the radius, Extents.y the half height.
        Capsule = 2,
        /// @brief A convex hull, stored as the child's own hull vertices in Points.
        Convex = 3,
        /// @brief A triangle mesh, stored as the child's own Points and Indices.
        Mesh = 4,
    };

    /// @brief One transformed child shape in a Compound CollisionShape.
    ///
    /// A primitive child (Box/Sphere/Capsule) is fully described by Kind and Extents; a Convex or
    /// Mesh child carries its own geometry in Points (and, for a Mesh, Indices). Offset and
    /// Rotation place the child in the compound's local frame.
    struct CollisionChild
    {
        /// @brief Which shape this child is.
        CollisionChildKind Kind = CollisionChildKind::Box;
        /// @brief Primitive dimensions (box half sizes; sphere radius in x; capsule radius in x,
        ///        half height in y); unused for a Convex or Mesh child.
        vec3 Extents = vec3(0.0f);
        /// @brief The child's centre in the compound's local frame, in metres.
        vec3 Offset = vec3(0.0f);
        /// @brief The child's orientation in the compound's local frame.
        quat Rotation = quat(1.0f, 0.0f, 0.0f, 0.0f);
        /// @brief Convex/Mesh child vertices in the compound's local frame; empty for a primitive.
        vector<vec3> Points;
        /// @brief Mesh child triangle indices into Points; empty for a non-Mesh child.
        vector<u32> Indices;

        /// @brief Returns the number of triangles; zero for a non-Mesh child.
        [[nodiscard]] usize GetTriangleCount() const { return Indices.size() / 3; }
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
        /// @brief Which geometry the shape carries.
        CollisionGeometry Geometry = CollisionGeometry::Convex;
        /// @brief Vertex positions in the shape's local frame, in metres; unused under Compound.
        vector<vec3> Points;
        /// @brief Triangle indices into Points; empty under Convex and Compound.
        vector<u32> Indices;
        /// @brief The compound's child shapes; empty under Convex and Mesh.
        vector<CollisionChild> Children;

        /// @brief Returns the number of top-level triangles; zero under Convex and Compound.
        [[nodiscard]] usize GetTriangleCount() const { return Indices.size() / 3; }

        /// @brief Whether the shape, or any compound child, is a triangle mesh.
        ///
        /// A triangle mesh may not back a Dynamic body and may not be swept; a compound inherits
        /// that restriction from a single Mesh child. Convex hulls and primitives never do.
        [[nodiscard]] bool ContainsTriangleMesh() const
        {
            if (Geometry == CollisionGeometry::Mesh)
            {
                return true;
            }
            for (const CollisionChild& child : Children)
            {
                if (child.Kind == CollisionChildKind::Mesh)
                {
                    return true;
                }
            }
            return false;
        }
    };

    /// @brief AssetTypeTrait specialization mapping CollisionShape to AssetTypes::CollisionShape.
    template <>
    struct AssetTypeTrait<CollisionShape>
    {
        /// @brief The asset type tag for CollisionShape.
        static constexpr AssetTypeId Type = AssetTypes::CollisionShape;
    };
}
