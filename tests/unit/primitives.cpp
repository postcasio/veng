// Primitive-generator unit cases: pure CPU geometry, no Context, no Vulkan.
// These pin the math — counts, triangle winding, unit-length normals/tangents,
// AABB extents, and the default no-material wiring. The populated-material path
// needs a GPU AssetHandle<Material>, so it is exercised elsewhere; here every
// generator is called with the empty default handle.

#include <doctest/doctest.h>

#include <cmath>

#include <glm/geometric.hpp>

#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Primitives.h>

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

    // Shared invariants every primitive's MeshData must satisfy.
    void CheckCommonInvariants(const MeshData& data)
    {
        // Triangle list.
        CHECK(data.Indices.size() % 3 == 0);
        CHECK_FALSE(data.Indices.empty());

        // Every index is in bounds.
        for (const u32 index : data.Indices)
        {
            CHECK(index < data.Vertices.size());
        }

        // Exactly one submesh covering the whole index range.
        REQUIRE(data.SubMeshes.size() == 1);
        CHECK(data.SubMeshes[0].IndexOffset == 0);
        CHECK(data.SubMeshes[0].IndexCount == data.Indices.size());

        // No-material default: empty list, unassigned submesh.
        CHECK(data.Materials.empty());
        CHECK(data.SubMeshes[0].MaterialIndex == SubMesh::NoMaterial);

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
