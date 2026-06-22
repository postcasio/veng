// Sim/View camera rig: the pure follow math (FollowCamera) and the View-phase
// CameraRigSystem reading the pawn position the Sim-phase MovementSystem finalized this
// tick. Pure CPU — no Context, no Vulkan symbol touched; builds a real Scene over a
// TypeRegistry and drives the real systems through a SceneSimulation.

#include <doctest/doctest.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/CameraRig.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Movement.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SceneSimulation.h>
#include <Veng/Scene/SystemRegistry.h>
#include <Veng/Scene/Transforms.h>

using namespace Veng;

namespace
{
    bool VecApprox(const vec3& a, const vec3& b, const f32 eps = 1e-4f)
    {
        return glm::all(glm::lessThan(glm::abs(a - b), vec3(eps)));
    }

    TypeRegistry MakeRegistry()
    {
        TypeRegistry registry;
        RegisterBuiltinTypes(registry);
        return registry;
    }

    // A SystemContext over never-dereferenced storage: the systems under test (movement,
    // camera rig) ignore the context entirely.
    struct ContextStorage
    {
        alignas(16) unsigned char AssetsBytes[64]{};
        alignas(16) unsigned char InputBytes[64]{};

        SystemContext Make()
        {
            return SystemContext{
                .Assets = *reinterpret_cast<AssetManager*>(AssetsBytes),
                .Input = *reinterpret_cast<Input*>(InputBytes),
            };
        }
    };
}

TEST_CASE("FollowCamera with zero damping snaps the camera behind the target")
{
    const Transform current; // identity
    const mat4 targetWorld = glm::translate(mat4(1.0f), vec3(2.0f, 0.0f, 0.0f));
    const CameraFollow follow{
        .Target = Entity::Null, .Offset = vec3(0.0f, 0.0f, 5.0f), .Damping = 0.0f};

    const Transform result = FollowCamera(current, targetWorld, follow, 0.016f);

    // Target at (2,0,0), offset +5 along its (unrotated) local Z: camera at (2,0,5).
    CHECK(VecApprox(result.Position, vec3(2.0f, 0.0f, 5.0f)));
}

TEST_CASE("FollowCamera rotates the offset into the target's orientation")
{
    const Transform current;
    // Yaw the target 90 degrees about world up, so its local +Z points along world +X.
    const quat yaw = glm::angleAxis(glm::radians(90.0f), vec3(0.0f, 1.0f, 0.0f));
    const mat4 targetWorld = glm::mat4_cast(yaw);
    const CameraFollow follow{.Offset = vec3(0.0f, 0.0f, 5.0f), .Damping = 0.0f};

    const Transform result = FollowCamera(current, targetWorld, follow, 0.016f);

    // The +Z offset rotates to world +X: camera at (5,0,0).
    CHECK(VecApprox(result.Position, vec3(5.0f, 0.0f, 0.0f)));
}

TEST_CASE("FollowCamera with damping lands between the current pose and the goal")
{
    const Transform current; // at origin
    const mat4 targetWorld = glm::translate(mat4(1.0f), vec3(0.0f, 0.0f, 10.0f));
    const CameraFollow follow{.Offset = vec3(0.0f, 0.0f, 5.0f), .Damping = 5.0f};

    // Goal is target (0,0,10) + offset +5Z = (0,0,15). With damping the camera moves part way.
    const Transform result = FollowCamera(current, targetWorld, follow, 0.1f);

    CHECK(result.Position.z > 0.0f);
    CHECK(result.Position.z < 15.0f);
}

TEST_CASE("FollowCamera is deterministic for a fixed delta")
{
    const Transform current;
    const mat4 targetWorld = glm::translate(mat4(1.0f), vec3(1.0f, 2.0f, 3.0f));
    const CameraFollow follow{.Offset = vec3(0.0f, 4.0f, 8.0f), .Damping = 3.0f};

    const Transform a = FollowCamera(current, targetWorld, follow, 0.05f);
    const Transform b = FollowCamera(current, targetWorld, follow, 0.05f);

    CHECK(VecApprox(a.Position, b.Position));
    CHECK(glm::abs(glm::dot(a.Rotation, b.Rotation)) == doctest::Approx(1.0f).epsilon(1e-5f));
}

TEST_CASE("The View-phase rig reads the pawn position the Sim-phase movement finalized this tick")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    // A pawn that the movement system advances along +Z this tick.
    const Entity pawn = scene->CreateEntity();
    scene->Add<Transform>(pawn, Transform{});
    scene->Add<Intent>(pawn, Intent{.Move = vec3(0.0f, 0.0f, 1.0f)});
    scene->Add<Mover>(pawn, Mover{.MoveSpeed = 10.0f, .TurnSpeed = 0.0f});

    // A camera that snaps directly to the pawn (zero offset, zero damping), so its post-tick
    // position equals the pawn's finalized position only if the rig ran after movement.
    const Entity camera = scene->CreateEntity();
    scene->Add<Transform>(camera, Transform{});
    scene->Add<CameraFollow>(camera,
                             CameraFollow{.Target = pawn, .Offset = vec3(0.0f), .Damping = 0.0f});

    // Registration order is intentionally rig-before-movement; the phase split must still
    // run movement (Sim) first, then the rig (View).
    SystemRegistry systems;
    systems.Register<CameraRigSystem>();
    systems.Register<MovementSystem>();

    SceneSimulation sim(systems);
    ContextStorage storage;
    sim.Update(*scene, 0.1f, storage.Make());

    // Movement: 1 * 10 * 0.1 = 1.0 along +Z. The rig snaps the camera onto that finalized pose.
    const vec3 pawnPosition = scene->Get<Transform>(pawn).Position;
    CHECK(VecApprox(pawnPosition, vec3(0.0f, 0.0f, 1.0f)));
    CHECK(VecApprox(scene->Get<Transform>(camera).Position, pawnPosition));
}

TEST_CASE("The rig leaves a camera with an unwired follow target untouched")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    const Entity camera = scene->CreateEntity();
    Transform pose;
    pose.Position = vec3(1.0f, 2.0f, 3.0f);
    scene->Add<Transform>(camera, pose);
    scene->Add<CameraFollow>(camera, CameraFollow{.Target = Entity::Null});

    CameraRigSystem rig;
    ContextStorage storage;
    rig.OnUpdate(*scene, 0.1f, storage.Make());

    CHECK(VecApprox(scene->Get<Transform>(camera).Position, vec3(1.0f, 2.0f, 3.0f)));
}
