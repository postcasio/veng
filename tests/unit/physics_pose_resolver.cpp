// The physics-pose resolver seam: the optional per-scene mapping between a Transform chain and its
// physics world's frame. With none installed a pose composes up the chain, which is exactly right
// when the two share an origin; installed, it is what lets a consumer whose authoritative positions
// live outside the f32 Transform put the engine's solver-space reads in the right frame. Pure CPU —
// a headless Scene, no solver, no GPU.

#include <doctest/doctest.h>

#include <Veng/Physics/PoseResolver.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/Transforms.h>

#include <glm/gtc/matrix_transform.hpp>

using namespace Veng;

namespace
{
    // A solver frame anchored away from the Transform chain's origin. The magnitude is irrelevant to
    // the mechanism — what matters is that the two frames disagree, so a pose read in one is wrong
    // in the other.
    constexpr dvec3 SolverOrigin(1000.0, 0.0, -2000.0);

    struct ResolverScene
    {
        TypeRegistry Types;
        Unique<Scene> World;

        ResolverScene()
        {
            RegisterBuiltinTypes(Types);
            World = Scene::Create(Types);
        }
    };

    // A resolver projecting the Transform chain into the offset solver frame, delegating the chain
    // composition itself to the engine's default.
    Unique<PhysicsPoseResolver> OffsetResolver()
    {
        return CreateUnique<PhysicsPoseResolver>(PhysicsPoseResolver{
            .Resolve =
                [](const Scene& scene, const Entity entity, const mat4& localOffset)
            {
                PhysicsPose pose = DefaultResolvePhysicsPose(scene, entity, localOffset);
                pose.Position += SolverOrigin;
                return pose;
            },
        });
    }
}

TEST_CASE("With no resolver installed a pose composes up the Transform chain")
{
    ResolverScene fixture;
    Scene& world = *fixture.World;

    const Entity parent = world.CreateEntity();
    world.Add<Transform>(parent, Transform{.Position = vec3(3.0f, 0.0f, 0.0f)});
    const Entity child = world.CreateEntity();
    world.Add<Transform>(child, Transform{.Position = vec3(0.0f, 2.0f, 0.0f)});
    world.SetParent(child, parent);

    const PhysicsPose pose = ResolvePhysicsPose(world, child);
    CHECK(pose.Position.x == doctest::Approx(3.0).epsilon(1.0e-5));
    CHECK(pose.Position.y == doctest::Approx(2.0).epsilon(1.0e-5));

    // A local offset is a pose in the entity's own frame, so it composes on the right exactly as the
    // world matrix does.
    const mat4 offset = glm::translate(mat4(1.0f), vec3(0.0f, 0.0f, -1.5f));
    const PhysicsPose offsetPose = ResolvePhysicsPose(world, child, offset);
    const vec3 expected = vec3(WorldMatrix(world, child) * offset[3]);
    CHECK(offsetPose.Position.x == doctest::Approx(f64(expected.x)).epsilon(1.0e-5));
    CHECK(offsetPose.Position.z == doctest::Approx(f64(expected.z)).epsilon(1.0e-5));
}

TEST_CASE("With no resolver installed a placement writes the entity's Transform")
{
    ResolverScene fixture;
    Scene& world = *fixture.World;

    const Entity entity = world.CreateEntity();
    world.Add<Transform>(entity, Transform{.Scale = vec3(2.0f)});
    PlaceAtPhysicsPose(
        world, entity,
        PhysicsPose{.Position = dvec3(1.0, 2.0, 3.0),
                    .Rotation = glm::angleAxis(glm::radians(90.0f), vec3(0.0f, 1.0f, 0.0f))});

    const Transform& transform = world.Get<Transform>(entity);
    CHECK(transform.Position.y == doctest::Approx(2.0f).epsilon(1.0e-5));
    CHECK(transform.Position.z == doctest::Approx(3.0f).epsilon(1.0e-5));
    // A pose is a rigid placement, so the scale a mesh socket may have left behind is reset.
    CHECK(transform.Scale.x == doctest::Approx(1.0f).epsilon(1.0e-5));

    // An entity with no Transform is given one.
    const Entity bare = world.CreateEntity();
    PlaceAtPhysicsPose(world, bare, PhysicsPose{.Position = dvec3(0.0, 5.0, 0.0)});
    REQUIRE(world.Has<Transform>(bare));
    CHECK(world.Get<Transform>(bare).Position.y == doctest::Approx(5.0f).epsilon(1.0e-5));
}

TEST_CASE("An installed resolver maps poses into the solver's frame")
{
    ResolverScene fixture;
    Scene& world = *fixture.World;
    world.SetPhysicsPoseResolver(OffsetResolver());

    const Entity entity = world.CreateEntity();
    world.Add<Transform>(entity, Transform{.Position = vec3(0.0f, 1.0f, 0.0f)});

    const PhysicsPose pose = ResolvePhysicsPose(world, entity);
    CHECK(pose.Position.x == doctest::Approx(SolverOrigin.x).epsilon(1.0e-9));
    CHECK(pose.Position.y == doctest::Approx(1.0).epsilon(1.0e-5));
    CHECK(pose.Position.z == doctest::Approx(SolverOrigin.z).epsilon(1.0e-9));
}

TEST_CASE("A resolver overriding one direction leaves the other on its default")
{
    ResolverScene fixture;
    Scene& world = *fixture.World;
    // OffsetResolver supplies Resolve and leaves Place empty, which is the partial-adoption case.
    world.SetPhysicsPoseResolver(OffsetResolver());

    const Entity entity = world.CreateEntity();
    world.Add<Transform>(entity, Transform{});
    PlaceAtPhysicsPose(world, entity, PhysicsPose{.Position = dvec3(0.0, 7.0, 0.0)});
    CHECK(world.Get<Transform>(entity).Position.y == doctest::Approx(7.0f).epsilon(1.0e-5));
}

TEST_CASE("A placement hook is where a consumer records the pose an engine system resolved")
{
    ResolverScene fixture;
    Scene& world = *fixture.World;

    dvec3 recorded(0.0);
    Entity recordedFor = Entity::Null;
    world.SetPhysicsPoseResolver(CreateUnique<PhysicsPoseResolver>(PhysicsPoseResolver{
        .Place =
            [&recorded, &recordedFor](Scene&, const Entity entity, const PhysicsPose& pose)
        {
            recorded = pose.Position;
            recordedFor = entity;
        },
    }));

    const Entity entity = world.CreateEntity();
    world.Add<Transform>(entity, Transform{});
    PlaceAtPhysicsPose(world, entity, PhysicsPose{.Position = dvec3(4.0, 5.0, 6.0)});

    CHECK(recordedFor == entity);
    CHECK(recorded.y == doctest::Approx(5.0).epsilon(1.0e-9));
    // The consumer's hook owns the write, so the engine left the f32 Transform alone.
    CHECK(world.Get<Transform>(entity).Position.y == doctest::Approx(0.0f).epsilon(1.0e-5));
}

TEST_CASE("Cloning a scene does not copy its pose resolver")
{
    ResolverScene fixture;
    fixture.World->SetPhysicsPoseResolver(OffsetResolver());
    const Unique<Scene> clone = fixture.World->Clone();
    CHECK(clone->GetPhysicsPoseResolver() == nullptr);
}
