// Async mesh build: a hand-built MeshData is uploaded through Mesh::Build,
// proving the worker job builds a drawable Ref<Mesh> off the render thread (the
// geometry copy is a host-visible memcpy, no transfer queue) and that the async
// path produces a mesh identical in index count, submesh table, and bounds to the
// blocking Mesh::Create sibling — differing only in *when*, not *what*.

#include <doctest/doctest.h>

#include <Veng/Asset/Material.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Task/TaskSystem.h>

#include <gpu/fixture.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    // A unit quad in the XY plane: four corners, two triangles, one submesh — the
    // smallest geometry that exercises the vertex + index upload and a non-empty bound.
    MeshData TwoTriangleQuad()
    {
        const auto vertex = [](vec3 position, vec2 uv)
        {
            return CanonicalVertex{
                .Position = position,
                .Normal = vec3(0.0f, 0.0f, 1.0f),
                .Tangent = vec4(1.0f, 0.0f, 0.0f, 1.0f),
                .UV = uv,
            };
        };

        MeshData data;
        data.Vertices = {
            vertex(vec3(-0.5f, -0.5f, 0.0f), vec2(0.0f, 0.0f)),
            vertex(vec3(0.5f, -0.5f, 0.0f), vec2(1.0f, 0.0f)),
            vertex(vec3(0.5f, 0.5f, 0.0f), vec2(1.0f, 1.0f)),
            vertex(vec3(-0.5f, 0.5f, 0.0f), vec2(0.0f, 1.0f)),
        };
        data.Indices = {0, 1, 2, 0, 2, 3};
        return data;
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "Mesh::Build: a worker builds a drawable mesh with correct bounds")
{
    const MeshData data = TwoTriangleQuad();

    Task<Ref<Mesh>> factory = Mesh::Build(Context, Tasks, data, "Async Quad");

    const Result<Ref<Mesh>> built = factory.Get();
    REQUIRE(built.has_value());

    const Ref<Mesh> mesh = *built;
    REQUIRE(mesh != nullptr);

    CHECK(mesh->GetIndexType() == IndexType::U32);
    CHECK(mesh->GetIndexCount() == 6);

    // Empty source submesh table → one synthesized unassigned submesh over the whole range.
    const std::span<const SubMesh> subMeshes = mesh->GetSubMeshes();
    REQUIRE(subMeshes.size() == 1);
    CHECK(subMeshes[0].IndexOffset == 0);
    CHECK(subMeshes[0].IndexCount == 6);
    CHECK(subMeshes[0].MaterialIndex == SubMesh::NoMaterial);

    // Canonical layout, carried materials (none here), and GPU buffers sized to the geometry.
    const VertexBufferLayout canonical = Mesh::CanonicalLayout();
    CHECK(mesh->GetLayout().GetStride() == canonical.GetStride());
    CHECK(mesh->GetMaterials().empty());
    REQUIRE(mesh->GetVertexBuffer() != nullptr);
    CHECK(mesh->GetVertexBuffer()->GetSize() == 4 * sizeof(CanonicalVertex));
    CHECK(mesh->GetIndexBuffer().GetBuffer()->GetSize() == 6 * sizeof(u32));

    // The worker folded ComputeBounds over the CPU geometry, so the streamed mesh is
    // broadphase-correct the instant it goes resident.
    CHECK_FALSE(mesh->GetBounds().IsEmpty());

    // Drain the worker before the fixture tears the Context down: a live worker must
    // not outlive the device.
    Tasks.WaitForAll();
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "Mesh::Build matches the blocking BuildSync (index count, submeshes, bounds)")
{
    const MeshData data = TwoTriangleQuad();

    const Ref<Mesh> sync = Mesh::BuildSync(Context, data, "Sync Quad");
    REQUIRE(sync != nullptr);

    Task<Ref<Mesh>> factory = Mesh::Build(Context, Tasks, data, "Async Quad");
    const Result<Ref<Mesh>> built = factory.Get();
    REQUIRE(built.has_value());
    const Ref<Mesh> async = *built;
    REQUIRE(async != nullptr);

    CHECK(async->GetIndexCount() == sync->GetIndexCount());

    REQUIRE(async->GetSubMeshes().size() == sync->GetSubMeshes().size());
    for (usize i = 0; i < sync->GetSubMeshes().size(); ++i)
    {
        CHECK(async->GetSubMeshes()[i].IndexOffset == sync->GetSubMeshes()[i].IndexOffset);
        CHECK(async->GetSubMeshes()[i].IndexCount == sync->GetSubMeshes()[i].IndexCount);
        CHECK(async->GetSubMeshes()[i].MaterialIndex == sync->GetSubMeshes()[i].MaterialIndex);
        CHECK(async->GetSubMeshes()[i].Bounds.Min == sync->GetSubMeshes()[i].Bounds.Min);
        CHECK(async->GetSubMeshes()[i].Bounds.Max == sync->GetSubMeshes()[i].Bounds.Max);
    }

    CHECK(async->GetBounds().Min == sync->GetBounds().Min);
    CHECK(async->GetBounds().Max == sync->GetBounds().Max);

    Tasks.WaitForAll();
}
