// Sim/View camera rig: the pure follow math (FollowCamera) and the View-phase
// CameraRigSystem reading the pawn position the Sim-phase MovementSystem finalized this
// tick. Pure CPU — no Context, no Vulkan symbol touched; builds a real Scene over a
// TypeRegistry and drives the real systems through a SceneSimulation.

#include <doctest/doctest.h>

#include <cmath>

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
        alignas(16) unsigned char TasksBytes[64]{};

        SystemContext Make()
        {
            return SystemContext{
                .Assets = *reinterpret_cast<AssetManager*>(AssetsBytes),
                .Input = *reinterpret_cast<Input*>(InputBytes),
                .Tasks = *reinterpret_cast<TaskSystem*>(TasksBytes),
                .Audio = *reinterpret_cast<Audio::AudioEngine*>(TasksBytes),
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

TEST_CASE("FollowCamera rotates the offset by the target's yaw")
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

TEST_CASE("FollowCamera keeps the camera above when the target pitches")
{
    const Transform current;
    // Pitch the target steeply nose-down about its local right; the offset trails by yaw
    // only, so the camera holds the offset's +Y height instead of swinging below.
    const quat pitch = glm::angleAxis(glm::radians(60.0f), vec3(1.0f, 0.0f, 0.0f));
    const mat4 targetWorld = glm::mat4_cast(pitch);
    const CameraFollow follow{.Offset = vec3(0.0f, 6.0f, 12.0f), .Damping = 0.0f};

    const Transform result = FollowCamera(current, targetWorld, follow, 0.016f);

    // Target unpitched in yaw, so the offset is unrotated: camera above and behind at (0,6,12).
    CHECK(VecApprox(result.Position, vec3(0.0f, 6.0f, 12.0f)));
}

TEST_CASE("FollowCamera orbits the camera by the follow pitch, preserving its distance")
{
    const Transform current;
    const mat4 targetWorld = mat4(1.0f); // target at the origin
    CameraFollow follow{.Offset = vec3(0.0f, 6.0f, 12.0f), .Damping = 0.0f};
    follow.Pitch = glm::radians(30.0f);

    const Transform result = FollowCamera(current, targetWorld, follow, 0.016f);

    // A positive pitch orbits the camera downward (tilting it to look up at the target)
    // about the target, so it drops below the offset height but keeps its distance.
    CHECK(result.Position.y < 6.0f);
    CHECK(glm::length(result.Position) ==
          doctest::Approx(glm::length(vec3(0.0f, 6.0f, 12.0f))).epsilon(0.01f));
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

TEST_CASE("LookRotation with a positive yaw turns the forward axis left")
{
    const CameraLook look{.Yaw = glm::radians(90.0f)};

    const vec3 forward = LookRotation(look) * vec3(0.0f, 0.0f, -1.0f);

    // Positive yaw about world up rotates -Z toward -X.
    CHECK(VecApprox(forward, vec3(-1.0f, 0.0f, 0.0f)));
}

TEST_CASE("LookRotation with a positive pitch looks up")
{
    const CameraLook look{.Pitch = glm::radians(45.0f)};

    const vec3 forward = LookRotation(look) * vec3(0.0f, 0.0f, -1.0f);

    CHECK(forward.y == doctest::Approx(glm::sin(glm::radians(45.0f))).epsilon(1e-4f));
    CHECK(forward.z < 0.0f);
}

TEST_CASE("LookRotation clamps the pitch to the limit")
{
    const CameraLook look{.Pitch = 3.0f, .PitchLimit = 1.0f};

    const vec3 forward = LookRotation(look) * vec3(0.0f, 0.0f, -1.0f);

    // Clamped to one radian of elevation, not the requested three.
    CHECK(forward.y == doctest::Approx(glm::sin(1.0f)).epsilon(1e-4f));
}

TEST_CASE("The rig writes a look camera's rotation and clamps its stored pitch")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    const Entity camera = scene->CreateEntity();
    Transform pose;
    pose.Position = vec3(1.0f, 2.0f, 3.0f);
    scene->Add<Transform>(camera, pose);
    scene->Add<CameraLook>(
        camera, CameraLook{.Yaw = glm::radians(90.0f), .Pitch = 5.0f, .PitchLimit = 1.2f});

    CameraRigSystem rig;
    ContextStorage storage;
    rig.OnUpdate(*scene, 0.016f, storage.Make());

    // Position untouched; rotation faces the yawed heading; the wound-up pitch stored clamped.
    CHECK(VecApprox(scene->Get<Transform>(camera).Position, vec3(1.0f, 2.0f, 3.0f)));
    const vec3 forward = scene->Get<Transform>(camera).Rotation * vec3(0.0f, 0.0f, -1.0f);
    CHECK(forward.x < -0.3f);
    CHECK(scene->Get<CameraLook>(camera).Pitch == doctest::Approx(1.2f));
}

TEST_CASE("OrbitCamera places the eye at Distance from the focus for a spread of yaw/pitch")
{
    const Transform current;
    const vec3 focus{3.0f, -2.0f, 5.0f};
    const f32 distance = 12.0f;

    for (const f32 yaw : {-2.0f, -0.5f, 0.0f, 0.7f, 2.5f})
    {
        for (const f32 pitch : {-1.0f, -0.2f, 0.0f, 0.4f, 1.0f})
        {
            const CameraOrbit orbit{.Focus = focus,
                                    .Distance = distance,
                                    .Yaw = yaw,
                                    .Pitch = pitch,
                                    .FocusTarget = focus};
            const Transform result = OrbitCamera(orbit, current, 0.016f);

            // The eye sits exactly Distance from the focus, whatever the orbit angle.
            CHECK(glm::length(result.Position - focus) == doctest::Approx(distance).epsilon(1e-4f));
        }
    }
}

TEST_CASE("OrbitCamera's forward points at the focus")
{
    const Transform current;
    const vec3 focus{1.0f, 4.0f, -2.0f};
    const CameraOrbit orbit{
        .Focus = focus, .Distance = 8.0f, .Yaw = 0.9f, .Pitch = 0.6f, .FocusTarget = focus};

    const Transform result = OrbitCamera(orbit, current, 0.016f);

    const vec3 forward = result.Rotation * vec3(0.0f, 0.0f, -1.0f);
    const vec3 toFocus = glm::normalize(focus - result.Position);
    CHECK(VecApprox(forward, toFocus));
}

TEST_CASE("OrbitCamera clamps the distance into its band")
{
    const Transform current;
    const vec3 focus{0.0f};

    const CameraOrbit tooFar{
        .Focus = focus, .Distance = 5000.0f, .MaxDistance = 1000.0f, .FocusTarget = focus};
    CHECK(glm::length(OrbitCamera(tooFar, current, 0.016f).Position - focus) ==
          doctest::Approx(1000.0f).epsilon(1e-4f));

    const CameraOrbit tooNear{
        .Focus = focus, .Distance = 0.01f, .MinDistance = 0.5f, .FocusTarget = focus};
    CHECK(glm::length(OrbitCamera(tooNear, current, 0.016f).Position - focus) ==
          doctest::Approx(0.5f).epsilon(1e-4f));
}

TEST_CASE("OrbitCamera clamps the pitch off the pole")
{
    const Transform current;
    const vec3 focus{0.0f};
    const f32 distance = 10.0f;

    // A pitch far past the limit resolves as if it were exactly the limit.
    const CameraOrbit past{.Focus = focus,
                           .Distance = distance,
                           .Pitch = 3.0f,
                           .PitchLimit = 1.0f,
                           .FocusTarget = focus};
    const CameraOrbit atLimit{.Focus = focus,
                              .Distance = distance,
                              .Pitch = 1.0f,
                              .PitchLimit = 1.0f,
                              .FocusTarget = focus};

    CHECK(VecApprox(OrbitCamera(past, current, 0.016f).Position,
                    OrbitCamera(atLimit, current, 0.016f).Position));
}

TEST_CASE("OrbitCamera's focus glide is frame-rate-independent")
{
    // A camera glides its focus from the origin toward a target; one big step and many small
    // steps summing to the same elapsed time must land the eye in the same place, because
    // exponential smoothing composes exactly.
    const CameraOrbit base{.Focus = vec3(0.0f),
                           .Distance = 6.0f,
                           .Yaw = 0.3f,
                           .Pitch = 0.2f,
                           .FocusTarget = vec3(10.0f, 4.0f, -6.0f),
                           .FocusDamping = 3.0f};

    TypeRegistry registry = MakeRegistry();
    ContextStorage storage;

    // One big step.
    const Unique<Scene> sceneBig = Scene::Create(registry);
    const Entity cameraBig = sceneBig->CreateEntity();
    sceneBig->Add<Transform>(cameraBig, Transform{});
    sceneBig->Add<CameraOrbit>(cameraBig, base);
    CameraRigSystem rigBig;
    rigBig.OnUpdate(*sceneBig, 1.0f, storage.Make());

    // Many small steps summing to the same elapsed time.
    const Unique<Scene> sceneSmall = Scene::Create(registry);
    const Entity cameraSmall = sceneSmall->CreateEntity();
    sceneSmall->Add<Transform>(cameraSmall, Transform{});
    sceneSmall->Add<CameraOrbit>(cameraSmall, base);
    CameraRigSystem rigSmall;
    for (int i = 0; i < 100; ++i)
    {
        rigSmall.OnUpdate(*sceneSmall, 0.01f, storage.Make());
    }

    CHECK(VecApprox(sceneBig->Get<Transform>(cameraBig).Position,
                    sceneSmall->Get<Transform>(cameraSmall).Position, 1e-3f));
}

TEST_CASE("OrbitCamera with zero focus damping snaps the focus to the target")
{
    const Transform current;
    const CameraOrbit orbit{.Focus = vec3(0.0f),
                            .Distance = 5.0f,
                            .FocusTarget = vec3(7.0f, 1.0f, -3.0f),
                            .FocusDamping = 0.0f};

    const Transform result = OrbitCamera(orbit, current, 0.016f);

    // The eye orbits the snapped-to target, so it sits Distance from FocusTarget, not from Focus.
    CHECK(glm::length(result.Position - orbit.FocusTarget) == doctest::Approx(5.0f).epsilon(1e-4f));
}

TEST_CASE("OrbitCamera at a pole-adjacent pitch produces a finite, non-degenerate rotation")
{
    const Transform current;
    const vec3 focus{0.0f};
    // A pitch right at the default limit sits near the pole; the clamp must keep the look-at
    // up vector from collapsing, so the rotation stays finite and unit-length.
    const CameraOrbit orbit{
        .Focus = focus, .Distance = 9.0f, .Pitch = 1.5f, .PitchLimit = 1.5f, .FocusTarget = focus};

    const Transform result = OrbitCamera(orbit, current, 0.016f);

    const quat r = result.Rotation;
    CHECK(std::isfinite(r.x));
    CHECK(std::isfinite(r.y));
    CHECK(std::isfinite(r.z));
    CHECK(std::isfinite(r.w));
    CHECK(glm::length(r) == doctest::Approx(1.0f).epsilon(1e-4f));

    // The forward still resolves and points at the focus.
    const vec3 forward = r * vec3(0.0f, 0.0f, -1.0f);
    CHECK(VecApprox(forward, glm::normalize(focus - result.Position)));
}
