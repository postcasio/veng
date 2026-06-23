// The spawn-resolve seam, fired: a VE_RESOLVE'd component's resolver runs through
// Prefab::SpawnInto's post-populate pass and through the standalone
// ResolveComponents entry point, and a resolver may safely Add a component from
// inside (the spawn pass fetches storage fresh by TypeId, so the resolver's Add
// does not dangle a held pointer).
//
// It lives in the GPU band only because firing a resolver needs an AssetManager,
// whose constructor takes a Context; the resolver bodies here touch no device.

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Prefab.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Scene/Resolve.h>
#include <Veng/Scene/Scene.h>

#include <gpu/fixture.h>

using namespace Veng;

namespace
{
    // A resolver-bearing probe: its resolver stamps Marker, proving the seam fired.
    struct ResolveProbe
    {
        i32 Marker = 0;
    };

    // A probe the resolver Adds from inside, to prove an Add mid-resolve is safe.
    struct ResolveAdded
    {
        i32 Tag = 0;
    };

    void ResolveProbeFn(ResolveProbe& probe, Scene& scene, Entity entity, AssetManager&)
    {
        probe.Marker = 42;

        // Add a sibling component from inside the resolver. The spawn pass fetches
        // each component fresh by TypeId, so the pool growth this may trigger never
        // dangles a pointer the pass holds across it.
        if (!scene.Has<ResolveAdded>(entity))
        {
            scene.Add<ResolveAdded>(entity, ResolveAdded{.Tag = 7});
        }
    }
}

VE_REFLECT(::ResolveProbe, 0x6B3F0C9A41D27E58ULL)
VE_FIELD(Marker)
VE_REFLECT_END();

VE_RESOLVE(ResolveProbe, ResolveProbeFn);

VE_REFLECT(::ResolveAdded, 0x9D04F1C2A85B736EULL)
VE_FIELD(Tag)
VE_REFLECT_END();

namespace
{
    template <class T>
    Prefab::Component MakeComponent(const TypeRegistry& registry, const T& value)
    {
        Prefab::Component component;
        component.Type = registry.IdOf<T>();
        WriteFields(component.Record, &value, registry.Info(component.Type), registry);
        return component;
    }

    struct ResolveFixture : Veng::Test::GpuFixture
    {
        Unique<Scene> Stage;
        Unique<AssetManager> Assets;

        ResolveFixture()
        {
            RegisterBuiltinTypes(Types);
            Types.Register<ResolveProbe>();
            Types.Register<ResolveAdded>();
            Stage = Scene::Create(Types);
            Assets = CreateUnique<AssetManager>(Context, Tasks, Types);
        }
    };
}

TEST_CASE_FIXTURE(ResolveFixture, "SpawnInto fires a component's resolver in a post-populate pass")
{
    vector<Prefab::PrefabEntity> entities;
    entities.push_back({{MakeComponent(Types, ResolveProbe{})}});

    const Ref<Prefab> prefab = Prefab::Create(std::move(entities), {});
    const vector<Entity> roots = prefab->SpawnInto(*Stage, *Assets);

    REQUIRE(roots.size() == 1);
    CHECK(Stage->Get<ResolveProbe>(roots[0]).Marker == 42);

    // The resolver's Add landed: the spawned entity gained the sibling component.
    REQUIRE(Stage->Has<ResolveAdded>(roots[0]));
    CHECK(Stage->Get<ResolveAdded>(roots[0]).Tag == 7);
}

TEST_CASE_FIXTURE(ResolveFixture, "ResolveComponents fires a resolver on a live entity")
{
    const Entity entity = Stage->CreateEntity();
    Stage->Add<ResolveProbe>(entity, ResolveProbe{});
    REQUIRE(Stage->Get<ResolveProbe>(entity).Marker == 0);

    ResolveComponents(*Stage, entity, *Assets);

    CHECK(Stage->Get<ResolveProbe>(entity).Marker == 42);
    REQUIRE(Stage->Has<ResolveAdded>(entity));
    CHECK(Stage->Get<ResolveAdded>(entity).Tag == 7);
}

TEST_CASE_FIXTURE(ResolveFixture, "A prefab of non-resolver components fires no resolver")
{
    // Name carries no VE_RESOLVE, so SpawnInto does no extra resolve work for it.
    vector<Prefab::PrefabEntity> entities;
    entities.push_back({{MakeComponent(Types, Name{"plain"})}});

    const Ref<Prefab> prefab = Prefab::Create(std::move(entities), {});
    const vector<Entity> roots = prefab->SpawnInto(*Stage, *Assets);

    REQUIRE(roots.size() == 1);
    CHECK(Stage->Get<Name>(roots[0]).Value == "plain");
    CHECK_FALSE(Stage->Has<ResolveAdded>(roots[0]));
}
