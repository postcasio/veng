// Adopt test: builds a runtime primitive Mesh (no cooker, no AssetId) and adopts
// it into an AssetHandle<Mesh> through AssetManager::Adopt, proving a runtime
// resource is a first-class handle — resident, drawable, id-less, and immune to
// CollectGarbage (its cache entry is detached, kept alive only by its handles).

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Primitives.h>
#include <Veng/Renderer/Buffer.h>

#include <gpu/fixture.h>

using namespace Veng;

TEST_CASE_FIXTURE(Veng::Test::GpuFixture, "AssetManager::Adopt: runtime primitive mesh becomes a first-class, id-less handle")
{
    AssetManager assets(Context, Tasks, Types);

    // A runtime primitive — built and uploaded with no cooker, no AssetId, no
    // material (the empty handle leaves the submesh unassigned).
    const Ref<Mesh> mesh = Mesh::Create(Context, Primitives::Cube(1.0f), "Adopted Cube");
    REQUIRE(mesh != nullptr);

    const AssetHandle<Mesh> handle = assets.Adopt(mesh);

    // Immediately resident, points at the same Mesh, and carries the invalid
    // AssetId — a runtime resource has no content identity.
    CHECK(handle.IsLoaded());
    CHECK(handle.Get() == mesh.get());
    CHECK_FALSE(handle.Id().IsValid());

    // Usable exactly like a cooked handle: the geometry is reachable through it.
    REQUIRE(handle->GetVertexBuffer() != nullptr);
    CHECK(handle->GetIndexCount() == 36);

    // The detached entry is not in the AssetId cache, so CollectGarbage never
    // evicts it; the handle keeps the mesh resident on its own.
    assets.CollectGarbage();
    CHECK(handle.IsLoaded());
    CHECK(handle.Get() == mesh.get());

    // Adopting does not deduplicate — each call yields a distinct entry over the
    // same underlying resource.
    const AssetHandle<Mesh> second = assets.Adopt(mesh);
    CHECK(second.Get() == mesh.get());
}
