// Interaction focus resolution: the builtin InteractionSystem sweeps an Overlap within an
// Interactor's reach, keeps the enabled Interactables inside its cone and within their own range,
// and writes the best by angle then distance to Focused. Pure CPU — a headless Scene and a solver,
// no Context, no GPU.

#include <doctest/doctest.h>

#include <Veng/Input.h>
#include <Veng/Physics/Components.h>
#include <Veng/Physics/PhysicsSystem.h>
#include <Veng/Physics/PhysicsWorld.h>
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
