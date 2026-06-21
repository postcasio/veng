// Adopt(Task) smoke: a runtime Mesh streams in through AssetManager::Adopt,
// proving the detached-but-pending cache-entry lifecycle end to end — the handle
// is returned not-yet-resident, the factory's Ref<Mesh> lands through the
// main-thread continuation pump, and IsLoaded() then flips to a drawable mesh.

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Primitives.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Task/TaskSystem.h>

#include <gpu/fixture.h>

using namespace Veng;

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "AssetManager::Adopt: a runtime mesh streams in as a pending handle")
{
    AssetManager assets(Context, Tasks, Types);

    // The geometry build + GPU upload run on the render thread; the factory task
    // hands the finished Ref<Mesh> to Adopt's main-thread continuation.
    const Ref<Mesh> mesh = Mesh::BuildSync(Context, Primitives::Cube(1.0f), "Async Cube");
    REQUIRE(mesh != nullptr);

    Task<Ref<Mesh>> factory = Tasks.Submit([mesh] { return mesh; });

    const AssetHandle<Mesh> handle = assets.Adopt<Mesh>(std::move(factory));

    // Pending: the handle exists but is not yet resident, and carries the invalid id.
    CHECK_FALSE(handle.IsLoaded());
    CHECK(handle.Get() == nullptr);
    CHECK_FALSE(handle.Id().IsValid());

    // Drain the worker and pump the continuation so the entry finalizes.
    Tasks.WaitForAll();
    Tasks.PumpMainThread();

    // Resident: the same handle now resolves to the drawable mesh.
    CHECK(handle.IsLoaded());
    REQUIRE(handle.Get() == mesh.get());
    REQUIRE(handle->GetVertexBuffer() != nullptr);
    CHECK(handle->GetIndexCount() == 36);

    // Detached like Adopt: the invalid id is never cached, so CollectGarbage never
    // evicts it while the handle holds it.
    CHECK(assets.CachedEntry(AssetId{}) == nullptr);
    assets.CollectGarbage();
    CHECK(handle.IsLoaded());
}
