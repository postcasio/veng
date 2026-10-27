#pragma once

#include <Veng/Veng.h>
#include <Veng/Result.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetType.h>

#include <limits>
#include <span>

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
        /// @brief A regular grid of heights, held in CollisionShape::HeightField.
        ///
        /// A heightfield is a surface, as a triangle mesh is: it may back a Static or Kinematic body
        /// only and may not be swept. It is built at run time and adopted, never cooked; the cooked
        /// format reserves the value so the two enums stay aligned, and the loader refuses it.
        HeightField = 3,
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

    /// @brief A regular grid of heights: the data of a HeightField CollisionShape.
    ///
    /// The surface is the triangulated grid through the points `Origin + (x * Spacing,
    /// Heights[z * SampleCount + x], z * Spacing)` for integer x and z in [0, SampleCount), in the
    /// shape's local frame, with heights along local +Y. Origin places sample (0, 0), so a body's
    /// origin can sit at the grid's centre (Origin = (-half, 0, -half) with half = (SampleCount - 1)
    /// * Spacing / 2) or at any other point a consumer's lattice prefers; the body's pose and the
    /// Collider's Offset and Rotation place and turn the grid as for any shape.
    ///
    /// The coordinates are shape-local f32 about the body's double-precision pose, so a grid a
    /// kilometre across resolves about 0.1 mm at its edge wherever in the world it stands. A large
    /// world places such a body through PhysicsPose (RigidBody::SyncTransform cleared), since
    /// Transform is f32.
    ///
    /// The solver stores the heights quantised. MaxError bounds the error the per-block packing may
    /// add, and the whole grid is first quantised to 16 bits over its height range, so the error is
    /// never below half of (highest - lowest) / 65534: about 0.8 mm per 100 m of relief.
    struct CollisionHeightField
    {
        /// @brief The height marking a sample with no collision; a cell touching one loses the
        ///        triangles that would use it.
        static constexpr f32 Hole = std::numeric_limits<f32>::infinity();
        /// @brief The fewest samples along each side a heightfield may have.
        static constexpr u32 MinSampleCount = 3;
        /// @brief The most samples along each side a heightfield may have.
        static constexpr u32 MaxSampleCount = 32769;

        /// @brief N: the grid holds N x N samples and (N - 1) x (N - 1) cells; at least 3.
        u32 SampleCount = 0;
        /// @brief Metres between neighbouring samples along local X and Z; positive.
        f32 Spacing = 1.0f;
        /// @brief The local position of sample (0, 0), in metres.
        vec3 Origin = vec3(0.0f);
        /// @brief SampleCount x SampleCount heights in metres along local +Y, row-major (z * N + x),
        ///        each finite or Hole.
        vector<f32> Heights;
        /// @brief Empty, or one surface tag per cell, row-major ((N - 1) x (N - 1), z * (N - 1) + x).
        ///
        /// A tag's meaning is the consumer's — a footstep sound, a dust colour, "not walkable". The
        /// engine attaches no friction or restitution to it; it is reported on the queries' hits
        /// and the character's ground state.
        vector<u8> CellTags;
        /// @brief The quantisation error bound in metres; positive.
        f32 MaxError = 0.001f;
    };

    /// @brief A solver shape CollisionShape::Prepare built, opaque outside the physics module.
    struct PreparedCollisionShape;

    /// @brief Solver-neutral collision geometry, loaded by AssetId.
    ///
    /// A CPU-only asset (no GPU resource) authored from a model at cook time and referenced by a
    /// Collider. The geometry is engine-owned rather than a solver's serialized shape: the solver
    /// builds its own shape from these points at load, so bumping the solver is a rebuild rather
    /// than a re-cook of every pack, and the asset layer names no third-party format.
    ///
    /// A render mesh is the wrong input to a solver — too dense, non-manifold, and split by
    /// material — so this is a separate asset with its own source, not a view onto a Mesh.
    struct VE_API CollisionShape
    {
        /// @brief Which geometry the shape carries.
        CollisionGeometry Geometry = CollisionGeometry::Convex;
        /// @brief Vertex positions in the shape's local frame, in metres; unused under Compound.
        vector<vec3> Points;
        /// @brief Triangle indices into Points; empty under Convex and Compound.
        vector<u32> Indices;
        /// @brief The compound's child shapes; empty under Convex and Mesh.
        vector<CollisionChild> Children;
        /// @brief The grid of heights under HeightField; unused otherwise.
        CollisionHeightField HeightField;
        /// @brief The solver shape Prepare built, or null; runtime-only and never serialised.
        ///
        /// A copy of the shape shares it, so a copy edited after Prepare carries a stale one: set
        /// the geometry, then prepare.
        mutable Ref<const PreparedCollisionShape> Prepared;

        /// @brief Returns the number of top-level triangles; zero under Convex and Compound.
        [[nodiscard]] usize GetTriangleCount() const { return Indices.size() / 3; }

        /// @brief Whether the shape, or any compound child, is a surface: a triangle mesh or a
        ///        heightfield.
        ///
        /// A surface may not back a Dynamic body and may not be swept; a compound inherits that
        /// restriction from a single Mesh child. Convex hulls and primitives never do.
        [[nodiscard]] bool ContainsTriangleMesh() const
        {
            if (Geometry == CollisionGeometry::Mesh || Geometry == CollisionGeometry::HeightField)
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

        /// @brief Checks that @p points make a convex hull a body can be built on, without building one.
        ///
        /// A world building a body on Convex geometry treats an unbuildable hull as a fatal assert:
        /// a cooked hull that fails is a build error. A producer making hull points at run time
        /// checks them here first and gets the reason back instead. The points must be finite and
        /// must enclose a volume: fewer than four, coincident, collinear and coplanar sets are all
        /// refused, since a flat hull has no inertia to give a dynamic body. Passing means the same
        /// points build without asserting.
        ///
        /// Thread-safe. It needs the solver's process registration, which it holds for the call,
        /// so it is cheapest while a PhysicsWorld is alive.
        /// @param points  The candidate hull vertices, in the shape's local frame.
        /// @return Success, or an error naming why the points make no hull.
        [[nodiscard]] static VE_API VoidResult ValidateConvex(std::span<const vec3> points);

        /// @brief Checks that @p field describes a heightfield a body can be built on.
        ///
        /// Refuses, with the reason: a sample count outside [MinSampleCount, MaxSampleCount], a
        /// height array whose length is not SampleCount squared, a height that is neither finite nor
        /// Hole, a spacing that is not a positive finite number, a tag array that is neither empty
        /// nor (SampleCount - 1) squared long, and a MaxError that is not a positive finite number.
        /// Passing means the field builds without asserting; a world building one that fails
        /// asserts, as it does for an unbuildable hull. Never asserts itself.
        /// @param field  The candidate grid.
        /// @return Success, or an error naming what is wrong.
        [[nodiscard]] static VoidResult ValidateHeightField(const CollisionHeightField& field);

        /// @brief Builds the solver shape for this geometry and keeps it in Prepared.
        ///
        /// The off-thread seam for run-time geometry: a streamer generates the geometry on a
        /// TaskSystem worker, calls Prepare there, and adopts the shape on the main thread, so the
        /// world's step creates the body at the cost of a broad-phase insertion rather than a shape
        /// build. A world building a body or a query on this geometry uses the prepared shape. Works
        /// for every geometry, a convex hull, a mesh and a compound as much as a heightfield.
        ///
        /// Idempotent: a prepared shape is not rebuilt. Safe on any thread, though not on two
        /// threads at once for one shape, and it must finish before the shape is handed to a world.
        /// It holds the solver's process registration for the call, as ValidateConvex does. The
        /// prepared shape is never serialised, so a solver bump stays a rebuild rather than a
        /// re-cook.
        /// @pre The geometry is buildable (ValidateConvex / ValidateHeightField pass); unbuildable
        ///      geometry is a fatal assert, as it is in a world.
        void Prepare() const;

        /// @brief Whether Prepare has built this shape's solver shape.
        [[nodiscard]] bool IsPrepared() const { return Prepared != nullptr; }

        /// @brief Returns the identity of the prepared solver shape, or null when unprepared.
        ///
        /// An opaque address comparable with PhysicsWorld::FindBuiltShape's, for tests and
        /// diagnostics, never to be dereferenced.
        [[nodiscard]] const void* GetPreparedShape() const;
    };

    /// @brief AssetTypeTrait specialization mapping CollisionShape to AssetTypes::CollisionShape.
    template <>
    struct AssetTypeTrait<CollisionShape>
    {
        /// @brief The asset type tag for CollisionShape.
        static constexpr AssetTypeId Type = AssetTypes::CollisionShape;
    };
}
