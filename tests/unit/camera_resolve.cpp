// Camera resolve: the Viewer seat selection and ResolveCameraView /
// ResolvePrimaryCameraView helpers. Pure CPU — no Context, no Vulkan symbol touched;
// builds a real Scene over a TypeRegistry and queries it.

#include <doctest/doctest.h>

#include <glm/gtc/matrix_transform.hpp>

#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

using namespace Veng;

namespace
{
    bool VecApprox(const vec3& a, const vec3& b, f32 eps = 1e-4f)
    {
        return glm::all(glm::lessThan(glm::abs(a - b), vec3(eps)));
    }

    // A registry with the builtins (Transform, Camera, Viewer, Hierarchy) registered,
    // the minimum a resolve query needs.
    TypeRegistry MakeRegistry()
    {
        TypeRegistry registry;
        RegisterBuiltinTypes(registry);
        return registry;
    }

    Entity MakeCamera(Scene& scene, vec3 position)
    {
        const Entity entity = scene.CreateEntity();
        Transform transform;
        transform.Position = position;
        scene.Add<Transform>(entity, transform);
        scene.Add<Camera>(entity, Camera{});
        return entity;
    }
}

TEST_CASE("ResolveCameraView resolves a Viewer naming a (Transform, Camera) entity")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    const Entity camera = MakeCamera(*scene, vec3{1.0f, 2.0f, 3.0f});
    const Entity seat = scene->CreateEntity();
    scene->Add<Viewer>(seat, Viewer{.Camera = camera});

    const optional<CameraView> view = ResolveCameraView(*scene, seat, 16.0f / 9.0f);
    REQUIRE(view.has_value());
    CHECK(VecApprox(view->GetPosition(), vec3{1.0f, 2.0f, 3.0f}));
}

TEST_CASE("ResolvePrimaryCameraView picks the first Viewer, else the first bare camera")
{
    SUBCASE("a Viewer takes priority over a bare camera")
    {
        TypeRegistry registry = MakeRegistry();
        const Unique<Scene> scene = Scene::Create(registry);

        MakeCamera(*scene, vec3{9.0f, 0.0f, 0.0f}); // bare camera, no seat
        const Entity viewed = MakeCamera(*scene, vec3{1.0f, 2.0f, 3.0f});
        const Entity seat = scene->CreateEntity();
        scene->Add<Viewer>(seat, Viewer{.Camera = viewed});

        const optional<CameraView> view = ResolvePrimaryCameraView(*scene, 1.0f);
        REQUIRE(view.has_value());
        CHECK(VecApprox(view->GetPosition(), vec3{1.0f, 2.0f, 3.0f}));
    }

    SUBCASE("falls back to the first bare camera when no Viewer exists")
    {
        TypeRegistry registry = MakeRegistry();
        const Unique<Scene> scene = Scene::Create(registry);

        MakeCamera(*scene, vec3{4.0f, 5.0f, 6.0f});

        const optional<CameraView> view = ResolvePrimaryCameraView(*scene, 1.0f);
        REQUIRE(view.has_value());
        CHECK(VecApprox(view->GetPosition(), vec3{4.0f, 5.0f, 6.0f}));
    }

    SUBCASE("returns nullopt for a scene with no camera at all")
    {
        TypeRegistry registry = MakeRegistry();
        const Unique<Scene> scene = Scene::Create(registry);
        (void)scene->CreateEntity();

        CHECK_FALSE(ResolvePrimaryCameraView(*scene, 1.0f).has_value());
    }
}

TEST_CASE("ResolveCameraView walks the Parent edge for a parented camera")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    // A rig translated in world space, with the camera a child translated again in
    // local space: the resolved position is the composed world translation, proving
    // the Parent walk is honored and not just the local transform.
    const Entity rig = scene->CreateEntity();
    Transform rigTransform;
    rigTransform.Position = vec3{10.0f, 0.0f, 0.0f};
    scene->Add<Transform>(rig, rigTransform);

    const Entity camera = MakeCamera(*scene, vec3{0.0f, 5.0f, 0.0f});
    scene->SetParent(camera, rig);

    const Entity seat = scene->CreateEntity();
    scene->Add<Viewer>(seat, Viewer{.Camera = camera});

    const optional<CameraView> view = ResolveCameraView(*scene, seat, 1.0f);
    REQUIRE(view.has_value());
    CHECK(VecApprox(view->GetPosition(), vec3{10.0f, 5.0f, 0.0f}));
}

TEST_CASE("ResolveCameraView plumbs the caller's aspect into the projection")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    const Entity camera = MakeCamera(*scene, vec3{0.0f});
    const Entity seat = scene->CreateEntity();
    scene->Add<Viewer>(seat, Viewer{.Camera = camera});

    const optional<CameraView> wide = ResolveCameraView(*scene, seat, 21.0f / 9.0f);
    const optional<CameraView> narrow = ResolveCameraView(*scene, seat, 4.0f / 3.0f);
    REQUIRE(wide.has_value());
    REQUIRE(narrow.has_value());

    // Aspect scales the projection's X term; two aspects produce two projections.
    CHECK(wide->Projection()[0][0] != doctest::Approx(narrow->Projection()[0][0]));
}

TEST_CASE("ResolveCameraView returns nullopt for degenerate references")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    SUBCASE("a seat with no Viewer")
    {
        const Entity seat = scene->CreateEntity();
        CHECK_FALSE(ResolveCameraView(*scene, seat, 1.0f).has_value());
    }

    SUBCASE("a Viewer naming Entity::Null")
    {
        const Entity seat = scene->CreateEntity();
        scene->Add<Viewer>(seat, Viewer{.Camera = Entity::Null});
        CHECK_FALSE(ResolveCameraView(*scene, seat, 1.0f).has_value());
    }

    SUBCASE("a Viewer naming an entity that lacks a Camera")
    {
        const Entity target = scene->CreateEntity();
        scene->Add<Transform>(target, Transform{});

        const Entity seat = scene->CreateEntity();
        scene->Add<Viewer>(seat, Viewer{.Camera = target});
        CHECK_FALSE(ResolveCameraView(*scene, seat, 1.0f).has_value());
    }
}
