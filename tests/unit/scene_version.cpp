// The Scene spatial-version counter, the const iteration path, and recursive
// DestroyEntity: pure ECS, device-free. The broadphase reads GetSpatialVersion()
// to decide "did anything spatial move?" — so this pins which mutations and
// accesses bump it (the three spatial pools: Transform, Parent, MeshRenderer),
// which leave it alone (Light, every const read), and that destroying a parent
// takes its whole subtree with it.

#include <doctest/doctest.h>

#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

using namespace Veng;

namespace
{
    // The spatial pools plus Light — the test exercises both the bump and the
    // no-bump arms. Registration is GPU-free, the same path RegisterBuiltinTypes
    // uses.
    TypeRegistry MakeRegistry()
    {
        TypeRegistry types;
        types.Register<Transform>("Transform");
        types.Register<Parent>("Parent");
        types.Register<MeshRenderer>("MeshRenderer");
        types.Register<Light>("Light");
        return types;
    }
}

TEST_CASE("Adding each spatial component bumps the spatial version")
{
    TypeRegistry types = MakeRegistry();
    Unique<Scene> scene = Scene::Create(types);

    SUBCASE("Transform")
    {
        const Entity e = scene->CreateEntity();
        const u64 before = scene->GetSpatialVersion();
        scene->Add<Transform>(e);
        CHECK(scene->GetSpatialVersion() > before);
    }

    SUBCASE("Parent")
    {
        const Entity e = scene->CreateEntity();
        const u64 before = scene->GetSpatialVersion();
        scene->Add<Parent>(e);
        CHECK(scene->GetSpatialVersion() > before);
    }

    SUBCASE("MeshRenderer")
    {
        const Entity e = scene->CreateEntity();
        const u64 before = scene->GetSpatialVersion();
        scene->Add<MeshRenderer>(e);
        CHECK(scene->GetSpatialVersion() > before);
    }
}

TEST_CASE("CreateEntity alone does not bump — a bare entity is no candidate")
{
    TypeRegistry types = MakeRegistry();
    Unique<Scene> scene = Scene::Create(types);

    const u64 before = scene->GetSpatialVersion();
    scene->CreateEntity();
    CHECK(scene->GetSpatialVersion() == before);
}

TEST_CASE("Removing each spatial component bumps the spatial version")
{
    TypeRegistry types = MakeRegistry();
    Unique<Scene> scene = Scene::Create(types);

    SUBCASE("Transform")
    {
        const Entity e = scene->CreateEntity();
        scene->Add<Transform>(e);
        const u64 before = scene->GetSpatialVersion();
        scene->Remove<Transform>(e);
        CHECK(scene->GetSpatialVersion() > before);
    }

    SUBCASE("Parent")
    {
        const Entity e = scene->CreateEntity();
        scene->Add<Parent>(e);
        const u64 before = scene->GetSpatialVersion();
        scene->Remove<Parent>(e);
        CHECK(scene->GetSpatialVersion() > before);
    }

    SUBCASE("MeshRenderer")
    {
        const Entity e = scene->CreateEntity();
        scene->Add<MeshRenderer>(e);
        const u64 before = scene->GetSpatialVersion();
        scene->Remove<MeshRenderer>(e);
        CHECK(scene->GetSpatialVersion() > before);
    }
}

TEST_CASE("DestroyEntity of a spatial-holding entity bumps the version")
{
    TypeRegistry types = MakeRegistry();
    Unique<Scene> scene = Scene::Create(types);

    const Entity e = scene->CreateEntity();
    scene->Add<Transform>(e);
    const u64 before = scene->GetSpatialVersion();
    scene->DestroyEntity(e);
    CHECK(scene->GetSpatialVersion() > before);
}

TEST_CASE("Light is not a spatial pool — its mutations never bump")
{
    TypeRegistry types = MakeRegistry();
    Unique<Scene> scene = Scene::Create(types);

    const Entity e = scene->CreateEntity();

    const u64 afterCreate = scene->GetSpatialVersion();
    scene->Add<Light>(e);
    CHECK(scene->GetSpatialVersion() == afterCreate);

    const u64 afterAdd = scene->GetSpatialVersion();
    scene->DestroyEntity(e);
    CHECK(scene->GetSpatialVersion() == afterAdd);
}

TEST_CASE("Non-const spatial access is a write proxy — it bumps")
{
    TypeRegistry types = MakeRegistry();
    Unique<Scene> scene = Scene::Create(types);

    const Entity e = scene->CreateEntity();
    scene->Add<Transform>(e);

    SUBCASE("Get<Transform>")
    {
        const u64 before = scene->GetSpatialVersion();
        Transform& transform = scene->Get<Transform>(e);
        transform.Position.x = 5.0f;
        CHECK(scene->GetSpatialVersion() > before);
    }

    SUBCASE("View<Transform>")
    {
        const u64 before = scene->GetSpatialVersion();
        for (auto [entity, transform] : scene->View<Transform>())
        {
            (void)entity;
            transform.Position.y = 1.0f;
        }
        CHECK(scene->GetSpatialVersion() > before);
    }

    SUBCASE("Each<Transform>")
    {
        const u64 before = scene->GetSpatialVersion();
        scene->Each<Transform>([](Entity, Transform& transform) { transform.Position.z = 1.0f; });
        CHECK(scene->GetSpatialVersion() > before);
    }

    SUBCASE("ForEachComponent over a Transform-holder")
    {
        const u64 before = scene->GetSpatialVersion();
        scene->ForEachComponent(e, [](TypeId, void*) {});
        CHECK(scene->GetSpatialVersion() > before);
    }
}

TEST_CASE("Non-const View<Light> does not bump — Light is not spatial")
{
    TypeRegistry types = MakeRegistry();
    Unique<Scene> scene = Scene::Create(types);

    const Entity e = scene->CreateEntity();
    scene->Add<Light>(e);

    const u64 before = scene->GetSpatialVersion();
    for (auto [entity, light] : scene->View<Light>())
    {
        (void)entity;
        (void)light;
    }
    CHECK(scene->GetSpatialVersion() == before);
}

TEST_CASE("The const iteration path never bumps the version")
{
    TypeRegistry types = MakeRegistry();
    Unique<Scene> scene = Scene::Create(types);

    const Entity e = scene->CreateEntity();
    scene->Add<Transform>(e);

    // A const view of the scene routes through the const TryGetRaw only — the
    // property the broadphase depends on: it can read the scene each frame
    // without marking it dirty.
    const Scene& constScene = *scene;

    SUBCASE("const View<Transform>")
    {
        const u64 before = scene->GetSpatialVersion();
        for (auto [entity, transform] : constScene.View<Transform>())
        {
            (void)entity;
            CHECK(transform.Scale.x == doctest::Approx(1.0f));
        }
        CHECK(scene->GetSpatialVersion() == before);
    }

    SUBCASE("const Each<Transform>")
    {
        const u64 before = scene->GetSpatialVersion();
        constScene.Each<Transform>([](Entity, const Transform& transform)
                                   { CHECK(transform.Scale.y == doctest::Approx(1.0f)); });
        CHECK(scene->GetSpatialVersion() == before);
    }

    SUBCASE("const TryGet<Transform>")
    {
        const u64 before = scene->GetSpatialVersion();
        const Transform* transform = constScene.TryGet<Transform>(e);
        CHECK(transform != nullptr);
        CHECK(scene->GetSpatialVersion() == before);
    }
}

TEST_CASE("The version is monotonic — it only ever increases")
{
    TypeRegistry types = MakeRegistry();
    Unique<Scene> scene = Scene::Create(types);

    u64 last = scene->GetSpatialVersion();
    for (int i = 0; i < 8; ++i)
    {
        const Entity e = scene->CreateEntity();
        scene->Add<Transform>(e);
        const u64 now = scene->GetSpatialVersion();
        CHECK(now >= last);
        last = now;
    }
}

TEST_CASE("DestroyEntity is recursive — it destroys the whole subtree")
{
    TypeRegistry types = MakeRegistry();
    Unique<Scene> scene = Scene::Create(types);

    // root → child → grandchild, plus a sibling subtree under a different root.
    const Entity root = scene->CreateEntity();
    scene->Add<Transform>(root);

    const Entity child = scene->CreateEntity();
    scene->Add<Transform>(child);
    scene->Add<Parent>(child, Parent{.Value = root});

    const Entity grandchild = scene->CreateEntity();
    scene->Add<Transform>(grandchild);
    scene->Add<Parent>(grandchild, Parent{.Value = child});

    const Entity otherRoot = scene->CreateEntity();
    scene->Add<Transform>(otherRoot);

    const Entity otherChild = scene->CreateEntity();
    scene->Add<Transform>(otherChild);
    scene->Add<Parent>(otherChild, Parent{.Value = otherRoot});

    const u64 before = scene->GetSpatialVersion();
    scene->DestroyEntity(root);

    // The whole root subtree is dead; their slots are recycled (the stale handles
    // no longer alive).
    CHECK_FALSE(scene->IsAlive(root));
    CHECK_FALSE(scene->IsAlive(child));
    CHECK_FALSE(scene->IsAlive(grandchild));

    // The sibling subtree is untouched.
    CHECK(scene->IsAlive(otherRoot));
    CHECK(scene->IsAlive(otherChild));

    // Destroying spatial-holding entities bumped the version.
    CHECK(scene->GetSpatialVersion() > before);
}

TEST_CASE("Destroying a leaf entity destroys only itself")
{
    TypeRegistry types = MakeRegistry();
    Unique<Scene> scene = Scene::Create(types);

    const Entity root = scene->CreateEntity();
    scene->Add<Transform>(root);

    const Entity child = scene->CreateEntity();
    scene->Add<Transform>(child);
    scene->Add<Parent>(child, Parent{.Value = root});

    scene->DestroyEntity(child);

    CHECK_FALSE(scene->IsAlive(child));
    CHECK(scene->IsAlive(root));
}
