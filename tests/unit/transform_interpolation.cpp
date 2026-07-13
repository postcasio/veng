// Fixed-timestep render interpolation cases. Two device-free surfaces: the pure InterpolateTransform
// blend (lerp position/scale, slerp rotation) and the Scene transform-history ring feeding
// GetInterpolatedWorldTransform. These pin the analytic blend, the two-tick snapshot roll, the
// hierarchy compose, the static-scene convergence, and the fall-back to the live pose — no device.

#include <doctest/doctest.h>

#include <glm/gtc/epsilon.hpp>

#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/Transforms.h>

using namespace Veng;

namespace
{
    TypeRegistry MakeRegistry()
    {
        TypeRegistry registry;
        registry.Register<Name>("Name");
        registry.Register<Transform>("Transform");
        registry.Register<Hierarchy>("Hierarchy");
        registry.Register<ViewPose>("ViewPose");
        return registry;
    }

    // The translation column of a world matrix (where the interpolation is easiest to read).
    vec3 Translation(const mat4& m)
    {
        return vec3(m[3]);
    }
}

TEST_CASE("interpolate transform: alpha endpoints return the exact endpoints")
{
    const Transform from{.Position = {0.0f, 0.0f, 0.0f},
                         .Rotation = glm::angleAxis(0.0f, vec3(0, 1, 0)),
                         .Scale = {1.0f, 1.0f, 1.0f}};
    const Transform to{.Position = {10.0f, 0.0f, 0.0f},
                       .Rotation = glm::angleAxis(glm::radians(90.0f), vec3(0, 1, 0)),
                       .Scale = {3.0f, 3.0f, 3.0f}};

    const Transform atZero = InterpolateTransform(from, to, 0.0f);
    CHECK(glm::all(glm::epsilonEqual(atZero.Position, from.Position, 1e-5f)));
    CHECK(glm::all(glm::epsilonEqual(atZero.Scale, from.Scale, 1e-5f)));

    const Transform atOne = InterpolateTransform(from, to, 1.0f);
    CHECK(glm::all(glm::epsilonEqual(atOne.Position, to.Position, 1e-5f)));
    CHECK(glm::all(glm::epsilonEqual(atOne.Scale, to.Scale, 1e-5f)));
}

TEST_CASE("interpolate transform: the midpoint is the analytic linear blend of position and scale")
{
    const Transform from{.Position = {0.0f, 0.0f, 0.0f}, .Scale = {1.0f, 1.0f, 1.0f}};
    const Transform to{.Position = {8.0f, 4.0f, -2.0f}, .Scale = {3.0f, 3.0f, 3.0f}};

    const Transform mid = InterpolateTransform(from, to, 0.25f);
    CHECK(glm::all(glm::epsilonEqual(mid.Position, vec3(2.0f, 1.0f, -0.5f), 1e-5f)));
    CHECK(glm::all(glm::epsilonEqual(mid.Scale, vec3(1.5f, 1.5f, 1.5f), 1e-5f)));
}

TEST_CASE("scene interpolation: two ticks of motion blend the world position by alpha")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    const Entity entity = scene->CreateEntity();
    scene->Add<Transform>(entity, Transform{.Position = {0.0f, 0.0f, 0.0f}});

    // Tick 1: the first snapshot has no previous entry for the entity, so interpolation returns the
    // live pose regardless of alpha.
    scene->SnapshotTransformHistory();
    {
        const mat4 world = scene->GetInterpolatedWorldTransform(entity, 0.5f);
        CHECK(glm::all(glm::epsilonEqual(Translation(world), vec3(0.0f), 1e-5f)));
    }

    // Tick 2: move to x=10 and snapshot, so the ring holds {0, 10}.
    scene->Get<Transform>(entity).Position = {10.0f, 0.0f, 0.0f};
    scene->SnapshotTransformHistory();
    CHECK(scene->HasTransformInterpolation());

    // Alpha 0 renders the previous tick, alpha 1 the current, alpha 0.5 the analytic midpoint.
    CHECK(glm::all(glm::epsilonEqual(
        Translation(scene->GetInterpolatedWorldTransform(entity, 0.0f)), vec3(0.0f), 1e-5f)));
    CHECK(glm::all(glm::epsilonEqual(
        Translation(scene->GetInterpolatedWorldTransform(entity, 1.0f)), vec3(10, 0, 0), 1e-5f)));
    CHECK(glm::all(glm::epsilonEqual(
        Translation(scene->GetInterpolatedWorldTransform(entity, 0.5f)), vec3(5, 0, 0), 1e-5f)));
}

TEST_CASE("scene interpolation: a static scene converges so the pose stops interpolating")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    const Entity entity = scene->CreateEntity();
    scene->Add<Transform>(entity, Transform{.Position = {0.0f, 0.0f, 0.0f}});

    scene->SnapshotTransformHistory();
    scene->Get<Transform>(entity).Position = {10.0f, 0.0f, 0.0f};
    scene->SnapshotTransformHistory();
    REQUIRE(scene->HasTransformInterpolation());

    // No further motion: the next snapshot (spatial version unchanged) converges prev to cur, so the
    // scene reports no interpolation and any alpha renders the settled current pose.
    scene->SnapshotTransformHistory();
    CHECK_FALSE(scene->HasTransformInterpolation());
    CHECK(glm::all(glm::epsilonEqual(
        Translation(scene->GetInterpolatedWorldTransform(entity, 0.5f)), vec3(10, 0, 0), 1e-5f)));
}

TEST_CASE("scene interpolation: a child composes its parent's interpolated world")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    const Entity parent = scene->CreateEntity();
    scene->Add<Transform>(parent, Transform{.Position = {0.0f, 0.0f, 0.0f}});
    const Entity child = scene->CreateEntity();
    scene->Add<Transform>(child, Transform{.Position = {2.0f, 0.0f, 0.0f}});
    scene->SetParent(child, parent);

    scene->SnapshotTransformHistory();
    scene->Get<Transform>(parent).Position = {10.0f, 0.0f, 0.0f};
    scene->SnapshotTransformHistory();

    // At alpha 0.5 the parent is halfway to x=5, and the child sits at parent + local(2) = 7.
    const mat4 childWorld = scene->GetInterpolatedWorldTransform(child, 0.5f);
    CHECK(glm::all(glm::epsilonEqual(Translation(childWorld), vec3(7, 0, 0), 1e-5f)));
}

TEST_CASE("scene interpolation: a ViewPose entity resolves its live pose, never the history")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    const Entity entity = scene->CreateEntity();
    scene->Add<Transform>(entity, Transform{.Position = {0.0f, 0.0f, 0.0f}});
    scene->Add<ViewPose>(entity);

    // Two ticks of history {0, 10}, then a per-frame (post-snapshot) write to x=20 — the View-phase
    // authoring pattern. The tagged entity renders the live pose at any alpha; untagged it would
    // blend the stale ring.
    scene->SnapshotTransformHistory();
    scene->Get<Transform>(entity).Position = {10.0f, 0.0f, 0.0f};
    scene->SnapshotTransformHistory();
    scene->Get<Transform>(entity).Position = {20.0f, 0.0f, 0.0f};
    REQUIRE(scene->HasTransformInterpolation());

    CHECK(glm::all(glm::epsilonEqual(
        Translation(scene->GetInterpolatedWorldTransform(entity, 0.0f)), vec3(20, 0, 0), 1e-5f)));
    CHECK(glm::all(glm::epsilonEqual(
        Translation(scene->GetInterpolatedWorldTransform(entity, 0.5f)), vec3(20, 0, 0), 1e-5f)));
}

TEST_CASE("scene interpolation: a ViewPose child stays live under an interpolated parent")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    const Entity parent = scene->CreateEntity();
    scene->Add<Transform>(parent, Transform{.Position = {0.0f, 0.0f, 0.0f}});
    const Entity child = scene->CreateEntity();
    scene->Add<Transform>(child, Transform{.Position = {2.0f, 0.0f, 0.0f}});
    scene->Add<ViewPose>(child);
    scene->SetParent(child, parent);

    scene->SnapshotTransformHistory();
    scene->Get<Transform>(parent).Position = {10.0f, 0.0f, 0.0f};
    scene->Get<Transform>(child).Position = {4.0f, 0.0f, 0.0f};
    scene->SnapshotTransformHistory();
    scene->Get<Transform>(child).Position = {6.0f, 0.0f, 0.0f};

    // The parent level blends to x=5; the tagged child contributes its live local(6) = 11 —
    // ancestor interpolation composes with the child's per-frame pose.
    const mat4 childWorld = scene->GetInterpolatedWorldTransform(child, 0.5f);
    CHECK(glm::all(glm::epsilonEqual(Translation(childWorld), vec3(11, 0, 0), 1e-5f)));
}
