// Primitive-generator unit cases: pure CPU geometry, no Vulkan device.
// These pin the math — counts, triangle winding, unit-length normals/tangents,
// AABB extents, and the default no-material wiring. Most generators here are
// called with the empty default handle; the last case covers the *pending*
// handle an asynchronous build returns, which needs a manager but no device.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

#include <glm/geometric.hpp>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Primitives.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Task/TaskSystem.h>

using namespace Veng;

namespace
{
    struct Aabb
    {
        vec3 Min;
        vec3 Max;
    };

    Aabb ComputeAabb(const MeshData& data)
    {
        REQUIRE_FALSE(data.Vertices.empty());
        Aabb box{.Min = data.Vertices[0].Position, .Max = data.Vertices[0].Position};
        for (const CanonicalVertex& v : data.Vertices)
        {
            box.Min = glm::min(box.Min, v.Position);
            box.Max = glm::max(box.Max, v.Position);
        }
        return box;
    }

    // Every triangle's winding must agree with its vertices' shading normals: the
    // geometric face normal (cross of two edges, in index order) must point the same
    // way as the stored normals. The surface pipeline culls back faces with a
    // CCW-front winding, so a triangle wound the wrong way has its outward face
    // culled and renders inside-out — geometry that looks correct but shades as if
    // its normals were reversed. The existing per-vertex outward-normal checks pass
    // regardless of winding, so this is the invariant that catches it.
    void CheckWindingMatchesNormals(const MeshData& data)
    {
        for (usize i = 0; i + 2 < data.Indices.size(); i += 3)
        {
            const CanonicalVertex& v0 = data.Vertices[data.Indices[i + 0]];
            const CanonicalVertex& v1 = data.Vertices[data.Indices[i + 1]];
            const CanonicalVertex& v2 = data.Vertices[data.Indices[i + 2]];

            const vec3 geometric = glm::cross(v1.Position - v0.Position, v2.Position - v0.Position);

            // A degenerate (zero-area) triangle has no winding to check.
            if (glm::length(geometric) <= 1e-12f)
            {
                continue;
            }

            const vec3 shading = v0.Normal + v1.Normal + v2.Normal;
            CHECK(glm::dot(geometric, shading) > 0.0f);
        }
    }

    // Shared invariants every primitive's MeshData must satisfy. `expectedSubMeshes` is the
    // submesh count the generator's arguments call for; whatever the count, the submeshes must
    // tile the index range end to end with no gap and no overlap.
    void CheckCommonInvariants(const MeshData& data, usize expectedSubMeshes = 1)
    {
        // Triangle list.
        CHECK(data.Indices.size() % 3 == 0);
        CHECK_FALSE(data.Indices.empty());

        // Every index is in bounds.
        for (const u32 index : data.Indices)
        {
            CHECK(index < data.Vertices.size());
        }

        REQUIRE(data.SubMeshes.size() == expectedSubMeshes);

        // No-material default: empty list, unassigned submeshes.
        CHECK(data.Materials.empty());

        usize covered = 0;
        for (const SubMesh& submesh : data.SubMeshes)
        {
            CHECK(submesh.IndexOffset == covered);
            CHECK(submesh.IndexCount % 3 == 0);
            CHECK(submesh.MaterialIndex == SubMesh::NoMaterial);
            covered += submesh.IndexCount;
        }
        CHECK(covered == data.Indices.size());

        // Normals unit length; tangents unit (xyz), orthogonal to normal, w = ±1.
        for (const CanonicalVertex& v : data.Vertices)
        {
            CHECK(glm::length(v.Normal) == doctest::Approx(1.0f).epsilon(0.001f));

            const vec3 tangentXyz(v.Tangent);
            CHECK(glm::length(tangentXyz) == doctest::Approx(1.0f).epsilon(0.001f));
            CHECK(std::abs(glm::dot(tangentXyz, v.Normal)) ==
                  doctest::Approx(0.0f).epsilon(0.001f));
            CHECK(std::abs(v.Tangent.w) == doctest::Approx(1.0f));
        }

        CheckWindingMatchesNormals(data);
    }

    // Two generator calls produced the same geometry: same grid, same indices, same submesh
    // ranges. Compared exactly, not approximately — the arguments are meant to reduce to the
    // identical arithmetic, so any difference at all is the failure.
    void CheckIdenticalGeometry(const MeshData& a, const MeshData& b)
    {
        REQUIRE(a.Vertices.size() == b.Vertices.size());
        REQUIRE(a.Indices.size() == b.Indices.size());
        REQUIRE(a.SubMeshes.size() == b.SubMeshes.size());

        for (usize i = 0; i < a.Vertices.size(); ++i)
        {
            CHECK(a.Vertices[i].Position == b.Vertices[i].Position);
            CHECK(a.Vertices[i].Normal == b.Vertices[i].Normal);
            CHECK(a.Vertices[i].Tangent == b.Vertices[i].Tangent);
            CHECK(a.Vertices[i].UV == b.Vertices[i].UV);
        }

        CHECK(a.Indices == b.Indices);

        for (usize i = 0; i < a.SubMeshes.size(); ++i)
        {
            CHECK(a.SubMeshes[i].IndexOffset == b.SubMeshes[i].IndexOffset);
            CHECK(a.SubMeshes[i].IndexCount == b.SubMeshes[i].IndexCount);
            CHECK(a.SubMeshes[i].MaterialIndex == b.SubMeshes[i].MaterialIndex);
        }
    }

    // Distance from the Y axis — the radius a vertex of a shape swept about that axis sits at.
    f32 RingRadius(const vec3& position)
    {
        return std::sqrt(position.x * position.x + position.z * position.z);
    }

    // Twice the area of the triangle at index-list offset `triangle * 3`, as the cross product
    // of two of its edges: zero length means a degenerate triangle.
    vec3 TriangleCross(const MeshData& data, usize triangle)
    {
        const vec3& p0 = data.Vertices[data.Indices[triangle * 3 + 0]].Position;
        const vec3& p1 = data.Vertices[data.Indices[triangle * 3 + 1]].Position;
        const vec3& p2 = data.Vertices[data.Indices[triangle * 3 + 2]].Position;
        return glm::cross(p1 - p0, p2 - p0);
    }
}

TEST_CASE("Cube: counts, invariants, and extent AABB")
{
    const f32 extent = 2.0f;
    const MeshData data = Primitives::Cube(extent);

    CHECK(data.Vertices.size() == 24);
    CHECK(data.Indices.size() == 36);
    CheckCommonInvariants(data);

    const Aabb box = ComputeAabb(data);
    const f32 h = extent * 0.5f;
    CHECK(box.Min.x == doctest::Approx(-h));
    CHECK(box.Min.y == doctest::Approx(-h));
    CHECK(box.Min.z == doctest::Approx(-h));
    CHECK(box.Max.x == doctest::Approx(+h));
    CHECK(box.Max.y == doctest::Approx(+h));
    CHECK(box.Max.z == doctest::Approx(+h));
}

TEST_CASE("Plane: counts scale with subdivisions, size AABB, +Y normal")
{
    const vec2 size(3.0f, 5.0f);
    const uvec2 subs(2, 4);
    const MeshData data = Primitives::Plane(size, subs);

    CHECK(data.Vertices.size() == static_cast<usize>(subs.x + 1) * (subs.y + 1));
    CHECK(data.Indices.size() == static_cast<usize>(subs.x) * subs.y * 6);
    CheckCommonInvariants(data);

    // Flat in XZ at Y = 0, +Y normal.
    for (const CanonicalVertex& v : data.Vertices)
    {
        CHECK(v.Position.y == doctest::Approx(0.0f));
        CHECK(v.Normal.x == doctest::Approx(0.0f));
        CHECK(v.Normal.y == doctest::Approx(1.0f));
        CHECK(v.Normal.z == doctest::Approx(0.0f));
    }

    const Aabb box = ComputeAabb(data);
    CHECK(box.Min.x == doctest::Approx(-size.x * 0.5f));
    CHECK(box.Max.x == doctest::Approx(+size.x * 0.5f));
    CHECK(box.Min.z == doctest::Approx(-size.y * 0.5f));
    CHECK(box.Max.z == doctest::Approx(+size.y * 0.5f));
}

TEST_CASE("Plane: subdivisions clamp to a minimum of 1")
{
    const MeshData clamped = Primitives::Plane(vec2(1.0f), uvec2(0));
    const MeshData explicitOne = Primitives::Plane(vec2(1.0f), uvec2(1));

    CHECK(clamped.Vertices.size() == explicitOne.Vertices.size());
    CHECK(clamped.Indices.size() == explicitOne.Indices.size());
    CHECK(clamped.Vertices.size() == 4);
    CHECK(clamped.Indices.size() == 6);
}

TEST_CASE("Plane: vertex count grows with subdivisions")
{
    const usize coarse = Primitives::Plane(vec2(1.0f), uvec2(1)).Vertices.size();
    const usize fine = Primitives::Plane(vec2(1.0f), uvec2(8)).Vertices.size();
    CHECK(fine > coarse);
}

TEST_CASE("Sphere: invariants, radius AABB, smooth normals")
{
    const f32 radius = 0.75f;
    const u32 rings = 16;
    const u32 segments = 24;
    const MeshData data = Primitives::Sphere(radius, rings, segments);

    CHECK(data.Vertices.size() == static_cast<usize>(rings + 1) * (segments + 1));
    CheckCommonInvariants(data);

    // Every vertex sits on the sphere surface; normal points outward.
    for (const CanonicalVertex& v : data.Vertices)
    {
        CHECK(glm::length(v.Position) == doctest::Approx(radius).epsilon(0.001f));
        const vec3 outward = glm::normalize(v.Position);
        CHECK(glm::dot(outward, v.Normal) == doctest::Approx(1.0f).epsilon(0.001f));
    }

    const Aabb box = ComputeAabb(data);
    CHECK(box.Min.x == doctest::Approx(-radius).epsilon(0.001f));
    CHECK(box.Max.x == doctest::Approx(+radius).epsilon(0.001f));
    CHECK(box.Min.y == doctest::Approx(-radius).epsilon(0.001f));
    CHECK(box.Max.y == doctest::Approx(+radius).epsilon(0.001f));
    CHECK(box.Min.z == doctest::Approx(-radius).epsilon(0.001f));
    CHECK(box.Max.z == doctest::Approx(+radius).epsilon(0.001f));
}

TEST_CASE("Sphere: rings/segments clamp to a minimum of 3")
{
    const MeshData clamped = Primitives::Sphere(0.5f, 0, 0);
    const MeshData explicitMin = Primitives::Sphere(0.5f, 3, 3);

    CHECK(clamped.Vertices.size() == explicitMin.Vertices.size());
    CHECK(clamped.Indices.size() == explicitMin.Indices.size());
}

TEST_CASE("Sphere: vertex count grows with rings and segments")
{
    const usize coarse = Primitives::Sphere(0.5f, 4, 6).Vertices.size();
    const usize moreRings = Primitives::Sphere(0.5f, 8, 6).Vertices.size();
    const usize moreSegments = Primitives::Sphere(0.5f, 4, 12).Vertices.size();

    CHECK(moreRings > coarse);
    CHECK(moreSegments > coarse);
}

TEST_CASE("Icosphere: invariants, radius AABB, outward normals")
{
    const f32 radius = 0.75f;
    const MeshData data = Primitives::Icosphere(radius, 2);

    CheckCommonInvariants(data);

    // Every vertex sits on the sphere surface; normal points outward.
    for (const CanonicalVertex& v : data.Vertices)
    {
        CHECK(glm::length(v.Position) == doctest::Approx(radius).epsilon(0.001f));
        const vec3 outward = glm::normalize(v.Position);
        CHECK(glm::dot(outward, v.Normal) == doctest::Approx(1.0f).epsilon(0.001f));
    }

    const Aabb box = ComputeAabb(data);
    CHECK(box.Min.x == doctest::Approx(-radius).epsilon(0.001f));
    CHECK(box.Max.x == doctest::Approx(+radius).epsilon(0.001f));
    CHECK(box.Min.y == doctest::Approx(-radius).epsilon(0.001f));
    CHECK(box.Max.y == doctest::Approx(+radius).epsilon(0.001f));
    CHECK(box.Min.z == doctest::Approx(-radius).epsilon(0.001f));
    CHECK(box.Max.z == doctest::Approx(+radius).epsilon(0.001f));
}

TEST_CASE("Icosphere: triangle count is 20 * 4^subdivisions")
{
    // Seam-splitting duplicates vertices, not triangles, so the index count is
    // exactly the subdivided-icosahedron face count regardless of the wrap fix.
    for (u32 subdivisions = 0; subdivisions <= 3; ++subdivisions)
    {
        const MeshData data = Primitives::Icosphere(0.5f, subdivisions);
        const usize triangles =
            static_cast<usize>(20) * (static_cast<usize>(1) << (2 * subdivisions));
        CHECK(data.Indices.size() == triangles * 3);
    }
}

TEST_CASE("Icosphere: vertex count grows with subdivisions")
{
    const usize base = Primitives::Icosphere(0.5f, 0).Vertices.size();
    const usize once = Primitives::Icosphere(0.5f, 1).Vertices.size();
    const usize twice = Primitives::Icosphere(0.5f, 2).Vertices.size();

    CHECK(once > base);
    CHECK(twice > once);
}

TEST_CASE("Cylinder: invariants, radius/height AABB")
{
    const f32 radius = 0.75f;
    const f32 height = 2.0f;
    const u32 segments = 24;
    const MeshData data = Primitives::Cylinder(radius, height, segments);

    CheckCommonInvariants(data);

    const Aabb box = ComputeAabb(data);
    CHECK(box.Min.x == doctest::Approx(-radius).epsilon(0.001f));
    CHECK(box.Max.x == doctest::Approx(+radius).epsilon(0.001f));
    CHECK(box.Min.z == doctest::Approx(-radius).epsilon(0.001f));
    CHECK(box.Max.z == doctest::Approx(+radius).epsilon(0.001f));
    CHECK(box.Min.y == doctest::Approx(-height * 0.5f).epsilon(0.001f));
    CHECK(box.Max.y == doctest::Approx(+height * 0.5f).epsilon(0.001f));
}

TEST_CASE("Cylinder: segments clamp to a minimum of 3")
{
    const MeshData clamped = Primitives::Cylinder(0.5f, 1.0f, 0);
    const MeshData explicitMin = Primitives::Cylinder(0.5f, 1.0f, 3);

    CHECK(clamped.Vertices.size() == explicitMin.Vertices.size());
    CHECK(clamped.Indices.size() == explicitMin.Indices.size());
}

TEST_CASE("Cylinder: vertex count grows with segments")
{
    const usize coarse = Primitives::Cylinder(0.5f, 1.0f, 6).Vertices.size();
    const usize fine = Primitives::Cylinder(0.5f, 1.0f, 16).Vertices.size();
    CHECK(fine > coarse);
}

TEST_CASE("Cone: invariants, radius/height AABB, apex on +Y")
{
    const f32 radius = 0.6f;
    const f32 height = 1.5f;
    const u32 segments = 24;
    const MeshData data = Primitives::Cone(radius, height, segments);

    CheckCommonInvariants(data);

    const Aabb box = ComputeAabb(data);
    CHECK(box.Min.x == doctest::Approx(-radius).epsilon(0.001f));
    CHECK(box.Max.x == doctest::Approx(+radius).epsilon(0.001f));
    CHECK(box.Min.z == doctest::Approx(-radius).epsilon(0.001f));
    CHECK(box.Max.z == doctest::Approx(+radius).epsilon(0.001f));
    CHECK(box.Min.y == doctest::Approx(-height * 0.5f).epsilon(0.001f));
    CHECK(box.Max.y == doctest::Approx(+height * 0.5f).epsilon(0.001f));
}

TEST_CASE("Cone: segments clamp to a minimum of 3")
{
    const MeshData clamped = Primitives::Cone(0.5f, 1.0f, 0);
    const MeshData explicitMin = Primitives::Cone(0.5f, 1.0f, 3);

    CHECK(clamped.Vertices.size() == explicitMin.Vertices.size());
    CHECK(clamped.Indices.size() == explicitMin.Indices.size());
}

TEST_CASE("Torus: invariants, outer/inner radius AABB")
{
    const f32 major = 0.8f;
    const f32 minor = 0.25f;
    const MeshData data = Primitives::Torus(major, minor, 24, 12);

    CheckCommonInvariants(data);

    // Every vertex is minor away from a point on the major circle in the XZ plane.
    for (const CanonicalVertex& v : data.Vertices)
    {
        const f32 r = std::sqrt(v.Position.x * v.Position.x + v.Position.z * v.Position.z);
        const f32 dx = r - major;
        const f32 dist = std::sqrt(dx * dx + v.Position.y * v.Position.y);
        CHECK(dist == doctest::Approx(minor).epsilon(0.01f));
    }

    const Aabb box = ComputeAabb(data);
    CHECK(box.Max.x == doctest::Approx(major + minor).epsilon(0.001f));
    CHECK(box.Min.x == doctest::Approx(-(major + minor)).epsilon(0.001f));
    CHECK(box.Max.y == doctest::Approx(minor).epsilon(0.001f));
    CHECK(box.Min.y == doctest::Approx(-minor).epsilon(0.001f));
}

TEST_CASE("Torus: segments clamp to a minimum of 3")
{
    const MeshData clamped = Primitives::Torus(0.5f, 0.2f, 0, 0);
    const MeshData explicitMin = Primitives::Torus(0.5f, 0.2f, 3, 3);

    CHECK(clamped.Vertices.size() == explicitMin.Vertices.size());
    CHECK(clamped.Indices.size() == explicitMin.Indices.size());
}

TEST_CASE("Torus: vertex count grows with segments")
{
    const usize coarse = Primitives::Torus(0.5f, 0.2f, 8, 6).Vertices.size();
    const usize moreMajor = Primitives::Torus(0.5f, 0.2f, 16, 6).Vertices.size();
    const usize moreMinor = Primitives::Torus(0.5f, 0.2f, 8, 12).Vertices.size();
    CHECK(moreMajor > coarse);
    CHECK(moreMinor > coarse);
}

TEST_CASE("Annulus: counts, planarity, radial bounds, radial UVs and +Y winding")
{
    const f32 inner = 0.4f;
    const f32 outer = 1.0f;
    const u32 radial = 3;
    const u32 angular = 24;
    const MeshData data = Primitives::Annulus(inner, outer, radial, angular);

    CHECK(data.Vertices.size() == static_cast<usize>(angular + 1) * (radial + 1));
    CHECK(data.Indices.size() == static_cast<usize>(angular) * radial * 6);
    CheckCommonInvariants(data);

    usize innerEdge = 0;
    usize outerEdge = 0;
    f32 minAngularUv = 1.0f;
    f32 maxAngularUv = 0.0f;

    for (const CanonicalVertex& v : data.Vertices)
    {
        // Flat in XZ at Y = 0, +Y normal.
        CHECK(v.Position.y == doctest::Approx(0.0f));
        CHECK(v.Normal.x == doctest::Approx(0.0f));
        CHECK(v.Normal.y == doctest::Approx(1.0f));
        CHECK(v.Normal.z == doctest::Approx(0.0f));

        const f32 r = RingRadius(v.Position);
        CHECK(r >= inner - 1e-4f);
        CHECK(r <= outer + 1e-4f);

        // u is the normalized radial fraction, so it recovers the vertex's own radius.
        CHECK(inner + (outer - inner) * v.UV.x == doctest::Approx(r).epsilon(0.001f));
        CHECK(v.UV.y >= 0.0f);
        CHECK(v.UV.y <= 1.0f);

        // The tangent follows +u, pointing radially outward.
        const vec3 outward = glm::normalize(vec3(v.Position.x, 0.0f, v.Position.z));
        CHECK(glm::dot(outward, vec3(v.Tangent)) == doctest::Approx(1.0f).epsilon(0.001f));

        if (v.UV.x == 0.0f)
        {
            ++innerEdge;
            CHECK(r == doctest::Approx(inner));
        }
        if (v.UV.x == 1.0f)
        {
            ++outerEdge;
            CHECK(r == doctest::Approx(outer));
        }

        minAngularUv = std::min(minAngularUv, v.UV.y);
        maxAngularUv = std::max(maxAngularUv, v.UV.y);
    }

    // One vertex per angular index sits on each edge ring, exactly at that radius.
    CHECK(innerEdge == static_cast<usize>(angular) + 1);
    CHECK(outerEdge == static_cast<usize>(angular) + 1);

    // The seam column duplicates, so v spans the full [0,1] instead of wrapping back to 0.
    CHECK(minAngularUv == doctest::Approx(0.0f));
    CHECK(maxAngularUv == doctest::Approx(1.0f));

    // No triangle is inverted — the failure a radial-then-angular index order produces on one of
    // the two triangles per quad, which a per-vertex normal check cannot see.
    for (usize triangle = 0; triangle < data.Indices.size() / 3; ++triangle)
    {
        const vec3 geometric = TriangleCross(data, triangle);
        REQUIRE(glm::length(geometric) > 1e-9f);
        CHECK(glm::normalize(geometric).y == doctest::Approx(1.0f).epsilon(0.001f));
    }
}

TEST_CASE("Annulus: segment counts clamp to their minimums")
{
    CheckIdenticalGeometry(Primitives::Annulus(0.2f, 0.5f, 0, 0, 0),
                           Primitives::Annulus(0.2f, 0.5f, 1, 3, 1));
}

TEST_CASE("Annulus: vertex count grows with segments")
{
    const usize coarse = Primitives::Annulus(0.2f, 0.5f, 1, 8).Vertices.size();
    const usize moreRadial = Primitives::Annulus(0.2f, 0.5f, 4, 8).Vertices.size();
    const usize moreAngular = Primitives::Annulus(0.2f, 0.5f, 1, 16).Vertices.size();

    CHECK(moreRadial > coarse);
    CHECK(moreAngular > coarse);
}

TEST_CASE("Annulus: an inner radius past the outer is swapped, not inverted")
{
    CheckIdenticalGeometry(Primitives::Annulus(1.0f, 0.4f, 2, 16),
                           Primitives::Annulus(0.4f, 1.0f, 2, 16));
}

TEST_CASE("Annulus: one angular submesh matches omitting the parameter")
{
    CheckIdenticalGeometry(Primitives::Annulus(0.4f, 1.0f, 2, 16, 1),
                           Primitives::Annulus(0.4f, 1.0f, 2, 16));
}

TEST_CASE("Annulus: angular submeshes partition the index buffer into equal sectors")
{
    const f32 inner = 0.4f;
    const f32 outer = 1.0f;
    const u32 radial = 2;
    const u32 angular = 24;
    const u32 sectors = 4;

    const MeshData split = Primitives::Annulus(inner, outer, radial, angular, sectors);
    const MeshData whole = Primitives::Annulus(inner, outer, radial, angular);

    // Contiguous, non-overlapping, covering the whole range.
    CheckCommonInvariants(split, sectors);

    // The sectors partition the index buffer, not the vertices: one grid, the same indices in the
    // same order, only the submesh table differing.
    CHECK(split.Vertices.size() == whole.Vertices.size());
    CHECK(split.Indices == whole.Indices);

    const u32 perSector = static_cast<u32>(whole.Indices.size()) / sectors;
    for (const SubMesh& submesh : split.SubMeshes)
    {
        CHECK(submesh.IndexCount == perSector);
    }

    // Each sector spans exactly its own 1/N of the turn.
    for (u32 sector = 0; sector < sectors; ++sector)
    {
        const SubMesh& submesh = split.SubMeshes[sector];
        f32 minAngularUv = 2.0f;
        f32 maxAngularUv = -1.0f;
        for (u32 k = 0; k < submesh.IndexCount; ++k)
        {
            const f32 v = split.Vertices[split.Indices[submesh.IndexOffset + k]].UV.y;
            minAngularUv = std::min(minAngularUv, v);
            maxAngularUv = std::max(maxAngularUv, v);
        }

        CHECK(minAngularUv ==
              doctest::Approx(static_cast<f32>(sector) / static_cast<f32>(sectors)));
        CHECK(maxAngularUv ==
              doctest::Approx(static_cast<f32>(sector + 1) / static_cast<f32>(sectors)));
    }
}

TEST_CASE("Annulus: angular segments round up to a multiple of the sector count")
{
    // 10 columns over 4 sectors rounds to 12, so no sector is short and no quad straddles a
    // boundary.
    const MeshData data = Primitives::Annulus(0.4f, 1.0f, 1, 10, 4);

    CHECK(data.Vertices.size() == static_cast<usize>(12 + 1) * 2);
    CHECK(data.Indices.size() == static_cast<usize>(12) * 6);
    CheckCommonInvariants(data, 4);

    for (const SubMesh& submesh : data.SubMeshes)
    {
        CHECK(submesh.IndexCount == 18);
    }
}

TEST_CASE("Annulus: a zero inner radius is a filled disc")
{
    const u32 radial = 2;
    const u32 angular = 16;
    const MeshData data = Primitives::Annulus(0.0f, 0.75f, radial, angular);

    CheckCommonInvariants(data);

    // The inner edge ring collapses onto the origin, so there is no hole.
    usize atOrigin = 0;
    for (const CanonicalVertex& v : data.Vertices)
    {
        if (RingRadius(v.Position) < 1e-6f)
        {
            ++atOrigin;
        }
    }
    CHECK(atOrigin == static_cast<usize>(angular) + 1);

    // The only degenerate triangles are the centre fan's: one per quad in the innermost band,
    // whose two inner corners are both the origin. Nothing outside that band degenerates.
    usize degenerate = 0;
    for (usize triangle = 0; triangle < data.Indices.size() / 3; ++triangle)
    {
        if (glm::length(TriangleCross(data, triangle)) <= 1e-9f)
        {
            ++degenerate;
        }
    }
    CHECK(degenerate == static_cast<usize>(angular));
}

TEST_CASE("Annulus: summed triangle area converges to the analytic ring area")
{
    constexpr f32 Pi = 3.14159265358979323846f;
    const f32 inner = 0.3f;
    const f32 outer = 0.9f;
    const f64 exact =
        static_cast<f64>(Pi) * (static_cast<f64>(outer) * outer - static_cast<f64>(inner) * inner);

    // A wrong radius interpolation keeps every vertex inside the radial bounds, so the per-vertex
    // checks pass and only the enclosed area moves.
    f64 previousError = exact;
    for (const u32 angular : {8u, 32u, 128u, 512u})
    {
        const MeshData data = Primitives::Annulus(inner, outer, 2, angular);

        f64 area = 0.0;
        for (usize triangle = 0; triangle < data.Indices.size() / 3; ++triangle)
        {
            area += 0.5 * static_cast<f64>(glm::length(TriangleCross(data, triangle)));
        }

        const f64 error = std::abs(area - exact);
        CHECK(error < previousError);
        previousError = error;
    }

    // The chords of a 512-column ring are indistinguishable from the ideal arcs at this scale.
    CHECK(previousError < exact * 1e-4);
}

TEST_CASE("Capsule: invariants, radius/full-extent AABB, surface distance")
{
    const f32 radius = 0.4f;
    const f32 height = 1.0f;
    const MeshData data = Primitives::Capsule(radius, height, 24, 6);

    CheckCommonInvariants(data);

    const f32 halfH = height * 0.5f;

    // Every vertex is `radius` from the nearer hemisphere center on the Y axis.
    for (const CanonicalVertex& v : data.Vertices)
    {
        const f32 centerY = v.Position.y >= 0.0f ? +halfH : -halfH;
        const vec3 toCenter = v.Position - vec3(0.0f, centerY, 0.0f);
        CHECK(glm::length(toCenter) == doctest::Approx(radius).epsilon(0.01f));
    }

    const Aabb box = ComputeAabb(data);
    CHECK(box.Max.x == doctest::Approx(radius).epsilon(0.001f));
    CHECK(box.Min.x == doctest::Approx(-radius).epsilon(0.001f));
    CHECK(box.Max.y == doctest::Approx(halfH + radius).epsilon(0.001f));
    CHECK(box.Min.y == doctest::Approx(-(halfH + radius)).epsilon(0.001f));
}

TEST_CASE("Capsule: segments and rings clamp to minimums")
{
    const MeshData clamped = Primitives::Capsule(0.5f, 1.0f, 0, 0);
    const MeshData explicitMin = Primitives::Capsule(0.5f, 1.0f, 3, 1);

    CHECK(clamped.Vertices.size() == explicitMin.Vertices.size());
    CHECK(clamped.Indices.size() == explicitMin.Indices.size());
}

TEST_CASE("Primitives: a generator records a material handle that is not yet resident")
{
    // The asynchronous Build returns a handle naming a real asset that becomes resident a frame or
    // more later. A generator runs immediately after, so it sees the pending handle — and recording
    // it by residency would drop it and bake NoMaterial into the submesh, which the draw gather
    // skips forever. The material landing afterwards cannot undo that, so the mesh would never draw.
    Renderer::Context context;
    TaskSystem tasks;
    TypeRegistry types;
    AssetManager manager(context, tasks, types);

    // A factory that is never pumped, so the handle stays pending for the whole case.
    const AssetHandle<MaterialInstance> pending =
        manager.Adopt<MaterialInstance>(tasks.Submit([] { return Ref<MaterialInstance>(); }));

    REQUIRE_FALSE(pending.IsLoaded());
    REQUIRE(pending.IsValid());

    const MeshData data = Primitives::Icosphere(1.0f, 1, pending);

    REQUIRE(data.Materials.size() == 1);
    REQUIRE_FALSE(data.SubMeshes.empty());
    for (const SubMesh& submesh : data.SubMeshes)
    {
        CHECK(submesh.MaterialIndex != SubMesh::NoMaterial);
        CHECK(submesh.MaterialIndex < data.Materials.size());
    }

    // An empty handle still means "no material" — the distinction the fix turns on.
    const MeshData bare = Primitives::Icosphere(1.0f, 1, AssetHandle<MaterialInstance>{});
    CHECK(bare.Materials.empty());
    for (const SubMesh& submesh : bare.SubMeshes)
    {
        CHECK(submesh.MaterialIndex == SubMesh::NoMaterial);
    }

    tasks.WaitForAll();
}
