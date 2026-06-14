// Primitive-generator unit cases: pure CPU geometry, no Context, no Vulkan.
// These pin the math — counts, winding-agnostic invariants, unit-length
// normals/tangents, AABB extents, and the default no-material wiring. The
// populated-material path needs a GPU AssetHandle<Material>, so it is exercised
// elsewhere; here every generator is called with the empty default handle.

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
        Aabb box{data.Vertices[0].Position, data.Vertices[0].Position};
        for (const CanonicalVertex& v : data.Vertices)
        {
            box.Min = glm::min(box.Min, v.Position);
            box.Max = glm::max(box.Max, v.Position);
        }
        return box;
    }

    // Shared invariants every primitive's MeshData must satisfy.
    void CheckCommonInvariants(const MeshData& data)
    {
        // Triangle list.
        CHECK(data.Indices.size() % 3 == 0);
        CHECK_FALSE(data.Indices.empty());

        // Every index is in bounds.
        for (u32 index : data.Indices)
            CHECK(index < data.Vertices.size());

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
            CHECK(std::abs(glm::dot(tangentXyz, v.Normal)) == doctest::Approx(0.0f).epsilon(0.001f));
            CHECK(std::abs(v.Tangent.w) == doctest::Approx(1.0f));
        }
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
