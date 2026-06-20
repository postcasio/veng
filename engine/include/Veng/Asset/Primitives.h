#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/Mesh.h>   // MeshData, CanonicalVertex

/// @brief Runtime primitive mesh generators.
///
/// Each function returns CPU-side MeshData in the canonical vertex layout
/// (Mesh::CanonicalLayout()); upload with Mesh::Create(context, data, name).
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
    [[nodiscard]] MeshData Plane(vec2 size = vec2(1.0f),
                                 uvec2 subdivisions = uvec2(1),
                                 AssetHandle<Material> material = {});

    /// @brief UV sphere of `radius` with `rings` latitude bands and `segments` longitude bands (min 3 each).
    ///
    /// Smooth normals; UVs are (longitude, latitude). One submesh.
    [[nodiscard]] MeshData Sphere(f32 radius = 0.5f,
                                  u32 rings = 16, u32 segments = 32,
                                  AssetHandle<Material> material = {});

    /// @brief Geodesic sphere of `radius` from a subdivided icosahedron.
    ///
    /// Each of the 20 base faces is split `subdivisions` times (4^subdivisions triangles each)
    /// and projected onto the sphere, so vertices are near-uniformly distributed with no pole
    /// clustering. Smooth normals; equirectangular UVs with the wrap seam split to avoid smearing.
    /// One submesh.
    [[nodiscard]] MeshData Icosphere(f32 radius = 0.5f, u32 subdivisions = 3,
                                     AssetHandle<Material> material = {});
}
