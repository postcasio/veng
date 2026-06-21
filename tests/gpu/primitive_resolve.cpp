// ResolvePrimitiveMeshes end to end: a PrimitiveComponent's active shape is generated
// into a streamed Mesh through CreateAsync and stored in the entity's MeshRenderer.
// Covers residency after pumping, dedup of identical shapes through the
// PrimitiveMeshCache, idempotence of a second scan, and re-resolution + prune after
// an entity's shape changes.

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Primitives.h>
#include <Veng/Math/AABB.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/PrimitiveResolve.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Task/TaskSystem.h>

#include <gpu/fixture.h>

using namespace Veng;

namespace
{
    // Pumps the worker drain + main-thread continuation + finalize so pending
    // CreateAsync handles flip resident.
    void PumpUntilResident(TaskSystem& tasks, AssetManager& manager)
    {
        tasks.WaitForAll();
        tasks.PumpMainThread();
        manager.PumpFinalizes();
    }

    Entity SpawnPrimitive(Scene& scene, const PrimitiveShapeVariant& shape)
    {
        const Entity entity = scene.CreateEntity();
        scene.Add<Transform>(entity);
        scene.Add<PrimitiveComponent>(entity).Shape = shape;
        return entity;
    }

    PrimitiveShapeVariant CubeVariant(f32 extent)
    {
        PrimitiveShapeVariant variant;
        static_cast<CubeShape*>(variant.SetActive(TypeIdOf<CubeShape>()))->Extent = extent;
        return variant;
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture, "ResolvePrimitiveMeshes streams in a primitive mesh")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    Unique<Scene> scene = Scene::Create(Types);

    const Entity entity = SpawnPrimitive(*scene, CubeVariant(1.0f));

    PrimitiveMeshCache cache;
    ResolvePrimitiveMeshes(*scene, assets, cache);

    // The MeshRenderer was added and holds a pending (not-yet-resident) handle.
    const MeshRenderer* renderer = scene->TryGet<MeshRenderer>(entity);
    REQUIRE(renderer != nullptr);
    CHECK_FALSE(renderer->Mesh.IsLoaded());

    PumpUntilResident(Tasks, assets);

    REQUIRE(scene->Get<MeshRenderer>(entity).Mesh.IsLoaded());
    CHECK(scene->Get<MeshRenderer>(entity).Mesh->GetIndexCount() ==
          Primitives::Cube(1.0f).Indices.size());
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "Identical shapes dedup to one mesh; a re-scan is a no-op")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    Unique<Scene> scene = Scene::Create(Types);

    const Entity a = SpawnPrimitive(*scene, CubeVariant(1.0f));
    const Entity b = SpawnPrimitive(*scene, CubeVariant(1.0f));

    PrimitiveMeshCache cache;
    ResolvePrimitiveMeshes(*scene, assets, cache);

    // One cache entry feeds both entities (dedup).
    CHECK(cache.Entries.size() == 1);

    PumpUntilResident(Tasks, assets);

    const MeshRenderer& ra = scene->Get<MeshRenderer>(a);
    const MeshRenderer& rb = scene->Get<MeshRenderer>(b);
    REQUIRE(ra.Mesh.IsLoaded());
    REQUIRE(rb.Mesh.IsLoaded());
    // Both entities share one underlying Ref<Mesh>.
    CHECK(ra.Mesh.Get() == rb.Mesh.Get());

    // Idempotence: a second scan creates nothing new and leaves the handles alone.
    const Mesh* before = ra.Mesh.Get();
    ResolvePrimitiveMeshes(*scene, assets, cache);
    CHECK(cache.Entries.size() == 1);
    CHECK(scene->Get<MeshRenderer>(a).Mesh.Get() == before);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "Changing a shape re-resolves the handle and prunes the orphaned key")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    Unique<Scene> scene = Scene::Create(Types);

    const Entity entity = SpawnPrimitive(*scene, CubeVariant(1.0f));

    PrimitiveMeshCache cache;
    ResolvePrimitiveMeshes(*scene, assets, cache);
    PumpUntilResident(Tasks, assets);

    // Hold the first mesh so dropping the cache entry does not free it (and the
    // allocator cannot reuse its address for the second mesh).
    const AssetHandle<Mesh> firstHandle = scene->Get<MeshRenderer>(entity).Mesh;
    REQUIRE(firstHandle.IsLoaded());
    const AABB firstBounds = firstHandle->GetBounds();

    // Edit the shape: a different extent yields a different ShapeKey.
    scene->Get<PrimitiveComponent>(entity).Shape = CubeVariant(2.0f);
    ResolvePrimitiveMeshes(*scene, assets, cache);

    // The orphaned 1.0f key was pruned; only the new key remains.
    CHECK(cache.Entries.size() == 1);

    PumpUntilResident(Tasks, assets);
    const AssetHandle<Mesh> secondHandle = scene->Get<MeshRenderer>(entity).Mesh;
    REQUIRE(secondHandle.IsLoaded());

    // The handle swapped to the new shape's mesh: a 2.0 cube is larger than a 1.0 cube.
    CHECK(secondHandle.Get() != firstHandle.Get());
    CHECK(secondHandle->GetBounds().Max.x > firstBounds.Max.x);
}
