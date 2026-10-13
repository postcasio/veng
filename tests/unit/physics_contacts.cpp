// Physics contact reports: what a ContactReport body touched, published after the step as
// BodyContacts — the pair, the point and normal, the approach speed before the solver acted, the
// impulse estimate and whether the contact began — merged per pair, capped per body, free in a world
// that marks nothing, and invisible to the simulation. Pure CPU — a headless Scene with no Context.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

#include <Veng/Physics/Components.h>
#include <Veng/Physics/PhysicsSystem.h>
#include <Veng/Physics/PhysicsWorld.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

using namespace Veng;

namespace
{
    constexpr f32 FixedStep = 1.0f / 60.0f;
    constexpr f32 Gravity = 9.81f;

    // A scene with a physics world and the builtin types registered.
    struct ContactFixture
    {
        TypeRegistry Types;
        Unique<Scene> World;
        mutable u64 SimTick = Scene::MinChangeTick;

        explicit ContactFixture(const PhysicsWorldInfo& info = {})
        {
            RegisterBuiltinTypes(Types);
            World = Scene::Create(Types);
            World->SetPhysicsWorld(PhysicsWorld::Create(info));
        }

        PhysicsWorld& Physics() const { return *World->GetPhysicsWorld(); }

        // Steps as the world drive does, advancing the change tick so an edit is seen as one.
        void Tick(const u32 count = 1) const
        {
            for (u32 i = 0; i < count; ++i)
            {
                World->SetChangeTick(++SimTick);
                StepPhysics(*World, FixedStep);
            }
        }

        Entity Spawn(const vec3 position, const RigidBody& body, const Collider& collider,
                     const bool report) const
        {
            const Entity entity = World->CreateEntity();
            World->Add<Transform>(entity, Transform{.Position = position});
            World->Add<RigidBody>(entity, body);
            World->Add<Collider>(entity, collider);
            if (report)
            {
                World->Add<ContactReport>(entity);
            }
            return entity;
        }

        Entity SpawnStaticBox(const vec3 position, const vec3 extents) const
        {
            return Spawn(position,
                         RigidBody{.Motion = MotionType::Static, .Layer = PhysicsLayer::Static},
                         Collider{.Shape = ColliderShape::Box, .Extents = extents}, false);
        }

        const vector<BodyContact>& Contacts(const Entity entity) const
        {
            return World->Get<BodyContacts>(entity).Current;
        }
    };

    PhysicsWorldInfo Weightless()
    {
        return PhysicsWorldInfo{.Gravity = vec3(0.0f)};
    }

    RigidBody Loose(const f32 mass)
    {
        return RigidBody{.Mass = mass, .LinearDamping = 0.0f, .AngularDamping = 0.0f};
    }

    Collider Sphere(const f32 radius)
    {
        return Collider{.Shape = ColliderShape::Sphere, .Extents = vec3(radius)};
    }

    Collider Box(const vec3 extents)
    {
        return Collider{.Shape = ColliderShape::Box, .Extents = extents};
    }

    // Several boxes falling onto the ground and onto each other, tumbling as they land.
    void SpawnTumblers(const ContactFixture& fixture, const bool report)
    {
        fixture.SpawnStaticBox(vec3(0.0f, -1.0f, 0.0f), vec3(20.0f, 1.0f, 20.0f));
        for (u32 i = 0; i < 6; ++i)
        {
            const auto f = static_cast<f32>(i);
            const Entity box = fixture.Spawn(vec3(0.3f * f - 0.6f, 1.0f + 1.3f * f, 0.2f * f),
                                             RigidBody{.Mass = 2.0f + f}, Box(vec3(0.5f)), report);
            fixture.World->Get<Transform>(box).Rotation =
                glm::angleAxis(0.4f * f, glm::normalize(vec3(1.0f, 2.0f, 0.5f)));
        }
    }
}

TEST_CASE("a head-on hit is reported once on each side as begun, with the momentum it exchanged")
{
    const ContactFixture fixture(Weightless());
    constexpr f32 LightMass = 2.0f;
    constexpr f32 HeavyMass = 6.0f;
    // Apart by more than the solver's speculative reach, so the contact is found once they overlap.
    const Entity light =
        fixture.Spawn(vec3(-0.535f, 0.0f, 0.0f), Loose(LightMass), Sphere(0.5f), true);
    const Entity heavy =
        fixture.Spawn(vec3(0.535f, 0.0f, 0.0f), Loose(HeavyMass), Sphere(0.5f), true);
    fixture.Tick();
    fixture.Physics().SetLinearVelocity(light, vec3(3.0f, 0.0f, 0.0f));
    fixture.Physics().SetLinearVelocity(heavy, vec3(-3.0f, 0.0f, 0.0f));

    vec3 before(0.0f);
    for (u32 i = 0; i < 4 && fixture.Contacts(light).empty(); ++i)
    {
        before = fixture.Physics().GetLinearVelocity(light);
        fixture.Tick();
    }
    const vector<BodyContact>& onLight = fixture.Contacts(light);
    const vector<BodyContact>& onHeavy = fixture.Contacts(heavy);
    REQUIRE(onLight.size() == 1);
    REQUIRE(onHeavy.size() == 1);
    CHECK(onLight[0].Other == heavy);
    CHECK(onHeavy[0].Other == light);
    CHECK(onLight[0].Began);
    CHECK(onHeavy[0].Began);
    CHECK(onLight[0].ApproachSpeed == doctest::Approx(6.0f).epsilon(1e-4));
    CHECK(onHeavy[0].ApproachSpeed == doctest::Approx(6.0f).epsilon(1e-4));
    CHECK(glm::dot(onLight[0].Normal, vec3(-1.0f, 0.0f, 0.0f)) == doctest::Approx(1.0f));
    CHECK(glm::dot(onLight[0].Normal, onHeavy[0].Normal) == doctest::Approx(-1.0f));

    // With nothing else touching either body, the estimate is the momentum the step exchanged.
    const f32 exchanged =
        LightMass * glm::length(fixture.Physics().GetLinearVelocity(light) - before);
    REQUIRE(exchanged > 1.0f);
    CHECK(onLight[0].Impulse == doctest::Approx(exchanged).epsilon(0.02));
    CHECK(onHeavy[0].Impulse == doctest::Approx(exchanged).epsilon(0.02));

    // Inelastic, so the pair moves on together, still in contact and no longer beginning.
    fixture.Tick();
    REQUIRE(fixture.Contacts(light).size() == 1);
    CHECK_FALSE(fixture.Contacts(light)[0].Began);
}

TEST_CASE("a resting contact persists at the approach and impulse of one step's weight")
{
    for (const u32 steps : {1U, 2U})
    {
        CAPTURE(steps);
        const ContactFixture fixture(PhysicsWorldInfo{.CollisionSteps = steps});
        constexpr f32 Mass = 10.0f;
        const Entity ground =
            fixture.SpawnStaticBox(vec3(0.0f, -1.0f, 0.0f), vec3(20.0f, 1.0f, 20.0f));
        const Entity box =
            fixture.Spawn(vec3(0.0f, 0.5f, 0.0f), RigidBody{.Mass = Mass}, Box(vec3(0.5f)), true);
        fixture.Tick();
        REQUIRE(fixture.Contacts(box).size() == 1);
        CHECK(fixture.Contacts(box)[0].Began);

        fixture.Tick(5);
        REQUIRE(fixture.Contacts(box).size() == 1);
        const BodyContact& contact = fixture.Contacts(box)[0];
        CHECK(contact.Other == ground);
        CHECK_FALSE(contact.Began);
        CHECK(glm::dot(contact.Normal, vec3(0.0f, 1.0f, 0.0f)) == doctest::Approx(1.0f));
        CHECK(contact.Point.y == doctest::Approx(0.0).epsilon(0.01));
        // The only approach a resting contact has is the fall gravity adds in one collision step.
        const f32 fall = Gravity * FixedStep / static_cast<f32>(steps);
        CHECK(contact.ApproachSpeed >= 0.0f);
        CHECK(contact.ApproachSpeed <= fall * 1.05f);
        // Summed over the collision steps, the impulse is the weight's over the whole step.
        CHECK(contact.Impulse == doctest::Approx(Mass * Gravity * FixedStep).epsilon(0.05));
    }
}

TEST_CASE("a contact point's approach includes the body's spin")
{
    const ContactFixture fixture(Weightless());
    // A wall whose face sits 1 cm below a rod's lower face, within the solver's speculative reach,
    // so the contact is found on the first step at the velocities set before it.
    const Entity wall = fixture.SpawnStaticBox(vec3(0.8f, 0.0f, -0.61f), vec3(0.4f, 1.0f, 0.5f));
    const Entity rod = fixture.Spawn(vec3(0.0f), Loose(1.0f), Box(vec3(1.0f, 0.1f, 0.1f)), true);
    fixture.Tick();
    const vec3 spin(0.0f, 3.0f, 0.0f);
    fixture.Physics().SetLinearVelocity(rod, vec3(0.0f));
    fixture.Physics().SetAngularVelocity(rod, spin);
    const dvec3 centre = fixture.Physics().GetBodyPose(rod)->Position;
    fixture.Tick();

    REQUIRE(fixture.Contacts(rod).size() == 1);
    const BodyContact& contact = fixture.Contacts(rod)[0];
    CHECK(contact.Other == wall);
    CHECK(glm::dot(contact.Normal, vec3(0.0f, 0.0f, 1.0f)) == doctest::Approx(1.0f));
    // The rod has no linear velocity: every bit of the approach is its spin at the point.
    const vec3 pointVelocity = glm::cross(spin, vec3(contact.Point - centre));
    const f32 expected = -glm::dot(pointVelocity, contact.Normal);
    REQUIRE(expected > 1.0f);
    CHECK(contact.ApproachSpeed == doctest::Approx(expected).epsilon(0.01));
}

TEST_CASE("a kinematic mover swept into a reporting body is reported at its own speed")
{
    const ContactFixture fixture(Weightless());
    constexpr f32 Speed = 4.0f;
    const Entity mover =
        fixture.Spawn(vec3(-1.0f, 0.0f, 0.0f), RigidBody{.Motion = MotionType::Kinematic},
                      Box(vec3(0.25f)), false);
    const Entity target = fixture.Spawn(vec3(0.0f), Loose(1.0f), Sphere(0.5f), true);
    fixture.Tick();
    for (u32 i = 0; i < 20 && fixture.Contacts(target).empty(); ++i)
    {
        fixture.World->Get<Transform>(mover).Position.x += Speed * FixedStep;
        fixture.Tick();
    }
    REQUIRE(fixture.Contacts(target).size() == 1);
    const BodyContact& contact = fixture.Contacts(target)[0];
    CHECK(contact.Other == mover);
    CHECK(contact.Began);
    CHECK(contact.ApproachSpeed == doctest::Approx(Speed).epsilon(0.01));
    CHECK(contact.Impulse > 0.0f);
}

TEST_CASE("a linear-cast hit is reported on the step the body is stopped, and begins once")
{
    const ContactFixture fixture(Weightless());
    constexpr f32 Speed = 300.0f;
    constexpr f32 Mass = 10.0f;
    // A 0.1 m slab across x = 0; a step at 300 m/s is fifty times as long.
    const Entity slab = fixture.SpawnStaticBox(vec3(0.0f), vec3(0.05f, 5.0f, 5.0f));
    const Entity bullet = fixture.Spawn(
        vec3(-3.0f, 0.0f, 0.0f),
        RigidBody{.Mass = Mass, .LinearDamping = 0.0f, .Quality = MotionQuality::LinearCast},
        Sphere(0.5f), true);
    fixture.Tick();
    fixture.Physics().SetLinearVelocity(bullet, vec3(Speed, 0.0f, 0.0f));
    fixture.Tick();

    REQUIRE(fixture.Physics().GetBodyPose(bullet)->Position.x < -0.5);
    REQUIRE(fixture.Contacts(bullet).size() == 1);
    const BodyContact& hit = fixture.Contacts(bullet)[0];
    CHECK(hit.Other == slab);
    CHECK(hit.Began);
    CHECK(glm::dot(hit.Normal, vec3(-1.0f, 0.0f, 0.0f)) == doctest::Approx(1.0f));
    CHECK(hit.ApproachSpeed == doctest::Approx(Speed).epsilon(0.01));
    CHECK(hit.Impulse == doctest::Approx(Mass * Speed).epsilon(0.02));

    fixture.Tick();
    REQUIRE(fixture.Contacts(bullet).size() == 1);
    CHECK_FALSE(fixture.Contacts(bullet)[0].Began);
}

TEST_CASE("a world that marks no body records no contact")
{
    for (const bool report : {false, true})
    {
        CAPTURE(report);
        const ContactFixture fixture;
        SpawnTumblers(fixture, report);
        u32 reported = 0;
        for (u32 i = 0; i < 90; ++i)
        {
            fixture.Tick();
            reported = std::max(reported, fixture.Physics().GetStepStats().ContactsReported);
        }
        // Marking the same bodies reports their landings, so the zero is not vacuous.
        CHECK((reported > 0) == report);
    }
}

TEST_CASE("a body touching more bodies than the cap keeps the fastest and counts the rest")
{
    const ContactFixture fixture;
    fixture.SpawnStaticBox(vec3(0.0f, -1.0f, 0.0f), vec3(20.0f, 1.0f, 20.0f));
    const Entity plate = fixture.Spawn(vec3(0.0f, 0.25f, 0.0f), RigidBody{.Mass = 100.0f},
                                       Box(vec3(6.0f, 0.25f, 6.0f)), true);
    // Thirty boxes resting on the plate, which also rests on the ground: thirty-one other bodies.
    for (u32 row = 0; row < 5; ++row)
    {
        for (u32 column = 0; column < 6; ++column)
        {
            fixture.Spawn(vec3(1.5f * static_cast<f32>(column) - 3.75f, 1.0f,
                               1.5f * static_cast<f32>(row) - 3.0f),
                          RigidBody{.Mass = 1.0f}, Box(vec3(0.5f)), false);
        }
    }
    fixture.Tick(2);

    const vector<BodyContact>& contacts = fixture.Contacts(plate);
    CHECK(contacts.size() == MaxBodyContacts);
    CHECK(fixture.Physics().GetStepStats().ContactsReported == MaxBodyContacts);
    CHECK(fixture.Physics().GetStepStats().ContactsDropped == 31 - MaxBodyContacts);
    CHECK(std::ranges::is_sorted(contacts, std::ranges::greater{}, &BodyContact::ApproachSpeed));
}

TEST_CASE("reporting contacts perturbs nothing, and the marker comes and goes in place")
{
    const auto run = [](const bool report)
    {
        const ContactFixture fixture;
        SpawnTumblers(fixture, report);
        fixture.Tick(120);
        return fixture.Physics().HashPoses();
    };
    CHECK(run(true) == run(false));

    const ContactFixture fixture(Weightless());
    const Entity body = fixture.Spawn(vec3(0.0f), Loose(1.0f), Sphere(0.5f), false);
    fixture.Tick();
    fixture.Physics().SetLinearVelocity(body, vec3(1.0f, 0.0f, 0.0f));
    fixture.World->Add<ContactReport>(body);
    fixture.Tick();
    CHECK(fixture.World->Has<BodyContacts>(body));
    REQUIRE(fixture.World->Remove<ContactReport>(body).has_value());
    fixture.Tick();
    CHECK_FALSE(fixture.World->Has<BodyContacts>(body));
    // Neither edit re-created the body, which would have discarded its velocity.
    CHECK(fixture.Physics().GetLinearVelocity(body).x == doctest::Approx(1.0f));
}
