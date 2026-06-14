#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/Mesh.h>   // MeshData, CanonicalVertex

// Runtime primitive-mesh generators. Each returns CPU-side MeshData in the
// canonical vertex layout (Mesh::CanonicalLayout()); upload it with
// Mesh::Create(context, data, name). Geometry is generated with analytic
// normals, tangents (xyz + handedness w), and UVs. A valid `material` handle is
// recorded on the produced submesh (the mesh owns it; the draw loop binds it);
// an empty handle leaves the submesh unassigned (SubMesh::NoMaterial).
namespace Veng::Primitives
{
    // Axis-aligned cube centered at the origin, `extent` units across the full
    // width (so ±extent/2 per axis). 24 vertices (4 per face, hard normals),
    // 36 indices, one submesh. Per-face UVs span [0,1].
    [[nodiscard]] MeshData Cube(f32 extent = 1.0f, AssetHandle<Material> material = {});

    // Flat plane in the XZ plane (+Y normal) centered at the origin, `size`
    // units wide/deep, tessellated into `subdivisions` quads per axis (min 1).
    // UVs span [0,1] across the plane.
    [[nodiscard]] MeshData Plane(vec2 size = vec2(1.0f),
                                 uvec2 subdivisions = uvec2(1),
                                 AssetHandle<Material> material = {});

    // UV sphere of `radius`, `rings` latitude bands and `segments` longitude
    // bands (min 3 each). Smooth normals; UVs are (longitude, latitude). One
    // submesh.
    [[nodiscard]] MeshData Sphere(f32 radius = 0.5f,
                                  u32 rings = 16, u32 segments = 32,
                                  AssetHandle<Material> material = {});
}
