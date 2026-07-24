// The character controller: a kinematic capsule whose up comes from the gravity field. Drives the
// PhysicsWorld capsule API directly (flat-floor locomotion, slope classification, stair stepping,
// the up-vector circuit inside an Axial field, a moving kinematic platform) and the builtin
// CharacterMovementSystem end to end (CharacterState output, zero-gravity float). Pure CPU — a
// headless Scene and a solver, no Context, no GPU.

#include <doctest/doctest.h>

#include <Veng/Input.h>
#include <Veng/Physics/CharacterController.h>
#include <Veng/Physics/CharacterMovementSystem.h>
#include <Veng/Physics/Components.h>
#include <Veng/Physics/Gravity.h>
#include <Veng/Physics/PhysicsSystem.h>
#include <Veng/Physics/PhysicsWorld.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cmath>

using namespace Veng;

namespace
{
    constexpr f32 FixedStep = 1.0f / 60.0f;

    // A quaternion rotating unit `from` onto unit `to`.
    quat FromTo(const vec3 from, const vec3 to)
    {
        const vec3 a = glm::normalize(from);
        const vec3 b = glm::normalize(to);
        const f32 dot = glm::dot(a, b);
        if (dot > 0.9999f)
        {
            return quat(1.0f, 0.0f, 0.0f, 0.0f);
        }
        if (dot < -0.9999f)
        {
            const vec3 axis = glm::normalize(glm::cross(
                a, std::abs(a.x) < 0.9f ? vec3(1.0f, 0.0f, 0.0f) : vec3(0.0f, 1.0f, 0.0f)));
            return glm::angleAxis(glm::pi<f32>(), axis);
        }
        return glm::angleAxis(std::acos(dot), glm::normalize(glm::cross(a, b)));
    }

    // Adds a static box body to a standalone world and returns the entity keyed to it.
    Entity AddStaticBox(PhysicsWorld& world, const u32 index, const dvec3 position,
                        const quat rotation, const vec3 halfExtents)
    {
        const Entity entity{.Index = index};
        world.CreateBody(entity,
                         RigidBody{.Motion = MotionType::Static, .Layer = PhysicsLayer::Static},
                         Collider{.Shape = ColliderShape::Box, .Extents = halfExtents},
                         PhysicsPose{.Position = position, .Rotation = rotation});
        return entity;
    }

    // A default walking capsule: ~1.8 m tall, 0.3 m radius, a 0.3 m step, a 45 degree slope limit.
    CharacterController DefaultController()
    {
        return CharacterController{
            .Radius = 0.3f,
            .Height = 1.8f,
            .StepHeight = 0.3f,
            .MaxSlopeAngle = glm::radians(45.0f),
            .WalkSpeed = 4.0f,
            .RunSpeed = 7.0f,
            .JumpImpulse = 5.0f,
            .AirControl = 0.2f,
        };
    }

    // Drives one capsule tick against a fixed world-up and Earth gravity.
    CharacterMoveResult Advance(PhysicsWorld& world, const Entity entity, const vec3 up,
                                const vec3 desired, const CharacterController& controller)
    {
        return world.UpdateCharacter(entity,
                                     CharacterMoveInput{
                                         .Up = up,
                                         .DesiredPlanarVelocity = desired,
                                         .JumpSpeed = controller.JumpImpulse,
                                         .GravityMagnitude = 9.81f,
                                         .AirControl = controller.AirControl,
                                     },
                                     FixedStep);
    }

    // A SystemContext over a headless Input and never-dereferenced asset/task storage — the
    // CharacterMovementSystem touches neither, reading only the context's Server authority.
    struct ContextStorage
    {
        Input HeadlessInput{nullptr};
        alignas(16) unsigned char AssetsBytes[64]{};
        alignas(16) unsigned char TasksBytes[64]{};

        SystemContext Make()
        {
            return SystemContext{
                .Assets = *reinterpret_cast<AssetManager*>(AssetsBytes),
                .Input = HeadlessInput,
                .Tasks = *reinterpret_cast<TaskSystem*>(TasksBytes),
            };
        }
    };
}

TEST_CASE("A character on a flat floor grounds and walks at WalkSpeed through its state")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    const Unique<Scene> scene = Scene::Create(types);
    scene->SetPhysicsWorld(PhysicsWorld::Create(PhysicsWorldInfo{}));

    // A uniform down field so the character's up resolves to +Y, exactly as a dynamic body's would.
    const Entity field = scene->CreateEntity();
    scene->Add<Transform>(field, Transform{});
    scene->Add<GravitySource>(field, GravitySource{.Kind = GravityKind::Uniform,
                                                   .Direction = vec3(0.0f, -1.0f, 0.0f),
                                                   .Magnitude = 9.81f,
                                                   .Bounds = Region{.HalfExtents = vec3(100.0f)}});

    // A static floor whose top face is at y = 0.
    const Entity floor = scene->CreateEntity();
    scene->Add<Transform>(floor, Transform{.Position = vec3(0.0f, -0.5f, 0.0f)});
    scene->Add<RigidBody>(floor,
                          RigidBody{.Motion = MotionType::Static, .Layer = PhysicsLayer::Static});
    scene->Add<Collider>(
        floor, Collider{.Shape = ColliderShape::Box, .Extents = vec3(20.0f, 0.5f, 20.0f)});

    // The walking character: forward (+Z local) at WalkSpeed, driven through the builtin system.
    const CharacterController controller = DefaultController();
    const Entity pawn = scene->CreateEntity();
    scene->Add<Transform>(pawn, Transform{.Position = vec3(0.0f, 0.1f, 0.0f)});
    scene->Add<CharacterController>(pawn, controller);
    scene->Add<Intent>(pawn, Intent{.Move = vec3(0.0f, 0.0f, 1.0f)});

    ContextStorage storage;
    CharacterMovementSystem characters;

    // Settle onto the floor for a moment, then measure a stretch of steady walking.
    for (u32 tick = 0; tick < 30; ++tick)
    {
        StepPhysics(*scene, FixedStep);
        characters.OnUpdate(*scene, FixedStep, storage.Make());
    }

    const f32 startZ = scene->Get<Transform>(pawn).Position.z;
    constexpr u32 WalkTicks = 90;
    for (u32 tick = 0; tick < WalkTicks; ++tick)
    {
        StepPhysics(*scene, FixedStep);
        characters.OnUpdate(*scene, FixedStep, storage.Make());
    }

    const auto& state = scene->Get<CharacterState>(pawn);
    CHECK(state.Grounded);
    CHECK(state.GroundEntity == floor);
    CHECK(state.Up.y == doctest::Approx(1.0f).epsilon(0.001));
    CHECK(state.PlanarSpeed == doctest::Approx(controller.WalkSpeed).epsilon(0.05));
    CHECK(std::abs(state.VerticalSpeed) < 0.2f);

    const f32 travelled = scene->Get<Transform>(pawn).Position.z - startZ;
    CHECK(travelled ==
          doctest::Approx(controller.WalkSpeed * static_cast<f32>(WalkTicks) * FixedStep)
              .epsilon(0.1));
}

TEST_CASE("A slope within the limit is walkable and one past it blocks the climb")
{
    // A ramp is a static box tilted about Z: local +X maps uphill, so pushing +X climbs it when the
    // slope is walkable and stalls when it is too steep. The up stays world +Y, so the slope angle
    // the controller sees is the tilt itself.
    const auto climbHeight = [](const f32 slopeDegrees, const f32 maxSlopeDegrees) -> f32
    {
        const Unique<PhysicsWorld> world = PhysicsWorld::Create(PhysicsWorldInfo{});
        const quat tilt = glm::angleAxis(glm::radians(slopeDegrees), vec3(0.0f, 0.0f, 1.0f));
        AddStaticBox(*world, 1, dvec3(0.0), tilt, vec3(8.0f, 0.5f, 8.0f));
        world->Step(FixedStep);

        CharacterController controller = DefaultController();
        controller.MaxSlopeAngle = glm::radians(maxSlopeDegrees);
        const Entity pawn{.Index = 2};
        // Feet land where the ramp's top face crosses x = 0.
        const f32 restY = 0.5f / std::cos(glm::radians(slopeDegrees));
        world->CreateCharacter(pawn, controller,
                               PhysicsPose{.Position = dvec3(0.0, restY + 0.2, 0.0)});

        for (u32 tick = 0; tick < 60; ++tick)
        {
            Advance(*world, pawn, vec3(0.0f, 1.0f, 0.0f), vec3(0.0f), controller);
        }
        const f32 settledY =
            static_cast<f32>(world->UpdateCharacter(pawn, {}, FixedStep).Position.y);

        for (u32 tick = 0; tick < 150; ++tick)
        {
            Advance(*world, pawn, vec3(0.0f, 1.0f, 0.0f), vec3(3.0f, 0.0f, 0.0f), controller);
        }
        const f32 endY = static_cast<f32>(world->UpdateCharacter(pawn, {}, FixedStep).Position.y);
        return endY - settledY;
    };

    // 30 degrees under a 45 degree limit: it climbs.
    CHECK(climbHeight(30.0f, 45.0f) > 0.4f);
    // 60 degrees over the same limit: it does not climb (it is blocked and slides back).
    CHECK(climbHeight(60.0f, 45.0f) < 0.1f);
}

TEST_CASE("An obstacle under the step height is stepped and one over it is not")
{
    const auto walkOntoStep = [](const f32 stepBoxHeight) -> vec3
    {
        const Unique<PhysicsWorld> world = PhysicsWorld::Create(PhysicsWorldInfo{});
        // Floor with its top at y = 0, and a raised deck the character walks toward: its edge is at
        // x = 1, its top at stepBoxHeight, and it runs far enough that the character ends on it.
        AddStaticBox(*world, 1, dvec3(0.0, -0.5, 0.0), quat(1.0f, 0.0f, 0.0f, 0.0f),
                     vec3(20.0f, 0.5f, 20.0f));
        AddStaticBox(*world, 2, dvec3(11.0, stepBoxHeight * 0.5, 0.0), quat(1.0f, 0.0f, 0.0f, 0.0f),
                     vec3(10.0f, stepBoxHeight * 0.5f, 5.0f));
        world->Step(FixedStep);

        const CharacterController controller = DefaultController();
        const Entity pawn{.Index = 3};
        world->CreateCharacter(pawn, controller, PhysicsPose{.Position = dvec3(0.0, 0.1, 0.0)});
        for (u32 tick = 0; tick < 30; ++tick)
        {
            Advance(*world, pawn, vec3(0.0f, 1.0f, 0.0f), vec3(0.0f), controller);
        }
        CharacterMoveResult result;
        for (u32 tick = 0; tick < 220; ++tick)
        {
            result =
                Advance(*world, pawn, vec3(0.0f, 1.0f, 0.0f), vec3(2.5f, 0.0f, 0.0f), controller);
        }
        return vec3(result.Position);
    };

    // A 0.2 m block (below the 0.3 m step) is climbed: the character ends on top of it, past x = 1.
    const vec3 stepped = walkOntoStep(0.2f);
    CHECK(stepped.y == doctest::Approx(0.2f).epsilon(0.15));
    CHECK(stepped.x > 1.5f);

    // A 0.6 m block (above the step) blocks: the character stays on the floor, stopped before it.
    const vec3 blocked = walkOntoStep(0.6f);
    CHECK(blocked.y < 0.15f);
    CHECK(blocked.x < 1.0f);
}

TEST_CASE("A character walked a full circuit inside an Axial field keeps a perpendicular inward up")
{
    // A ring habitat: gravity is Axial about +Z, so down is radially outward and up is radially
    // inward everywhere on the inner wall. The wall is a faceted ring of static boxes; the
    // character walks the inside of it a full lap. The property a constant-vector design cannot
    // deliver: the up is exactly perpendicular to the axis and inward at every point, continuously.
    constexpr f32 Radius = 4.0f;
    constexpr u32 Segments = 64;
    constexpr vec3 Axis(0.0f, 0.0f, 1.0f);

    const Unique<PhysicsWorld> world = PhysicsWorld::Create(PhysicsWorldInfo{});
    const f32 arc = glm::two_pi<f32>() * Radius / static_cast<f32>(Segments);
    for (u32 i = 0; i < Segments; ++i)
    {
        const f32 angle = glm::two_pi<f32>() * static_cast<f32>(i) / static_cast<f32>(Segments);
        const vec3 outward(std::cos(angle), std::sin(angle), 0.0f);
        // Each segment's local +Y is the outward radial (its thickness axis), so its inner face
        // sits at Radius facing inward; local +Z stays the ring axis.
        const vec3 tangent(std::sin(angle), -std::cos(angle), 0.0f);
        const mat3 basis(tangent, outward, Axis);
        const quat rotation = glm::quat_cast(basis);
        AddStaticBox(*world, 100 + i, dvec3(outward * (Radius + 0.3f)), rotation,
                     vec3(arc * 0.9f, 0.3f, 1.5f));
    }
    world->Step(FixedStep);

    const GravitySourceInstance source{
        .Kind = GravityKind::Axial,
        .Direction = Axis,
        .Origin = vec3(0.0f),
        .Magnitude = 9.81f,
        .Bounds = Region{.HalfExtents = vec3(100.0f)},
    };
    const std::array<GravitySourceInstance, 1> sources{source};

    CharacterController controller = DefaultController();
    controller.Radius = 0.25f;
    controller.Height = 1.0f;
    controller.StepHeight = 0.2f;
    const Entity pawn{.Index = 1};
    world->CreateCharacter(
        pawn, controller,
        PhysicsPose{.Position = dvec3((Radius - 0.05f), 0.0, 0.0),
                    .Rotation = FromTo(vec3(0.0f, 1.0f, 0.0f), vec3(-1.0f, 0.0f, 0.0f))});

    const auto upAt = [&](const vec3 position)
    { return -glm::normalize(EvaluateGravity(sources, position)); };

    // Seat the capsule against the wall under gravity before it starts to walk.
    vec3 position((Radius - 0.05f), 0.0f, 0.0f);
    for (u32 tick = 0; tick < 40; ++tick)
    {
        const CharacterMoveResult result = world.get()->UpdateCharacter(
            pawn, CharacterMoveInput{.Up = upAt(position), .GravityMagnitude = 9.81f}, FixedStep);
        position = vec3(result.Position);
    }

    // Walk tangentially a full lap, checking the up at every step.
    constexpr f32 Speed = 2.0f;
    vec3 previousUp = upAt(position);
    f32 travelled = 0.0f;
    f32 lastAngle = std::atan2(position.y, position.x);
    bool completedLap = false;
    for (u32 tick = 0; tick < 2000 && !completedLap; ++tick)
    {
        const vec3 up = upAt(position);
        const vec3 outward = -up;
        const vec3 tangent = glm::normalize(glm::cross(Axis, outward));

        const CharacterMoveResult result =
            world->UpdateCharacter(pawn,
                                   CharacterMoveInput{
                                       .Up = up,
                                       .DesiredPlanarVelocity = tangent * Speed,
                                       .GravityMagnitude = 9.81f,
                                       .AirControl = controller.AirControl,
                                   },
                                   FixedStep);
        position = vec3(result.Position);

        const vec3 radial = glm::normalize(vec3(position.x, position.y, 0.0f));
        // Grounded on the inner wall, up perpendicular to the axis and pointing inward.
        CHECK(result.Grounded);
        CHECK(std::abs(glm::dot(up, Axis)) < 1e-3f);
        CHECK(glm::dot(up, radial) < -0.999f);
        // No discontinuity: the up turns by a small amount each step, never jumps.
        CHECK(glm::dot(up, previousUp) > 0.99f);
        previousUp = up;

        const f32 angle = std::atan2(position.y, position.x);
        f32 delta = angle - lastAngle;
        if (delta > glm::pi<f32>())
        {
            delta -= glm::two_pi<f32>();
        }
        else if (delta < -glm::pi<f32>())
        {
            delta += glm::two_pi<f32>();
        }
        travelled += std::abs(delta);
        lastAngle = angle;
        completedLap = travelled >= glm::two_pi<f32>();
    }
    CHECK(completedLap);
}

TEST_CASE("A character on a translating and rotating kinematic platform stays put relative to it")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    const Unique<Scene> scene = Scene::Create(types);
    scene->SetPhysicsWorld(PhysicsWorld::Create(PhysicsWorldInfo{}));
    PhysicsWorld& world = *scene->GetPhysicsWorld();

    // A kinematic platform whose top face starts at y = 0.25.
    const Entity platform = scene->CreateEntity();
    scene->Add<Transform>(platform, Transform{});
    scene->Add<RigidBody>(
        platform, RigidBody{.Motion = MotionType::Kinematic, .Layer = PhysicsLayer::Moving});
    scene->Add<Collider>(platform,
                         Collider{.Shape = ColliderShape::Box, .Extents = vec3(3.0f, 0.25f, 3.0f)});

    // Create the platform body and its broad-phase entry.
    for (u32 tick = 0; tick < 5; ++tick)
    {
        StepPhysics(*scene, FixedStep);
    }

    const CharacterController controller = DefaultController();
    const Entity pawn{.Index = 999};
    world.CreateCharacter(pawn, controller, PhysicsPose{.Position = dvec3(1.0, 0.30, 0.5)});

    // Seat the character on the stationary platform.
    for (u32 tick = 0; tick < 40; ++tick)
    {
        StepPhysics(*scene, FixedStep);
        Advance(world, pawn, vec3(0.0f, 1.0f, 0.0f), vec3(0.0f), controller);
    }

    // The character's position in the platform's frame at rest, which must hold as it moves.
    const CharacterMoveResult seated = world.UpdateCharacter(pawn, {}, FixedStep);
    const auto& seatTransform = scene->Get<Transform>(platform);
    const vec3 initialOffset =
        glm::inverse(seatTransform.Rotation) * (vec3(seated.Position) - seatTransform.Position);

    constexpr f32 Omega = 0.3f;
    constexpr vec3 Velocity(0.3f, 0.0f, 0.0f);
    f32 elapsed = 0.0f;
    vec3 charPosition(seated.Position);
    for (u32 tick = 0; tick < 1000; ++tick)
    {
        elapsed += FixedStep;
        auto& transform = scene->Get<Transform>(platform);
        transform.Position = Velocity * elapsed;
        transform.Rotation = glm::angleAxis(Omega * elapsed, vec3(0.0f, 1.0f, 0.0f));

        StepPhysics(*scene, FixedStep);
        const CharacterMoveResult result =
            Advance(world, pawn, vec3(0.0f, 1.0f, 0.0f), vec3(0.0f), controller);
        charPosition = vec3(result.Position);
    }

    const auto& endTransform = scene->Get<Transform>(platform);
    const vec3 endOffset =
        glm::inverse(endTransform.Rotation) * (charPosition - endTransform.Position);
    CHECK(glm::length(endOffset - initialOffset) < 0.3f);
}

TEST_CASE("A character reached by no gravity source keeps its up and does not tumble or assert")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    const Unique<Scene> scene = Scene::Create(types);
    scene->SetPhysicsWorld(PhysicsWorld::Create(PhysicsWorldInfo{}));

    // No gravity source anywhere: EvaluateGravity is zero, so the character is in free-fall.
    const CharacterController controller = DefaultController();
    const Entity pawn = scene->CreateEntity();
    scene->Add<Transform>(pawn, Transform{.Position = vec3(0.0f, 5.0f, 0.0f)});
    scene->Add<CharacterController>(pawn, controller);
    // A steady forward drift, to prove it floats rather than falls or spins.
    scene->Add<Intent>(pawn, Intent{.Move = vec3(0.0f, 0.0f, 1.0f)});

    ContextStorage storage;
    CharacterMovementSystem characters;
    for (u32 tick = 0; tick < 200; ++tick)
    {
        characters.OnUpdate(*scene, FixedStep, storage.Make());
    }

    const auto& state = scene->Get<CharacterState>(pawn);
    // The up is retained (world +Y, its seed), not divided by zero or drifted.
    CHECK(!state.Grounded);
    CHECK(state.Up.y == doctest::Approx(1.0f).epsilon(0.001));
    CHECK(state.GroundEntity == Entity::Null);
    // The capsule did not tumble: its own up still matches the resolved up.
    const vec3 bodyUp = scene->Get<Transform>(pawn).Rotation * vec3(0.0f, 1.0f, 0.0f);
    CHECK(glm::dot(bodyUp, vec3(0.0f, 1.0f, 0.0f)) > 0.999f);
}
