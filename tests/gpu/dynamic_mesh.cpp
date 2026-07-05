// DynamicMesh: async geometry rebuilds behind a stable front handle. Proves the front
// handle stays empty until the first build lands, survives (keeps the old geometry) while
// a rebuild is in flight, swaps only on Update() after the build completes, and that
// rebuilds queued while one is in flight coalesce to the newest geometry (latest wins —
// an intermediate rebuild is never built).

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/DynamicMesh.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Task/TaskSystem.h>

#include <gpu/fixture.h>

using namespace Veng;

namespace
{
    // Drains the worker pool and pumps the main-thread continuation so a pending Build handle
    // finalizes into residency.
    void PumpToResidency(TaskSystem& tasks)
    {
        tasks.WaitForAll();
        tasks.PumpMainThread();
    }

    // Flat XY geometry of `triangles` triangles — distinct index counts distinguish which
    // rebuild's geometry the front handle holds.
    MeshData TriangleFan(u32 triangles)
    {
        MeshData data;
        data.Vertices.push_back(CanonicalVertex{
            .Position = vec3(0.0f),
            .Normal = vec3(0.0f, 0.0f, 1.0f),
            .Tangent = vec4(1.0f, 0.0f, 0.0f, 1.0f),
            .UV = vec2(0.0f),
        });
        for (u32 i = 0; i <= triangles; ++i)
        {
            const f32 angle = static_cast<f32>(i) * 0.5f;
            data.Vertices.push_back(CanonicalVertex{
                .Position = vec3(std::cos(angle), std::sin(angle), 0.0f),
                .Normal = vec3(0.0f, 0.0f, 1.0f),
                .Tangent = vec4(1.0f, 0.0f, 0.0f, 1.0f),
                .UV = vec2(1.0f, 0.0f),
            });
        }
        for (u32 i = 0; i < triangles; ++i)
        {
            data.Indices.insert(data.Indices.end(), {0u, i + 1, i + 2});
        }
        return data;
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "DynamicMesh: the front handle is empty until the first build lands")
{
    AssetManager assets(Context, Tasks, Types);
    DynamicMesh dynamic(assets, "Dynamic Fan");

    CHECK_FALSE(dynamic.GetMesh().IsLoaded());
    CHECK_FALSE(dynamic.IsRebuildPending());

    dynamic.Rebuild(TriangleFan(1));
    CHECK(dynamic.IsRebuildPending());
    CHECK_FALSE(dynamic.GetMesh().IsLoaded());

    // Before the worker lands there is nothing to promote.
    CHECK_FALSE(dynamic.Update());

    PumpToResidency(Tasks);
    CHECK(dynamic.Update());
    REQUIRE(dynamic.GetMesh().IsLoaded());
    CHECK(dynamic.GetMesh()->GetIndexCount() == 3);
    CHECK_FALSE(dynamic.IsRebuildPending());

    dynamic.Reset();
    CHECK_FALSE(dynamic.GetMesh().IsLoaded());
    CHECK_FALSE(dynamic.IsRebuildPending());
}

TEST_CASE_FIXTURE(
    Veng::Test::GpuFixture,
    "DynamicMesh: the front survives an in-flight rebuild and queued rebuilds coalesce")
{
    AssetManager assets(Context, Tasks, Types);
    DynamicMesh dynamic(assets, "Dynamic Fan");

    dynamic.Rebuild(TriangleFan(2)); // starts immediately
    dynamic.Rebuild(TriangleFan(3)); // queued behind the in-flight build
    dynamic.Rebuild(TriangleFan(4)); // replaces the queued geometry — 3 is never built

    PumpToResidency(Tasks);

    // The first build promotes and the newest queued geometry starts; the front holds the
    // completed build while the next streams in.
    CHECK(dynamic.Update());
    REQUIRE(dynamic.GetMesh().IsLoaded());
    CHECK(dynamic.GetMesh()->GetIndexCount() == 2 * 3);
    CHECK(dynamic.IsRebuildPending());

    PumpToResidency(Tasks);
    CHECK(dynamic.Update());
    REQUIRE(dynamic.GetMesh().IsLoaded());
    CHECK(dynamic.GetMesh()->GetIndexCount() == 4 * 3);
    CHECK_FALSE(dynamic.IsRebuildPending());
}
