// The mesh-source recipe path end to end: a MeshRenderer whose Source carries a shape
// recipe is built into its Mesh during Prefab::SpawnInto's populate pass, yielding a
// pending handle that streams to residency through the ordinary async load path.
// Covers residency after pumping, that identical recipes resolve to distinct meshes
// (no dedup), that an empty Source leaves the cooked Mesh untouched, and that SpawnInto's
// ResidencyBatch tracks the pending recipe handle and waits it resident.
//
// It lives in the GPU band because SpawnInto + Build<Mesh> need an AssetManager, whose
// constructor takes a Context; the bodies here touch no device beyond the upload.

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Prefab.h>
#include <Veng/Asset/Primitives.h>
#include <Veng/Math/AABB.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Task/TaskSystem.h>

#include <gpu/fixture.h>

using namespace Veng;

namespace
{
    // Pumps the worker drain + main-thread continuation + finalize so pending
    // Build handles flip resident.
    void PumpUntilResident(TaskSystem& tasks, AssetManager& manager)
    {
        tasks.WaitForAll();
        tasks.PumpMainThread();
        manager.PumpFinalizes();
    }

    template <class Shape>
    MeshSource SourceOf(const Shape& value)
    {
        MeshSource source;
        *static_cast<Shape*>(source.SetActive(TypeIdOf<Shape>())) = value;
        return source;
    }

    template <class T>
    Prefab::Component MakeComponent(const TypeRegistry& registry, const T& value)
    {
        Prefab::Component component;
        component.Type = registry.IdOf<T>();
        WriteFields(component.Record, &value, registry.Info(component.Type), registry);
        return component;
    }

    // Builds a one-entity prefab whose lone component is a MeshRenderer with the given
    // recipe source, then spawns it and returns the spawned root.
    Entity SpawnRecipe(const TypeRegistry& types, Scene& scene, AssetManager& manager,
                       const MeshSource& source)
    {
        MeshRenderer renderer;
        renderer.Source = source;

        vector<Prefab::PrefabEntity> entities;
        entities.push_back({{MakeComponent(types, renderer)}});
        const Ref<Prefab> prefab = Prefab::Create(std::move(entities), {});
        const vector<Entity> roots = prefab->SpawnInto(scene, manager).Roots;
        REQUIRE(roots.size() == 1);
        return roots[0];
    }

    MeshSource CubeSource(f32 extent)
    {
        return SourceOf(CubeShape{.Extent = extent});
    }

    MeshSource CylinderSource(f32 radius, f32 height, u32 segments)
    {
        return SourceOf(CylinderShape{.Radius = radius, .Height = height, .Segments = segments});
    }

    struct RecipeFixture : Veng::Test::GpuFixture
    {
        Unique<Scene> Stage;
        Unique<AssetManager> Assets;

        RecipeFixture()
        {
            RegisterBuiltinTypes(Types);
            Stage = Scene::Create(Types);
            Assets = CreateUnique<AssetManager>(Context, Tasks, Types);
        }
    };
}

TEST_CASE_FIXTURE(RecipeFixture, "SpawnInto streams in a recipe-sourced mesh")
{
    const Entity entity = SpawnRecipe(Types, *Stage, *Assets, CubeSource(1.0f));

    // The MeshRenderer holds a pending (not-yet-resident) handle right after spawn.
    const MeshRenderer* renderer = Stage->TryGet<MeshRenderer>(entity);
    REQUIRE(renderer != nullptr);
    CHECK_FALSE(renderer->Mesh.IsLoaded());

    PumpUntilResident(Tasks, *Assets);

    REQUIRE(Stage->Get<MeshRenderer>(entity).Mesh.IsLoaded());
    CHECK(Stage->Get<MeshRenderer>(entity).Mesh->GetIndexCount() ==
          Primitives::Cube(1.0f).Indices.size());
}

TEST_CASE_FIXTURE(RecipeFixture, "SpawnInto streams in a cylinder recipe mesh")
{
    const Entity entity = SpawnRecipe(Types, *Stage, *Assets, CylinderSource(0.5f, 1.0f, 24));

    PumpUntilResident(Tasks, *Assets);

    REQUIRE(Stage->Get<MeshRenderer>(entity).Mesh.IsLoaded());
    CHECK(Stage->Get<MeshRenderer>(entity).Mesh->GetIndexCount() ==
          Primitives::Cylinder(0.5f, 1.0f, 24).Indices.size());
}

TEST_CASE_FIXTURE(RecipeFixture, "Identical recipes resolve to distinct meshes")
{
    const Entity a = SpawnRecipe(Types, *Stage, *Assets, CubeSource(1.0f));
    const Entity b = SpawnRecipe(Types, *Stage, *Assets, CubeSource(1.0f));

    PumpUntilResident(Tasks, *Assets);

    const MeshRenderer& ra = Stage->Get<MeshRenderer>(a);
    const MeshRenderer& rb = Stage->Get<MeshRenderer>(b);
    REQUIRE(ra.Mesh.IsLoaded());
    REQUIRE(rb.Mesh.IsLoaded());
    // No dedup cache: each spawn builds its own mesh.
    CHECK(ra.Mesh.Get() != rb.Mesh.Get());
}

TEST_CASE_FIXTURE(RecipeFixture, "A larger recipe builds a larger mesh")
{
    const Entity small = SpawnRecipe(Types, *Stage, *Assets, CubeSource(1.0f));
    const Entity large = SpawnRecipe(Types, *Stage, *Assets, CubeSource(2.0f));

    PumpUntilResident(Tasks, *Assets);

    const AssetHandle<Mesh> smallMesh = Stage->Get<MeshRenderer>(small).Mesh;
    const AssetHandle<Mesh> largeMesh = Stage->Get<MeshRenderer>(large).Mesh;
    REQUIRE(smallMesh.IsLoaded());
    REQUIRE(largeMesh.IsLoaded());

    // The 2.0 cube's bounds exceed the 1.0 cube's: the source drove the geometry.
    CHECK(largeMesh->GetBounds().Max.x > smallMesh->GetBounds().Max.x);
}

TEST_CASE_FIXTURE(RecipeFixture,
                  "SpawnInto's batch tracks the pending recipe mesh and waits it resident")
{
    MeshRenderer renderer;
    renderer.Source = CubeSource(1.0f);

    vector<Prefab::PrefabEntity> entities;
    entities.push_back({{MakeComponent(Types, renderer)}});
    const Ref<Prefab> prefab = Prefab::Create(std::move(entities), {});

    Prefab::SpawnResult spawned = prefab->SpawnInto(*Stage, *Assets);
    REQUIRE(spawned.Roots.size() == 1);

    // The recipe mesh streams in async, so the spawn's batch holds exactly that one pending handle.
    CHECK(spawned.Pending.TotalCount() == 1);
    CHECK(spawned.Pending.ResidentCount() == 0);
    CHECK_FALSE(spawned.Pending.IsResident());

    // WaitResident owns the pump-and-sleep loop, landing the upload continuation + finalize.
    spawned.Pending.WaitResident(Tasks);
    CHECK(spawned.Pending.IsResident());
    CHECK(Stage->Get<MeshRenderer>(spawned.Roots[0]).Mesh.IsLoaded());
}

TEST_CASE_FIXTURE(RecipeFixture, "An empty source spawn yields an empty, already-resident batch")
{
    vector<Prefab::PrefabEntity> entities;
    entities.push_back({{MakeComponent(Types, MeshRenderer{})}});
    const Ref<Prefab> prefab = Prefab::Create(std::move(entities), {});

    const Prefab::SpawnResult spawned = prefab->SpawnInto(*Stage, *Assets);

    // No recipe and no resident cooked handle to track: the batch is empty and resident.
    CHECK(spawned.Pending.IsEmpty());
    CHECK(spawned.Pending.IsResident());
    CHECK(spawned.Pending.TotalCount() == 0);
}

TEST_CASE_FIXTURE(RecipeFixture, "An empty source leaves the cooked Mesh untouched")
{
    const Entity entity = SpawnRecipe(Types, *Stage, *Assets, MeshSource{});

    // No recipe to build: the MeshRenderer's Mesh stays the authored (empty) handle.
    const MeshRenderer* renderer = Stage->TryGet<MeshRenderer>(entity);
    REQUIRE(renderer != nullptr);
    CHECK_FALSE(renderer->Mesh.IsLoaded());
    CHECK(renderer->Source.ActiveType() == InvalidTypeId);
}
