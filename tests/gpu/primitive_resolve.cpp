// The Primitive resolver end to end: ResolveComponents builds the active
// shape into a streamed Mesh through CreatePrimitiveMesh and stores it in the entity's
// MeshRenderer. Covers residency after pumping, that identical shapes resolve to
// distinct meshes (no dedup), re-resolution swapping the handle, and an empty variant
// leaving the renderer untouched.

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Primitives.h>
#include <Veng/Math/AABB.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Resolve.h>
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
        scene.Add<Primitive>(entity).Shape = shape;
        return entity;
    }

    PrimitiveShapeVariant CubeVariant(f32 extent)
    {
        PrimitiveShapeVariant variant;
        static_cast<CubeShape*>(variant.SetActive(TypeIdOf<CubeShape>()))->Extent = extent;
        return variant;
    }

    PrimitiveShapeVariant CylinderVariant(f32 radius, f32 height, u32 segments)
    {
        PrimitiveShapeVariant variant;
        auto* shape = static_cast<CylinderShape*>(variant.SetActive(TypeIdOf<CylinderShape>()));
        shape->Radius = radius;
        shape->Height = height;
        shape->Segments = segments;
        return variant;
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture, "ResolveComponents streams in a primitive mesh")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    Unique<Scene> scene = Scene::Create(Types);

    const Entity entity = SpawnPrimitive(*scene, CubeVariant(1.0f));

    ResolveComponents(*scene, entity, assets);

    // The MeshRenderer was added and holds a pending (not-yet-resident) handle.
    const MeshRenderer* renderer = scene->TryGet<MeshRenderer>(entity);
    REQUIRE(renderer != nullptr);
    CHECK_FALSE(renderer->Mesh.IsLoaded());

    PumpUntilResident(Tasks, assets);

    REQUIRE(scene->Get<MeshRenderer>(entity).Mesh.IsLoaded());
    CHECK(scene->Get<MeshRenderer>(entity).Mesh->GetIndexCount() ==
          Primitives::Cube(1.0f).Indices.size());
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture, "ResolveComponents streams in a new-shape primitive mesh")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    Unique<Scene> scene = Scene::Create(Types);

    const Entity entity = SpawnPrimitive(*scene, CylinderVariant(0.5f, 1.0f, 24));

    ResolveComponents(*scene, entity, assets);
    PumpUntilResident(Tasks, assets);

    // A cylinder rides the unchanged resolve path; its mesh streams to residency
    // with the generator's index count.
    REQUIRE(scene->Get<MeshRenderer>(entity).Mesh.IsLoaded());
    CHECK(scene->Get<MeshRenderer>(entity).Mesh->GetIndexCount() ==
          Primitives::Cylinder(0.5f, 1.0f, 24).Indices.size());
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture, "Identical shapes resolve to distinct meshes")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    Unique<Scene> scene = Scene::Create(Types);

    const Entity a = SpawnPrimitive(*scene, CubeVariant(1.0f));
    const Entity b = SpawnPrimitive(*scene, CubeVariant(1.0f));

    ResolveComponents(*scene, a, assets);
    ResolveComponents(*scene, b, assets);
    PumpUntilResident(Tasks, assets);

    const MeshRenderer& ra = scene->Get<MeshRenderer>(a);
    const MeshRenderer& rb = scene->Get<MeshRenderer>(b);
    REQUIRE(ra.Mesh.IsLoaded());
    REQUIRE(rb.Mesh.IsLoaded());
    // No dedup cache: each entity builds its own mesh.
    CHECK(ra.Mesh.Get() != rb.Mesh.Get());
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture, "Re-resolving a changed shape swaps the handle")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    Unique<Scene> scene = Scene::Create(Types);

    const Entity entity = SpawnPrimitive(*scene, CubeVariant(1.0f));

    ResolveComponents(*scene, entity, assets);
    PumpUntilResident(Tasks, assets);

    // Hold the first mesh so it does not free (and the allocator cannot reuse its
    // address for the second mesh).
    const AssetHandle<Mesh> firstHandle = scene->Get<MeshRenderer>(entity).Mesh;
    REQUIRE(firstHandle.IsLoaded());
    const AABB firstBounds = firstHandle->GetBounds();

    scene->Get<Primitive>(entity).Shape = CubeVariant(2.0f);
    ResolveComponents(*scene, entity, assets);
    PumpUntilResident(Tasks, assets);

    const AssetHandle<Mesh> secondHandle = scene->Get<MeshRenderer>(entity).Mesh;
    REQUIRE(secondHandle.IsLoaded());

    // The handle swapped to the new shape's mesh: a 2.0 cube is larger than a 1.0 cube.
    CHECK(secondHandle.Get() != firstHandle.Get());
    CHECK(secondHandle->GetBounds().Max.x > firstBounds.Max.x);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture, "An empty variant leaves the renderer empty")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    Unique<Scene> scene = Scene::Create(Types);

    const Entity entity = SpawnPrimitive(*scene, PrimitiveShapeVariant{});

    ResolveComponents(*scene, entity, assets);

    // No shape to build: no MeshRenderer is added.
    CHECK(scene->TryGet<MeshRenderer>(entity) == nullptr);
}
