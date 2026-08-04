// Interaction focus resolution: the builtin InteractionSystem sweeps an Overlap within an
// Interactor's reach, keeps the enabled Interactables inside its cone and within their own range,
// and writes the best by angle then distance to Focused. Pure CPU — a headless Scene and a solver,
// no Context, no GPU.

#include <doctest/doctest.h>

#include <Veng/Input.h>
#include <Veng/Physics/Components.h>
#include <Veng/Physics/PhysicsSystem.h>
#include <Veng/Physics/PhysicsWorld.h>
#include <Veng/Physics/PoseResolver.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Interaction.h>
#include <Veng/Scene/InteractionSystem.h>
#include <Veng/Scene/Scene.h>

using namespace Veng;

namespace
{
    constexpr f32 FixedStep = 1.0f / 60.0f;

    // A solver frame anchored away from the Transform chain's origin. The magnitude is irrelevant to
    // the mechanism — it need only exceed the interactor's reach, so that a sweep run in the wrong
    // frame finds nothing at all.
    constexpr dvec3 SolverOrigin(1000.0, 0.0, -2000.0);

    // A SystemContext the InteractionSystem never reads into — it resolves purely from scene state.
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
                .Audio = *reinterpret_cast<Audio::AudioEngine*>(TasksBytes),
            };
        }
    };

    // A scene with a physics world and the interactor at the origin facing -Z (its local forward).
    struct InteractionScene
    {
        TypeRegistry Types;
        Unique<Scene> World;
        Entity Actor;

        InteractionScene()
        {
            RegisterBuiltinTypes(Types);
            World = Scene::Create(Types);
            World->SetPhysicsWorld(PhysicsWorld::Create(PhysicsWorldInfo{}));

            Actor = World->CreateEntity();
            World->Add<Transform>(Actor, Transform{});
            // A generous reach so the sphere sweep returns every candidate — the cone and range
            // filters, not the sweep radius, are what these cases exercise.
            World->Add<Interactor>(Actor,
                                   Interactor{.Reach = 20.0f, .ConeAngle = glm::radians(45.0f)});
        }

        // A static, findable interactable body whose Transform place is @p position but whose body
        // stands at SolverOrigin + position: the split-frame shape, with the Transform bound to
        // nothing (SyncTransform cleared) and PhysicsPose the only channel to the solver.
        Entity AddInteractableInSolverFrame(const vec3 position, const f32 range)
        {
            const Entity entity = World->CreateEntity();
            World->Add<Transform>(entity, Transform{.Position = position});
            World->Add<RigidBody>(entity, RigidBody{.Motion = MotionType::Static,
                                                    .Layer = PhysicsLayer::Query,
                                                    .SyncTransform = false});
            World->Add<Collider>(entity,
                                 Collider{.Shape = ColliderShape::Box, .Extents = vec3(0.25f)});
            World->Add<PhysicsPose>(entity,
                                    PhysicsPose{.Position = SolverOrigin + dvec3(position)});
            World->Add<Interactable>(entity, Interactable{.Verb = "Use", .Range = range});
            return entity;
        }

        // A static, findable interactable body at a world position.
        Entity AddInteractable(const vec3 position, const f32 range, const bool enabled)
        {
            const Entity entity = World->CreateEntity();
            World->Add<Transform>(entity, Transform{.Position = position});
            World->Add<RigidBody>(
                entity, RigidBody{.Motion = MotionType::Static, .Layer = PhysicsLayer::Query});
            World->Add<Collider>(entity,
                                 Collider{.Shape = ColliderShape::Box, .Extents = vec3(0.25f)});
            World->Add<Interactable>(
                entity, Interactable{.Verb = "Use", .Range = range, .Enabled = enabled});
            return entity;
        }

        // A static, findable interactable whose box is large enough to hold the interactor: the
        // enterable case, where the origin the cone is measured to is an interior point.
        Entity AddLargeInteractable(const vec3 position, const vec3 halfExtents, const f32 range)
        {
            const Entity entity = World->CreateEntity();
            World->Add<Transform>(entity, Transform{.Position = position});
            World->Add<RigidBody>(
                entity, RigidBody{.Motion = MotionType::Static, .Layer = PhysicsLayer::Query});
            World->Add<Collider>(entity,
                                 Collider{.Shape = ColliderShape::Box, .Extents = halfExtents});
            World->Add<Interactable>(entity, Interactable{.Verb = "Use", .Range = range});
            return entity;
        }

        // Projects the Transform chain into the offset solver frame, delegating the chain composition
        // to the engine's default — the shape a consumer with an external authoritative store takes.
        void InstallOffsetResolver()
        {
            World->SetPhysicsPoseResolver(CreateUnique<PhysicsPoseResolver>(PhysicsPoseResolver{
                .Resolve =
                    [](const Scene& scene, const Entity entity, const mat4& localOffset)
                {
                    PhysicsPose pose = DefaultResolvePhysicsPose(scene, entity, localOffset);
                    pose.Position += SolverOrigin;
                    return pose;
                },
            }));
        }

        Entity Resolve()
        {
            // One step brings the static bodies into the broad phase so Overlap can find them.
            StepPhysics(*World, FixedStep);
            ContextStorage storage;
            InteractionSystem system;
            system.OnUpdate(*World, FixedStep, storage.Make());
            return World->Get<Interactor>(Actor).Focused;
        }
    };
}

TEST_CASE("The nearest in-cone interactable wins over a farther one dead ahead")
{
    InteractionScene fixture;
    // Both dead ahead (zero bearing), so distance is the tie-break and the nearer wins.
    const Entity near = fixture.AddInteractable(vec3(0.0f, 0.0f, -2.0f), 10.0f, true);
    fixture.AddInteractable(vec3(0.0f, 0.0f, -5.0f), 10.0f, true);
    CHECK(fixture.Resolve() == near);
}

TEST_CASE("An interactable outside the cone is not selected")
{
    InteractionScene fixture;
    // Almost due sideways (+X) from an actor facing -Z: ~90 degrees off, past the 45 degree cone.
    fixture.AddInteractable(vec3(5.0f, 0.0f, -0.1f), 10.0f, true);
    CHECK(fixture.Resolve().IsNull());
}

TEST_CASE("An interactable beyond its own range is not selected")
{
    InteractionScene fixture;
    // Dead ahead and inside the reach sweep, but past the interactable's own 1 m range at 2 m.
    fixture.AddInteractable(vec3(0.0f, 0.0f, -2.0f), 1.0f, true);
    CHECK(fixture.Resolve().IsNull());
}

TEST_CASE("A disabled interactable is skipped")
{
    InteractionScene fixture;
    const Entity target = fixture.AddInteractable(vec3(0.0f, 0.0f, -2.0f), 10.0f, false);
    CHECK(fixture.Resolve().IsNull());

    // Enabling that same placement makes it the focus, proving only Enabled kept it out.
    fixture.World->Get<Interactable>(target).Enabled = true;
    CHECK(fixture.Resolve() == target);
}

TEST_CASE("Focused is Null with no candidates")
{
    InteractionScene fixture;
    CHECK(fixture.Resolve().IsNull());
}

TEST_CASE("The best in-cone interactable is preferred by bearing over a closer off-axis one")
{
    InteractionScene fixture;
    // A closer candidate off to the side within the cone, and a farther one dead ahead. Angle wins
    // over distance, so the dead-ahead one is focused.
    fixture.AddInteractable(vec3(1.0f, 0.0f, -1.2f), 10.0f, true);
    const Entity ahead = fixture.AddInteractable(vec3(0.0f, 0.0f, -3.0f), 10.0f, true);
    CHECK(fixture.Resolve() == ahead);
}

TEST_CASE("An offset pose resolver lands focus resolution in the solver's frame")
{
    InteractionScene fixture;
    fixture.InstallOffsetResolver();
    // Both bodies stand in the solver's frame; only the projection of their Transform places them
    // ahead of the interactor. Dead ahead, so distance breaks the tie and the nearer wins.
    const Entity near = fixture.AddInteractableInSolverFrame(vec3(0.0f, 0.0f, -2.0f), 10.0f);
    fixture.AddInteractableInSolverFrame(vec3(0.0f, 0.0f, -5.0f), 10.0f);
    CHECK(fixture.Resolve() == near);

    // The cone and range filters run in that same frame, so an out-of-cone candidate is still
    // rejected rather than admitted by an accidentally-shifted bearing.
    InteractionScene sideways;
    sideways.InstallOffsetResolver();
    sideways.AddInteractableInSolverFrame(vec3(5.0f, 0.0f, -0.1f), 10.0f);
    CHECK(sideways.Resolve().IsNull());
}

TEST_CASE("An offset solver frame focuses nothing while no resolver is installed")
{
    InteractionScene fixture;
    // The gap the seam closes: the sweep runs at the interactor's Transform place, where the solver
    // holds nothing, so no interactable is ever offered.
    fixture.AddInteractableInSolverFrame(vec3(0.0f, 0.0f, -2.0f), 10.0f);
    CHECK(fixture.Resolve().IsNull());
}

TEST_CASE("An interactable whose body encloses the interactor is focused from inside it")
{
    InteractionScene fixture;
    // The enterable shape: a 4 x 4 x 24 m box holding the interactor, whose *origin* sits 10.6 m
    // behind an actor facing -Z. The bearing to that origin is a half turn, so no ConeAngle short of
    // one admits it — and the interactor is standing in it, which is the whole reason it should be
    // offered. Range covers the offset from the origin to the interactor inside it.
    const Entity cabin =
        fixture.AddLargeInteractable(vec3(0.0f, 0.0f, 10.6f), vec3(2.0f, 2.0f, 12.0f), 14.0f);
    CHECK(fixture.Resolve() == cabin);
}

TEST_CASE("A large interactable behind the interactor but not enclosing it is still rejected")
{
    InteractionScene fixture;
    // Same bearing, same size class, and inside the reach sweep — but the box starts 15 m astern, so
    // the interactor is outside it and the cone means what it says. The exemption is enclosure, not
    // bulk.
    fixture.AddLargeInteractable(vec3(0.0f, 0.0f, 20.0f), vec3(2.0f, 2.0f, 5.0f), 30.0f);
    CHECK(fixture.Resolve().IsNull());
}

TEST_CASE("A candidate genuinely looked at wins over the body the interactor stands in")
{
    InteractionScene fixture;
    // An enclosing body keeps its true bearing for the ranking, so the thing dead ahead is preferred
    // — being inside something does not capture focus away from what is being looked at.
    fixture.AddLargeInteractable(vec3(0.0f, 0.0f, 10.6f), vec3(2.0f, 2.0f, 12.0f), 14.0f);
    const Entity ahead = fixture.AddInteractable(vec3(0.0f, 0.0f, -2.0f), 10.0f, true);
    CHECK(fixture.Resolve() == ahead);
}

TEST_CASE("An enclosing interactable is still subject to Enabled and to its own Range")
{
    InteractionScene fixture;
    // Enclosure exempts the cone and nothing else: the two data gates still hold.
    const Entity cabin =
        fixture.AddLargeInteractable(vec3(0.0f, 0.0f, 10.6f), vec3(2.0f, 2.0f, 12.0f), 14.0f);
    fixture.World->Get<Interactable>(cabin).Enabled = false;
    CHECK(fixture.Resolve().IsNull());

    fixture.World->Get<Interactable>(cabin).Enabled = true;
    fixture.World->Get<Interactable>(cabin).Range = 5.0f;
    CHECK(fixture.Resolve().IsNull());

    fixture.World->Get<Interactable>(cabin).Range = 14.0f;
    CHECK(fixture.Resolve() == cabin);
}
