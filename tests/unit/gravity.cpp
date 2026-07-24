// Gravity fields: the pure EvaluateGravity core (Uniform/Radial/Axial shapes, priority and
// blend-band composition, the no-source-is-zero rule) with no solver attached, plus one
// integration case that a dynamic body released in an Axial field falls along the local radial and
// rests against a wall perpendicular to it. Pure CPU — a headless Scene, no Context, no GPU.

#include <doctest/doctest.h>

#include <Veng/Physics/Components.h>
#include <Veng/Physics/Gravity.h>
#include <Veng/Physics/PhysicsSystem.h>
#include <Veng/Physics/PhysicsWorld.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

#include <array>

using namespace Veng;

namespace
{
    // A box region centred at the origin, large enough to contain every sample below.
    Region WideBox()
    {
        return Region{.Shape = RegionShape::Box, .HalfExtents = vec3(100.0f)};
    }

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
            // Antiparallel: rotate a half turn about any axis perpendicular to `a`.
            const vec3 axis = glm::normalize(glm::cross(
                a, std::abs(a.x) < 0.9f ? vec3(1.0f, 0.0f, 0.0f) : vec3(0.0f, 1.0f, 0.0f)));
            return glm::angleAxis(glm::pi<f32>(), axis);
        }
        return glm::angleAxis(std::acos(dot), glm::normalize(glm::cross(a, b)));
    }
}

TEST_CASE("a Uniform source returns its down vector everywhere inside its region")
{
    const std::array<GravitySourceInstance, 1> sources{GravitySourceInstance{
        .Kind = GravityKind::Uniform,
        .Direction = vec3(0.0f, -1.0f, 0.0f),
        .Magnitude = 9.81f,
        .Bounds = WideBox(),
    }};

    for (const vec3 point : {vec3(0.0f), vec3(3.0f, -2.0f, 7.0f), vec3(-9.0f, 40.0f, 1.0f)})
    {
        const vec3 gravity = EvaluateGravity(sources, point);
        CHECK(gravity.x == doctest::Approx(0.0f));
        CHECK(gravity.y == doctest::Approx(-9.81f));
        CHECK(gravity.z == doctest::Approx(0.0f));
    }

    // Outside the region there is no source, and no source is zero — not a default down vector.
    const vec3 outside = EvaluateGravity(sources, vec3(0.0f, 500.0f, 0.0f));
    CHECK(glm::length(outside) == doctest::Approx(0.0f));
}

TEST_CASE("a Radial source points at its origin and ramps between inner and outer radius")
{
    const std::array<GravitySourceInstance, 1> sources{GravitySourceInstance{
        .Kind = GravityKind::Radial,
        .Origin = vec3(0.0f),
        .Magnitude = 9.81f,
        .InnerRadius = 2.0f,
        .OuterRadius = 5.0f,
        .Bounds = Region{.Shape = RegionShape::Sphere, .HalfExtents = vec3(100.0f)},
    }};

    // Beyond the outer radius the field is at full strength and points straight at the origin.
    const vec3 far = EvaluateGravity(sources, vec3(10.0f, 0.0f, 0.0f));
    CHECK(far.x == doctest::Approx(-9.81f));
    CHECK(far.y == doctest::Approx(0.0f));
    CHECK(far.z == doctest::Approx(0.0f));

    // Halfway through the ramp (radius 3.5 of the [2,5] band) the strength is halved.
    const vec3 mid = EvaluateGravity(sources, vec3(0.0f, 0.0f, 3.5f));
    CHECK(glm::length(mid) == doctest::Approx(9.81f * 0.5f).epsilon(0.001));
    CHECK(glm::normalize(mid).z == doctest::Approx(-1.0f));

    // Inside the inner radius the core is weightless.
    const vec3 core = EvaluateGravity(sources, vec3(1.0f, 0.0f, 0.0f));
    CHECK(glm::length(core) == doctest::Approx(0.0f));
}

TEST_CASE("an Axial source is exactly perpendicular and outward at 64 points round a circle")
{
    // The continuity property the whole design exists for: about a spin axis, "down" is radially
    // outward and perpendicular to the axis everywhere, with a closed form rather than a stack of
    // constant-vector volumes.
    constexpr f32 Magnitude = 8.0f;
    const vec3 axis = glm::normalize(vec3(0.0f, 1.0f, 0.0f));
    const std::array<GravitySourceInstance, 1> sources{GravitySourceInstance{
        .Kind = GravityKind::Axial,
        .Direction = axis,
        .Origin = vec3(0.0f),
        .Magnitude = Magnitude,
        .Bounds = Region{.Shape = RegionShape::Cylinder, .HalfExtents = vec3(100.0f, 100.0f, 0.0f)},
    }};

    constexpr u32 Samples = 64;
    for (u32 i = 0; i < Samples; ++i)
    {
        const f32 angle = glm::two_pi<f32>() * static_cast<f32>(i) / static_cast<f32>(Samples);
        const f32 radius = 6.0f;
        const vec3 outward(std::cos(angle), 0.0f, std::sin(angle));
        const vec3 point = outward * radius + axis * 3.0f; // offset along the axis too

        const vec3 gravity = EvaluateGravity(sources, point);

        // Exactly perpendicular to the axis.
        CHECK(glm::dot(gravity, axis) == doctest::Approx(0.0f).epsilon(0.0001));
        // Exactly radially outward, at full strength.
        CHECK(glm::length(gravity) == doctest::Approx(Magnitude));
        CHECK(glm::dot(glm::normalize(gravity), outward) == doctest::Approx(1.0f));
    }
}

TEST_CASE("the highest-priority source containing a point wins where regions overlap")
{
    const std::array<GravitySourceInstance, 2> sources{
        GravitySourceInstance{
            .Kind = GravityKind::Uniform,
            .Direction = vec3(0.0f, -1.0f, 0.0f),
            .Magnitude = 9.81f,
            .Bounds = WideBox(),
            .Priority = 10,
        },
        GravitySourceInstance{
            .Kind = GravityKind::Uniform,
            .Direction = vec3(1.0f, 0.0f, 0.0f),
            .Magnitude = 5.0f,
            .Bounds = WideBox(),
            .Priority = 5,
        },
    };

    const vec3 gravity = EvaluateGravity(sources, vec3(0.0f));
    CHECK(gravity.x == doctest::Approx(0.0f));
    CHECK(gravity.y == doctest::Approx(-9.81f));
    CHECK(gravity.z == doctest::Approx(0.0f));
}

TEST_CASE("a point in a source's blend band interpolates against the next source down")
{
    // A high-priority box with a 2 m blend band over a low-priority box that covers everything.
    const std::array<GravitySourceInstance, 2> sources{
        GravitySourceInstance{
            .Kind = GravityKind::Uniform,
            .Direction = vec3(0.0f, -1.0f, 0.0f),
            .Magnitude = 10.0f,
            .Bounds = Region{.Shape = RegionShape::Box, .HalfExtents = vec3(5.0f)},
            .Priority = 10,
            .BlendWidth = 2.0f,
        },
        GravitySourceInstance{
            .Kind = GravityKind::Uniform,
            .Direction = vec3(1.0f, 0.0f, 0.0f),
            .Magnitude = 4.0f,
            .Bounds = WideBox(),
            .Priority = 5,
        },
    };

    // At x = 4 the point is 1 m inside the high-priority box's boundary — half of its 2 m band — so
    // it is half the high source and half the one below.
    const vec3 gravity = EvaluateGravity(sources, vec3(4.0f, 0.0f, 0.0f));
    CHECK(gravity.x == doctest::Approx(2.0f));  // 0.5 * 4
    CHECK(gravity.y == doctest::Approx(-5.0f)); // 0.5 * -10
    CHECK(gravity.z == doctest::Approx(0.0f));
}

TEST_CASE("a point reached by no source is weightless, not defaulted")
{
    const std::array<GravitySourceInstance, 1> sources{GravitySourceInstance{
        .Kind = GravityKind::Uniform,
        .Direction = vec3(0.0f, -1.0f, 0.0f),
        .Magnitude = 9.81f,
        .Bounds =
            Region{.Shape = RegionShape::Box, .Center = vec3(0.0f), .HalfExtents = vec3(1.0f)},
    }};

    const vec3 gravity = EvaluateGravity(sources, vec3(50.0f, 0.0f, 0.0f));
    CHECK(glm::length(gravity) == doctest::Approx(0.0f));

    // The empty set is likewise zero.
    CHECK(glm::length(EvaluateGravity({}, vec3(0.0f))) == doctest::Approx(0.0f));
}

TEST_CASE("Contains tests a point against a region's shape and orientation")
{
    const Region box{.Shape = RegionShape::Box, .HalfExtents = vec3(2.0f, 1.0f, 3.0f)};
    CHECK(Contains(box, vec3(1.9f, 0.9f, -2.9f)));
    CHECK_FALSE(Contains(box, vec3(2.1f, 0.0f, 0.0f)));

    const Region sphere{.Shape = RegionShape::Sphere, .HalfExtents = vec3(4.0f)};
    CHECK(Contains(sphere, vec3(0.0f, 3.9f, 0.0f)));
    CHECK_FALSE(Contains(sphere, vec3(0.0f, 4.1f, 0.0f)));

    const Region cylinder{.Shape = RegionShape::Cylinder, .HalfExtents = vec3(2.0f, 5.0f, 0.0f)};
    CHECK(Contains(cylinder, vec3(1.9f, 4.9f, 0.0f)));
    CHECK_FALSE(Contains(cylinder, vec3(0.0f, 5.1f, 0.0f))); // past the half height
    CHECK_FALSE(Contains(cylinder, vec3(2.1f, 0.0f, 0.0f))); // past the radius
}

TEST_CASE("a dynamic body released in an Axial field falls outward and rests on the wall")
{
    // Axis is world +Y through the origin; "down" at any point is radially outward. At eight angles
    // round the axis, a box released near the axis should fall outward along that radial and settle
    // against a wall perpendicular to it — the property the field exists to make continuous.
    constexpr f32 FixedStep = 1.0f / 60.0f;
    constexpr f32 WallRadius = 6.0f;
    constexpr f32 WallHalfThickness = 0.5f;
    constexpr f32 BoxHalf = 0.5f;
    const vec3 axis(0.0f, 1.0f, 0.0f);

    constexpr u32 Angles = 8;
    for (u32 i = 0; i < Angles; ++i)
    {
        const f32 angle = glm::two_pi<f32>() * static_cast<f32>(i) / static_cast<f32>(Angles);
        const vec3 outward(std::cos(angle), 0.0f, std::sin(angle));

        TypeRegistry types;
        RegisterBuiltinTypes(types);
        const Unique<Scene> scene = Scene::Create(types);
        scene->SetPhysicsWorld(PhysicsWorld::Create(PhysicsWorldInfo{}));

        // The Axial field.
        const Entity field = scene->CreateEntity();
        scene->Add<Transform>(field, Transform{});
        scene->Add<GravitySource>(field,
                                  GravitySource{
                                      .Kind = GravityKind::Axial,
                                      .Direction = axis,
                                      .Magnitude = 9.81f,
                                      .Bounds = Region{.Shape = RegionShape::Cylinder,
                                                       .HalfExtents = vec3(50.0f, 50.0f, 0.0f)},
                                  });

        // A static wall at WallRadius, its inner face perpendicular to the radial (its local +Y,
        // the thin axis, turned to face inward along -outward).
        const Entity wall = scene->CreateEntity();
        scene->Add<Transform>(wall,
                              Transform{.Position = outward * WallRadius,
                                        .Rotation = FromTo(vec3(0.0f, 1.0f, 0.0f), -outward)});
        scene->Add<RigidBody>(
            wall, RigidBody{.Motion = MotionType::Static, .Layer = PhysicsLayer::Static});
        scene->Add<Collider>(wall, Collider{.Shape = ColliderShape::Box,
                                            .Extents = vec3(6.0f, WallHalfThickness, 6.0f)});

        // A dynamic box released near the axis, on the same radial.
        const Entity box = scene->CreateEntity();
        scene->Add<Transform>(box, Transform{.Position = outward * 2.0f});
        scene->Add<RigidBody>(box, RigidBody{.Motion = MotionType::Dynamic, .Mass = 5.0f});
        scene->Add<Collider>(box, Collider{.Shape = ColliderShape::Box, .Extents = vec3(BoxHalf)});

        for (u32 step = 0; step < 240; ++step)
        {
            StepPhysics(*scene, FixedStep);
        }

        const vec3 rest = scene->Get<Transform>(box).Position;
        const f32 restRadius = glm::length(vec2(rest.x, rest.z));
        // It travelled outward to the wall's inner face: WallRadius - thickness - the box half.
        CHECK(restRadius ==
              doctest::Approx(WallRadius - WallHalfThickness - BoxHalf).epsilon(0.05));
        // It fell along the radial, not sideways: still on the outward line, no axial drift.
        const f32 tangential = glm::length(rest - outward * restRadius - axis * rest.y);
        CHECK(tangential == doctest::Approx(0.0f).epsilon(0.02));
        CHECK(rest.y == doctest::Approx(0.0f).epsilon(0.05));
    }
}
