#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/Mesh.h> // MeshData, CanonicalVertex

/// @brief Runtime primitive mesh generators.
///
/// Each function returns CPU-side MeshData in the canonical vertex layout
/// (Mesh::CanonicalLayout()); upload with Mesh::BuildSync(context, data, name).
/// Geometry is generated with analytic normals, tangents (xyz + handedness w),
/// and UVs. A valid material handle is recorded on the produced submesh
/// (the mesh owns it; the draw loop binds it); an empty handle leaves the
/// submesh unassigned (SubMesh::NoMaterial).
namespace Veng::Primitives
{
    /// @brief Axis-aligned cube centered at the origin, `extent` units across the full width (±extent/2 per axis).
    ///
    /// 24 vertices (4 per face, hard normals), 36 indices, one submesh. Per-face UVs span [0,1].
    [[nodiscard]] MeshData Cube(f32 extent = 1.0f, AssetHandle<Material> material = {});

    /// @brief Flat XZ-plane quad (+Y normal) centered at the origin, tessellated into `subdivisions` quads per axis.
    ///
    /// UVs span [0,1] across the plane. Minimum 1 subdivision per axis.
    [[nodiscard]] MeshData Plane(vec2 size = vec2(1.0f), uvec2 subdivisions = uvec2(1),
                                 AssetHandle<Material> material = {});

    /// @brief UV sphere of `radius` with `rings` latitude bands and `segments` longitude bands (min 3 each).
    ///
    /// Smooth normals; UVs are (longitude, latitude). One submesh.
    [[nodiscard]] MeshData Sphere(f32 radius = 0.5f, u32 rings = 16, u32 segments = 32,
                                  AssetHandle<Material> material = {});

    /// @brief Geodesic sphere of `radius` from a subdivided icosahedron.
    ///
    /// Each of the 20 base faces is split `subdivisions` times (4^subdivisions triangles each)
    /// and projected onto the sphere, so vertices are near-uniformly distributed with no pole
    /// clustering. Smooth normals; equirectangular UVs with the wrap seam split to avoid smearing.
    /// One submesh.
    [[nodiscard]] MeshData Icosphere(f32 radius = 0.5f, u32 subdivisions = 3,
                                     AssetHandle<Material> material = {});

    /// @brief Capped cylinder about the Y axis, `radius` across and `height` tall, centered at the origin.
    ///
    /// A radial side band of `segments` longitude columns (min 3) plus a top and bottom cap fan,
    /// each with its own hard +Y / -Y normal. Side normals point radially outward; side UVs are
    /// (longitude, height). One submesh.
    [[nodiscard]] MeshData Cylinder(f32 radius = 0.5f, f32 height = 1.0f, u32 segments = 32,
                                    AssetHandle<Material> material = {});

    /// @brief Cone about the Y axis: a circular base of `radius` and an apex `height` above it, centered at the origin.
    ///
    /// A side band of `segments` longitude columns (min 3) whose apex ring duplicates the apex
    /// per segment so each side face carries its own slanted normal, plus a bottom cap fan with a
    /// hard -Y normal. One submesh.
    [[nodiscard]] MeshData Cone(f32 radius = 0.5f, f32 height = 1.0f, u32 segments = 32,
                                AssetHandle<Material> material = {});

    /// @brief Torus in the XZ plane: a tube of `minorRadius` swept around `majorRadius`, centered at the origin.
    ///
    /// `majorSegments` columns around the ring and `minorSegments` columns around the tube (min 3
    /// each); seam columns duplicate so UVs do not wrap. Smooth normals point away from the tube
    /// center circle; UVs are (major angle, minor angle). One submesh.
    [[nodiscard]] MeshData Torus(f32 majorRadius = 0.5f, f32 minorRadius = 0.2f,
                                 u32 majorSegments = 32, u32 minorSegments = 16,
                                 AssetHandle<Material> material = {});

    /// @brief Capsule about the Y axis: a cylinder of `height` capped by two hemispheres of `radius`, centered at the origin.
    ///
    /// `segments` longitude columns (min 3) shared by the band and both caps; each hemisphere has
    /// `rings` latitude bands (min 1). The cylinder spans the central `height`; the hemisphere
    /// centers sit at ±height/2, so the full extent is height + 2*radius. Smooth normals; one submesh.
    [[nodiscard]] MeshData Capsule(f32 radius = 0.5f, f32 height = 1.0f, u32 segments = 32,
                                   u32 rings = 8, AssetHandle<Material> material = {});
}
