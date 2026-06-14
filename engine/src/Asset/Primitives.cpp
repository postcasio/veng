#include <Veng/Asset/Primitives.h>

#include <algorithm>
#include <cmath>

#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>

namespace Veng::Primitives
{
    namespace
    {
        // Finishes a primitive's single submesh from the material handle: a
        // valid handle is recorded as the mesh's one resident material and
        // indexed by the submesh; an empty handle leaves the submesh
        // unassigned (SubMesh::NoMaterial) and the material list empty.
        void FinishSubMesh(MeshData& data, AssetHandle<Material> material)
        {
            const u32 indexCount = static_cast<u32>(data.Indices.size());

            u32 materialIndex = SubMesh::NoMaterial;
            if (material)
            {
                materialIndex = static_cast<u32>(data.Materials.size());
                data.Materials.push_back(std::move(material));
            }

            data.SubMeshes.push_back(SubMesh{
                .IndexOffset = 0,
                .IndexCount = indexCount,
                .MaterialIndex = materialIndex,
            });
        }
    }

    MeshData Cube(f32 extent, AssetHandle<Material> material)
    {
        const f32 h = extent * 0.5f;

        // Six faces, each with its own normal and tangent (hard edges). Per
        // face: the outward normal, the U-direction tangent, and the V
        // direction; the four corners are normal-centered plus ±U/±V. UVs span
        // the unit square; handedness w = +1 (UVs are not mirrored).
        struct Face
        {
            vec3 Normal;
            vec3 Tangent;
            vec3 Bitangent;
        };

        const Face faces[6] = {
            {{+1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, +1.0f, 0.0f}}, // +X
            {{-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, +1.0f}, {0.0f, +1.0f, 0.0f}}, // -X
            {{0.0f, +1.0f, 0.0f}, {+1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, +1.0f}}, // +Y
            {{0.0f, -1.0f, 0.0f}, {+1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}}, // -Y
            {{0.0f, 0.0f, +1.0f}, {+1.0f, 0.0f, 0.0f}, {0.0f, +1.0f, 0.0f}}, // +Z
            {{0.0f, 0.0f, -1.0f}, {-1.0f, 0.0f, 0.0f}, {0.0f, +1.0f, 0.0f}}, // -Z
        };

        MeshData data;
        data.Vertices.reserve(24);
        data.Indices.reserve(36);

        for (const Face& face : faces)
        {
            const u32 base = static_cast<u32>(data.Vertices.size());

            // Corners ordered so the index pattern below winds CCW when viewed
            // from outside (along -normal toward the face).
            const vec2 corners[4] = {
                {0.0f, 0.0f}, // -U -V
                {1.0f, 0.0f}, // +U -V
                {1.0f, 1.0f}, // +U +V
                {0.0f, 1.0f}, // -U +V
            };

            for (const vec2& uv : corners)
            {
                const f32 u = uv.x * 2.0f - 1.0f;
                const f32 v = uv.y * 2.0f - 1.0f;
                const vec3 position = (face.Normal + face.Tangent * u + face.Bitangent * v) * h;

                data.Vertices.push_back(CanonicalVertex{
                    .Position = position,
                    .Normal = face.Normal,
                    .Tangent = vec4(face.Tangent, 1.0f),
                    .UV = uv,
                });
            }

            data.Indices.push_back(base + 0);
            data.Indices.push_back(base + 1);
            data.Indices.push_back(base + 2);
            data.Indices.push_back(base + 0);
            data.Indices.push_back(base + 2);
            data.Indices.push_back(base + 3);
        }

        FinishSubMesh(data, std::move(material));
        return data;
    }

    MeshData Plane(vec2 size, uvec2 subdivisions, AssetHandle<Material> material)
    {
        const u32 sx = std::max(1u, subdivisions.x);
        const u32 sz = std::max(1u, subdivisions.y);

        const vec2 half = size * 0.5f;

        MeshData data;
        data.Vertices.reserve(static_cast<usize>(sx + 1) * (sz + 1));
        data.Indices.reserve(static_cast<usize>(sx) * sz * 6);

        // (sx+1) x (sz+1) grid of vertices on the XZ plane, +Y normal.
        for (u32 j = 0; j <= sz; ++j)
        {
            const f32 tz = static_cast<f32>(j) / static_cast<f32>(sz);
            for (u32 i = 0; i <= sx; ++i)
            {
                const f32 tx = static_cast<f32>(i) / static_cast<f32>(sx);

                data.Vertices.push_back(CanonicalVertex{
                    .Position = vec3(half.x * (tx * 2.0f - 1.0f), 0.0f, half.y * (tz * 2.0f - 1.0f)),
                    .Normal = vec3(0.0f, 1.0f, 0.0f),
                    .Tangent = vec4(1.0f, 0.0f, 0.0f, 1.0f),
                    .UV = vec2(tx, tz),
                });
            }
        }

        const u32 stride = sx + 1;
        for (u32 j = 0; j < sz; ++j)
        {
            for (u32 i = 0; i < sx; ++i)
            {
                const u32 a = j * stride + i;
                const u32 b = a + 1;
                const u32 c = a + stride;
                const u32 d = c + 1;

                // CCW seen from above (+Y looking down -Y).
                data.Indices.push_back(a);
                data.Indices.push_back(c);
                data.Indices.push_back(b);

                data.Indices.push_back(b);
                data.Indices.push_back(c);
                data.Indices.push_back(d);
            }
        }

        FinishSubMesh(data, std::move(material));
        return data;
    }

    MeshData Sphere(f32 radius, u32 rings, u32 segments, AssetHandle<Material> material)
    {
        rings = std::max(3u, rings);
        segments = std::max(3u, segments);

        MeshData data;
        data.Vertices.reserve(static_cast<usize>(rings + 1) * (segments + 1));
        data.Indices.reserve(static_cast<usize>(rings) * segments * 6);

        constexpr f32 Pi = 3.14159265358979323846f;

        // (rings+1) latitude rows x (segments+1) longitude columns. theta runs
        // 0..pi from the +Y pole; phi runs 0..2pi. The seam column (phi = 2pi)
        // duplicates the phi = 0 verts with UV.x = 1 so UVs do not wrap.
        for (u32 r = 0; r <= rings; ++r)
        {
            const f32 vt = static_cast<f32>(r) / static_cast<f32>(rings);
            const f32 theta = vt * Pi;
            const f32 sinTheta = std::sin(theta);
            const f32 cosTheta = std::cos(theta);

            for (u32 s = 0; s <= segments; ++s)
            {
                const f32 vu = static_cast<f32>(s) / static_cast<f32>(segments);
                const f32 phi = vu * 2.0f * Pi;
                const f32 sinPhi = std::sin(phi);
                const f32 cosPhi = std::cos(phi);

                const vec3 normal = vec3(sinTheta * cosPhi, cosTheta, sinTheta * sinPhi);

                // d(position)/d(phi), normalized: the +U (longitude) direction.
                // Degenerate at the poles (sinTheta = 0); fall back to +X there.
                vec3 tangent = vec3(-sinPhi, 0.0f, cosPhi);
                if (sinTheta <= 1e-6f)
                    tangent = vec3(1.0f, 0.0f, 0.0f);

                data.Vertices.push_back(CanonicalVertex{
                    .Position = normal * radius,
                    .Normal = normal,
                    .Tangent = vec4(glm::normalize(tangent), 1.0f),
                    .UV = vec2(vu, vt),
                });
            }
        }

        const u32 stride = segments + 1;
        for (u32 r = 0; r < rings; ++r)
        {
            for (u32 s = 0; s < segments; ++s)
            {
                const u32 a = r * stride + s;
                const u32 b = a + 1;
                const u32 c = a + stride;
                const u32 d = c + 1;

                // Skip the degenerate triangle at each pole cap (one edge
                // collapses to the pole), emitting only the non-degenerate one.
                if (r != 0)
                {
                    data.Indices.push_back(a);
                    data.Indices.push_back(c);
                    data.Indices.push_back(b);
                }
                if (r != rings - 1)
                {
                    data.Indices.push_back(b);
                    data.Indices.push_back(c);
                    data.Indices.push_back(d);
                }
            }
        }

        FinishSubMesh(data, std::move(material));
        return data;
    }
}
