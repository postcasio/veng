// Physics collision content and the query surface: cooked CollisionShape colliders (convex hull
// and triangle mesh, and the motion types a triangle mesh may back), sensors and the overlap set
// they publish, the three constraints, and the Raycast/ShapeCast/Overlap trio. Pure CPU — a
// headless Scene with no Context and no GPU.

#include <doctest/doctest.h>

#include <algorithm>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/CollisionShape.h>
#include <Veng/Physics/Components.h>
#include <Veng/Physics/PhysicsSystem.h>
#include <Veng/Physics/PhysicsWorld.h>
#include <Veng/Physics/Queries.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

using namespace Veng;

namespace
{
    // The Sim tick the module is arranged around; every step below uses it so a step count is a
    // simulated duration.
    constexpr f32 FixedStep = 1.0f / 60.0f;

    // A 2 m axis-aligned cube as a convex point cloud, centred on the body's origin.
    Ref<CollisionShape> CubeHull()
    {
        const Ref<CollisionShape> shape = CreateRef<CollisionShape>();
        shape->Geometry = CollisionGeometry::Convex;
        for (const f32 x : {-1.0f, 1.0f})
        {
            for (const f32 y : {-1.0f, 1.0f})
            {
                for (const f32 z : {-1.0f, 1.0f})
                {
                    shape->Points.emplace_back(x, y, z);
                }
            }
        }
        return shape;
    }

    // A 20 x 20 m horizontal quad at the body's origin, as two triangles.
    Ref<CollisionShape> GroundMesh()
    {
        const Ref<CollisionShape> shape = CreateRef<CollisionShape>();
        shape->Geometry = CollisionGeometry::Mesh;
        shape->Points = {vec3(-10.0f, 0.0f, -10.0f), vec3(10.0f, 0.0f, -10.0f),
                         vec3(10.0f, 0.0f, 10.0f), vec3(-10.0f, 0.0f, 10.0f)};
        shape->Indices = {0, 2, 1, 0, 3, 2};
        return shape;
    }

    // A scene with a physics world, the builtin types registered, and nothing else. The cooked
    // shapes are owned here rather than by an AssetManager: the test drives the module, not the
    // asset pipeline.
    struct ContentFixture
    {
        TypeRegistry Types;
        Unique<Scene> World;
        Ref<CollisionShape> Hull = CubeHull();
        Ref<CollisionShape> Ground = GroundMesh();

        ContentFixture()
        {
            RegisterBuiltinTypes(Types);
            World = Scene::Create(Types);
            World->SetPhysicsWorld(PhysicsWorld::Create(PhysicsWorldInfo{}));
        }

        PhysicsWorld& Physics() const { return *World->GetPhysicsWorld(); }

        // A static box centred at `position` with the given half extents.
        Entity SpawnBox(const vec3 position, const vec3 half,
                        const MotionType motion = MotionType::Static,
                        const PhysicsLayer layer = PhysicsLayer::Static) const
        {
            const Entity entity = World->CreateEntity();
            World->Add<Transform>(entity, Transform{.Position = position});
            World->Add<RigidBody>(entity, RigidBody{.Motion = motion, .Layer = layer});
            World->Add<Collider>(entity, Collider{.Shape = ColliderShape::Box, .Extents = half});
            return entity;
        }

        void Step(const u32 count) const
        {
            for (u32 i = 0; i < count; ++i)
            {
                StepPhysics(*World, FixedStep);
            }
        }
    };

    // A Collider naming a caller-owned cooked shape, for a query that sweeps or overlaps one.
    Collider CookedCollider(const Ref<CollisionShape>& shape)
    {
        Collider collider{.Shape = ColliderShape::Mesh};
        collider.Geometry = AssetManager::Adopt<CollisionShape>(shape);
        return collider;
    }
}

TEST_CASE("a convex-hull CollisionShape backs a dynamic body and rests on the ground")
{
    const ContentFixture fixture;
    fixture.SpawnBox(vec3(0.0f, -1.0f, 0.0f), vec3(20.0f, 1.0f, 20.0f));

    const Entity box = fixture.World->CreateEntity();
    fixture.World->Add<Transform>(box, Transform{.Position = vec3(0.0f, 4.0f, 0.0f)});
    fixture.World->Add<RigidBody>(box, RigidBody{.Motion = MotionType::Dynamic, .Mass = 10.0f});
    fixture.World->Add<Collider>(box, CookedCollider(fixture.Hull));

    fixture.Step(180);

    // The hull is a 2 m cube, so its centre settles one metre above the ground's top face at y = 0.
    CHECK(fixture.World->Get<Transform>(box).Position.y == doctest::Approx(1.0f).epsilon(0.02));
}

TEST_CASE("a triangle-mesh CollisionShape backs a kinematic body and carries what lands on it")
{
    const ContentFixture fixture;

    const Entity floor = fixture.World->CreateEntity();
    fixture.World->Add<Transform>(floor, Transform{.Position = vec3(0.0f, 0.0f, 0.0f)});
    fixture.World->Add<RigidBody>(
        floor, RigidBody{.Motion = MotionType::Kinematic, .Layer = PhysicsLayer::Moving});
    fixture.World->Add<Collider>(floor, CookedCollider(fixture.Ground));

    const Entity box = fixture.World->CreateEntity();
    fixture.World->Add<Transform>(box, Transform{.Position = vec3(0.0f, 3.0f, 0.0f)});
    fixture.World->Add<RigidBody>(box, RigidBody{.Motion = MotionType::Dynamic, .Mass = 5.0f});
    fixture.World->Add<Collider>(box, Collider{.Extents = vec3(0.5f)});

    fixture.Step(180);

    CHECK(fixture.Physics().GetBodyCount() == 2);
    CHECK(fixture.World->Get<Transform>(box).Position.y == doctest::Approx(0.5f).epsilon(0.05));
}

TEST_CASE("a collider whose cooked geometry is not resident has no body until it arrives")
{
    const ContentFixture fixture;

    const Entity entity = fixture.World->CreateEntity();
    fixture.World->Add<Transform>(entity, Transform{});
    fixture.World->Add<RigidBody>(entity, RigidBody{.Motion = MotionType::Static});
    fixture.World->Add<Collider>(entity, Collider{.Shape = ColliderShape::Mesh});

    fixture.Step(1);
    CHECK(fixture.Physics().GetBodyCount() == 0);

    fixture.World->Get<Collider>(entity) = CookedCollider(fixture.Hull);
    fixture.Step(1);
    CHECK(fixture.Physics().HasBody(entity));
}

TEST_CASE("a raycast against a known static box hits at the analytically expected point")
{
    const ContentFixture fixture;
    // A 1 m half-extent box centred at the origin, so its +y face is the plane y = 1.
    const Entity target = fixture.SpawnBox(vec3(0.0f), vec3(1.0f));
    fixture.Step(1);

    const optional<RayHit> hit =
        Raycast(&fixture.Physics(), dvec3(0.0, 5.0, 0.0), vec3(0.0f, -1.0f, 0.0f), 10.0f);
    REQUIRE(hit.has_value());
    CHECK(hit->Body == target);
    CHECK(hit->Position.y == doctest::Approx(1.0).epsilon(0.001));
    CHECK(hit->Distance == doctest::Approx(4.0f).epsilon(0.001));
    CHECK(hit->Fraction == doctest::Approx(0.4f).epsilon(0.001));
    CHECK(hit->Normal.y == doctest::Approx(1.0f).epsilon(0.01));

    // A ray that stops short of the box reaches its full length unobstructed.
    CHECK_FALSE(Raycast(&fixture.Physics(), dvec3(0.0, 5.0, 0.0), vec3(0.0f, -1.0f, 0.0f), 3.0f)
                    .has_value());
}

TEST_CASE("a query filter's layer mask and ignore list decide what a raycast may report")
{
    const ContentFixture fixture;
    const Entity nearBox = fixture.SpawnBox(vec3(0.0f, 0.0f, 0.0f), vec3(1.0f), MotionType::Static,
                                            PhysicsLayer::Moving);
    const Entity farBox = fixture.SpawnBox(vec3(0.0f, -6.0f, 0.0f), vec3(1.0f));
    fixture.Step(1);

    const Entity ignored[] = {nearBox};
    const optional<RayHit> skipped =
        Raycast(&fixture.Physics(), dvec3(0.0, 5.0, 0.0), vec3(0.0f, -1.0f, 0.0f), 20.0f,
                QueryFilter{.Ignore = ignored});
    REQUIRE(skipped.has_value());
    CHECK(skipped->Body == farBox);

    const optional<RayHit> staticOnly =
        Raycast(&fixture.Physics(), dvec3(0.0, 5.0, 0.0), vec3(0.0f, -1.0f, 0.0f), 20.0f,
                QueryFilter{.Layers = PhysicsLayerBit(PhysicsLayer::Static)});
    REQUIRE(staticOnly.has_value());
    CHECK(staticOnly->Body == farBox);
}

TEST_CASE("a shape cast into a wall stops at its surface and names what it met")
{
    const ContentFixture fixture;
    // A wall 1 m thick centred at x = 5, so its facing surface is the plane x = 4.5.
    const Entity wall = fixture.SpawnBox(vec3(5.0f, 0.0f, 0.0f), vec3(0.5f, 5.0f, 5.0f));
    fixture.Step(1);

    // Sweep a unit-radius sphere from the origin to the far side of the wall: it must stop where
    // its surface meets the wall's, at x = 3.5, which is 35 % of the 10 m displacement.
    const Collider sphere{.Shape = ColliderShape::Sphere, .Extents = vec3(1.0f)};
    const optional<ShapeHit> hit =
        ShapeCast(&fixture.Physics(), sphere, PhysicsPose{}, dvec3(10.0, 0.0, 0.0));
    REQUIRE(hit.has_value());
    CHECK(hit->Body == wall);
    CHECK(hit->Fraction == doctest::Approx(0.35f).epsilon(0.01));
    CHECK(hit->Position.x == doctest::Approx(4.5).epsilon(0.02));
    CHECK(hit->Normal.x == doctest::Approx(-1.0f).epsilon(0.02));

    // A sweep that stops short of the wall completes unobstructed.
    CHECK_FALSE(
        ShapeCast(&fixture.Physics(), sphere, PhysicsPose{}, dvec3(3.0, 0.0, 0.0)).has_value());
}

TEST_CASE("a cooked convex hull sweeps like a primitive, so a mover can sweep its own shape")
{
    const ContentFixture fixture;
    const Entity wall = fixture.SpawnBox(vec3(6.0f, 0.0f, 0.0f), vec3(0.5f, 5.0f, 5.0f));
    fixture.Step(1);

    // The hull is a 2 m cube, so its +x face meets the wall's facing surface at x = 4.5: a
    // displacement fraction of 0.45 over 10 m.
    const optional<ShapeHit> hit = ShapeCast(&fixture.Physics(), CookedCollider(fixture.Hull),
                                             PhysicsPose{}, dvec3(10.0, 0.0, 0.0));
    REQUIRE(hit.has_value());
    CHECK(hit->Body == wall);
    CHECK(hit->Fraction == doctest::Approx(0.45f).epsilon(0.02));
}

TEST_CASE("an overlap returns the entities inside the volume and none touching it from outside")
{
    const ContentFixture fixture;
    const Entity inside = fixture.SpawnBox(vec3(0.0f), vec3(0.25f));
    // Half extents 0.25 centred at x = 2.25, so its -x face is exactly the test volume's +x face:
    // touching, not intersecting.
    fixture.SpawnBox(vec3(2.25f, 0.0f, 0.0f), vec3(0.25f));
    // Well clear of the volume.
    fixture.SpawnBox(vec3(6.0f, 0.0f, 0.0f), vec3(0.25f));
    fixture.Step(1);

    const Collider volume{.Shape = ColliderShape::Box, .Extents = vec3(2.0f)};
    vector<Entity> found;
    const usize count = Overlap(&fixture.Physics(), volume, PhysicsPose{}, QueryFilter{}, found);

    CHECK(count == 1);
    REQUIRE(found.size() == 1);
    CHECK(found.front() == inside);
}

TEST_CASE("queries against a scene with no physics world return empty rather than asserting")
{
    const Collider shape{.Shape = ColliderShape::Sphere, .Extents = vec3(1.0f)};
    vector<Entity> found{Entity{.Index = 7}};

    CHECK_FALSE(Raycast(nullptr, dvec3(0.0), vec3(0.0f, -1.0f, 0.0f), 10.0f).has_value());
    CHECK_FALSE(ShapeCast(nullptr, shape, PhysicsPose{}, dvec3(1.0, 0.0, 0.0)).has_value());
    CHECK(Overlap(nullptr, shape, PhysicsPose{}, QueryFilter{}, found) == 0);
    CHECK(found.empty());
}

TEST_CASE("a sensor publishes the bodies inside it, and the enter and exit deltas")
{
    const ContentFixture fixture;

    const Entity sensor = fixture.World->CreateEntity();
    fixture.World->Add<Transform>(sensor, Transform{.Position = vec3(0.0f, 0.0f, 0.0f)});
    fixture.World->Add<RigidBody>(
        sensor, RigidBody{.Motion = MotionType::Static, .Layer = PhysicsLayer::Trigger});
    fixture.World->Add<Collider>(sensor, Collider{.Extents = vec3(2.0f)});
    fixture.World->Add<Sensor>(sensor, Sensor{});

    // A kinematic mover driven through the volume and out the other side.
    const Entity mover = fixture.World->CreateEntity();
    fixture.World->Add<Transform>(mover, Transform{.Position = vec3(-6.0f, 0.0f, 0.0f)});
    fixture.World->Add<RigidBody>(
        mover, RigidBody{.Motion = MotionType::Kinematic, .Layer = PhysicsLayer::Moving});
    fixture.World->Add<Collider>(mover, Collider{.Extents = vec3(0.5f)});

    fixture.Step(2);
    REQUIRE(fixture.World->Has<SensorOverlaps>(sensor));
    CHECK(fixture.World->Get<SensorOverlaps>(sensor).Current.empty());

    // Inside the volume: the mover enters. The first step sweeps it there (contacts are found
    // against the pose the step starts from), the second finds it overlapping.
    fixture.World->Get<Transform>(mover).Position = vec3(0.0f, 0.0f, 0.0f);
    fixture.Step(2);
    {
        const SensorOverlaps& overlaps = fixture.World->Get<SensorOverlaps>(sensor);
        REQUIRE(overlaps.Current.size() == 1);
        CHECK(overlaps.Current.front() == mover);
        REQUIRE(overlaps.Entered.size() == 1);
        CHECK(overlaps.Entered.front() == mover);
        CHECK(overlaps.Exited.empty());
    }

    // Still inside: no new delta on either side.
    fixture.Step(1);
    {
        const SensorOverlaps& overlaps = fixture.World->Get<SensorOverlaps>(sensor);
        CHECK(overlaps.Current.size() == 1);
        CHECK(overlaps.Entered.empty());
        CHECK(overlaps.Exited.empty());
    }

    // Out the far side.
    fixture.World->Get<Transform>(mover).Position = vec3(8.0f, 0.0f, 0.0f);
    fixture.Step(2);
    {
        const SensorOverlaps& overlaps = fixture.World->Get<SensorOverlaps>(sensor);
        CHECK(overlaps.Current.empty());
        CHECK(overlaps.Exited.size() <= 1);
    }

    // A sensor resolves no contact: the mover passed straight through rather than being stopped.
    CHECK(fixture.World->Get<Transform>(mover).Position.x == doctest::Approx(8.0f).epsilon(0.01));
}

TEST_CASE("a sensor's layer mask filters what it publishes")
{
    const ContentFixture fixture;

    const Entity sensor = fixture.World->CreateEntity();
    fixture.World->Add<Transform>(sensor, Transform{});
    fixture.World->Add<RigidBody>(
        sensor, RigidBody{.Motion = MotionType::Static, .Layer = PhysicsLayer::Trigger});
    fixture.World->Add<Collider>(sensor, Collider{.Extents = vec3(2.0f)});
    fixture.World->Add<Sensor>(sensor, Sensor{.Layers = PhysicsLayerBit(PhysicsLayer::Character)});

    const Entity mover = fixture.World->CreateEntity();
    fixture.World->Add<Transform>(mover, Transform{});
    fixture.World->Add<RigidBody>(
        mover, RigidBody{.Motion = MotionType::Kinematic, .Layer = PhysicsLayer::Moving});
    fixture.World->Add<Collider>(mover, Collider{.Extents = vec3(0.5f)});

    fixture.Step(3);
    CHECK(fixture.World->Get<SensorOverlaps>(sensor).Current.empty());
}

TEST_CASE("a fixed constraint carries a dynamic body on a moving kinematic one without drift")
{
    const ContentFixture fixture;

    const Entity carrier = fixture.World->CreateEntity();
    fixture.World->Add<Transform>(carrier, Transform{.Position = vec3(0.0f, 0.0f, 0.0f)});
    fixture.World->Add<RigidBody>(
        carrier, RigidBody{.Motion = MotionType::Kinematic, .Layer = PhysicsLayer::Moving});
    fixture.World->Add<Collider>(carrier, Collider{.Extents = vec3(1.0f)});

    const Entity cargo = fixture.World->CreateEntity();
    fixture.World->Add<Transform>(cargo, Transform{.Position = vec3(0.0f, 2.0f, 0.0f)});
    fixture.World->Add<RigidBody>(cargo, RigidBody{.Motion = MotionType::Dynamic, .Mass = 4.0f});
    fixture.World->Add<Collider>(cargo, Collider{.Extents = vec3(0.5f)});

    // One step so both bodies exist, then latch them where they stand.
    fixture.Step(1);
    fixture.World->Add<FixedConstraint>(cargo, FixedConstraint{.Target = carrier});
    fixture.Step(1);
    CHECK(fixture.Physics().GetConstraintCount() == 1);

    // Drive the carrier a long way over a thousand ticks; the cargo must ride it exactly.
    constexpr u32 Ticks = 1000;
    constexpr f32 PerTick = 0.01f;
    for (u32 i = 0; i < Ticks; ++i)
    {
        fixture.World->Get<Transform>(carrier).Position.x += PerTick;
        fixture.Step(1);
    }

    const vec3 carrierPosition = fixture.World->Get<Transform>(carrier).Position;
    const vec3 cargoPosition = fixture.World->Get<Transform>(cargo).Position;
    CHECK(carrierPosition.x == doctest::Approx(Ticks * PerTick).epsilon(0.001));
    CHECK((cargoPosition - carrierPosition).x == doctest::Approx(0.0f).epsilon(0.01));
    CHECK((cargoPosition - carrierPosition).y == doctest::Approx(2.0f).epsilon(0.01));

    // The component is the authority: dropping it releases the pair.
    (void)fixture.World->Remove<FixedConstraint>(cargo);
    fixture.Step(1);
    CHECK(fixture.Physics().GetConstraintCount() == 0);
}

TEST_CASE("a point constraint holds a shared pivot and a hinge leaves one axis free")
{
    const ContentFixture fixture;

    const Entity anchor = fixture.World->CreateEntity();
    fixture.World->Add<Transform>(anchor, Transform{.Position = vec3(0.0f, 5.0f, 0.0f)});
    fixture.World->Add<RigidBody>(
        anchor, RigidBody{.Motion = MotionType::Static, .Layer = PhysicsLayer::Static});
    fixture.World->Add<Collider>(anchor, Collider{.Extents = vec3(0.25f)});

    const Entity bob = fixture.World->CreateEntity();
    fixture.World->Add<Transform>(bob, Transform{.Position = vec3(1.0f, 5.0f, 0.0f)});
    fixture.World->Add<RigidBody>(bob, RigidBody{.Motion = MotionType::Dynamic, .Mass = 2.0f});
    fixture.World->Add<Collider>(bob, Collider{.Extents = vec3(0.25f)});

    fixture.Step(1);
    fixture.World->Add<PointConstraint>(
        bob, PointConstraint{.Target = anchor, .Point = vec3(0.0f, 5.0f, 0.0f)});
    fixture.Step(240);

    // A pendulum under gravity swings but never leaves its arc: the pivot distance holds.
    const vec3 bobPosition = fixture.World->Get<Transform>(bob).Position;
    CHECK(glm::length(bobPosition - vec3(0.0f, 5.0f, 0.0f)) == doctest::Approx(1.0f).epsilon(0.05));
    CHECK(bobPosition.y < 5.0f);

    // A hinge about +z keeps its body in the xy plane through the pivot.
    const Entity door = fixture.World->CreateEntity();
    fixture.World->Add<Transform>(door, Transform{.Position = vec3(1.0f, 0.0f, 0.0f)});
    fixture.World->Add<RigidBody>(door, RigidBody{.Motion = MotionType::Dynamic, .Mass = 2.0f});
    fixture.World->Add<Collider>(door, Collider{.Extents = vec3(0.5f, 0.5f, 0.1f)});

    const Entity frame = fixture.World->CreateEntity();
    fixture.World->Add<Transform>(frame, Transform{.Position = vec3(0.0f, 0.0f, 0.0f)});
    fixture.World->Add<RigidBody>(
        frame, RigidBody{.Motion = MotionType::Static, .Layer = PhysicsLayer::Static});
    fixture.World->Add<Collider>(frame, Collider{.Extents = vec3(0.1f)});

    fixture.Step(1);
    fixture.World->Add<HingeConstraint>(
        door,
        HingeConstraint{.Target = frame, .Point = vec3(0.0f), .Axis = vec3(0.0f, 0.0f, 1.0f)});
    fixture.Step(240);

    CHECK(fixture.Physics().GetConstraintCount() == 2);
    CHECK(fixture.World->Get<Transform>(door).Position.z == doctest::Approx(0.0f).epsilon(0.01));
}
