// The first-person camera rig: the pure basis/camera math (FirstPersonBasis / FirstPersonCamera)
// and the View-phase CameraRigSystem arm that drives it from a target's resolved up. The signature
// property is that the horizon stays level when "up" is not a world constant — proven at 32 sample
// up-vectors over a sphere and around a full lap inside an Axial gravity field, the case a rig
// yawing about world up cannot hold. Pure CPU: a device-free Scene, a default Renderer::Context for
// the socket case (no Vulkan device touched), and the real system through a SceneSimulation.

#include <doctest/doctest.h>

#include <array>
#include <cmath>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Physics/CharacterController.h>
#include <Veng/Physics/Gravity.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/CameraRig.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/Transforms.h>
#include <Veng/Task/TaskSystem.h>

using namespace Veng;

namespace
{
    bool VecApprox(const vec3& a, const vec3& b, const f32 eps = 1e-4f)
    {
        return glm::all(glm::lessThan(glm::abs(a - b), vec3(eps)));
    }

    // 32 roughly-even directions over the sphere (a Fibonacci lattice) — the up-vector sample set
    // the basis property is checked across.
    std::array<vec3, 32> SphereSamples()
    {
        std::array<vec3, 32> out{};
        constexpr f32 golden = 2.399963f; // the golden angle, radians
        for (u32 i = 0; i < 32; ++i)
        {
            const f32 z = 1.0f - 2.0f * (static_cast<f32>(i) + 0.5f) / 32.0f;
            const f32 r = std::sqrt(glm::max(0.0f, 1.0f - z * z));
            const f32 phi = golden * static_cast<f32>(i);
            out[i] = vec3(r * std::cos(phi), r * std::sin(phi), z);
        }
        return out;
    }

    // Any unit vector perpendicular to `up` — a plausible target forward for a basis sample.
    vec3 PerpendicularTo(const vec3 up)
    {
        const vec3 seed = std::abs(up.x) < 0.9f ? vec3(1.0f, 0.0f, 0.0f) : vec3(0.0f, 1.0f, 0.0f);
        return glm::normalize(seed - glm::dot(seed, up) * up);
    }

    // A SystemContext over never-dereferenced storage: the camera rig ignores the context entirely.
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
            };
        }
    };
}

TEST_CASE("FirstPersonBasis is orthonormal with a level horizon at 32 up-vectors over a sphere")
{
    for (const vec3& up : SphereSamples())
    {
        const vec3 forward = PerpendicularTo(up);
        const CameraBasis basis = FirstPersonBasis(up, forward, 0.7f, 0.3f, -1.4f, 1.4f);

        // Unit length.
        CHECK(glm::length(basis.Right) == doctest::Approx(1.0f).epsilon(1e-4f));
        CHECK(glm::length(basis.Up) == doctest::Approx(1.0f).epsilon(1e-4f));
        CHECK(glm::length(basis.Forward) == doctest::Approx(1.0f).epsilon(1e-4f));

        // Mutually perpendicular.
        CHECK(glm::dot(basis.Right, basis.Up) == doctest::Approx(0.0f).epsilon(1e-4f));
        CHECK(glm::dot(basis.Right, basis.Forward) == doctest::Approx(0.0f).epsilon(1e-4f));
        CHECK(glm::dot(basis.Up, basis.Forward) == doctest::Approx(0.0f).epsilon(1e-4f));

        // The horizon (right axis) is perpendicular to the character's up — a level horizon, and
        // the camera up coplanar with the character's up. This is the property that fails for a rig
        // yawing about world up once the character's up leaves world +Y.
        CHECK(glm::dot(basis.Right, glm::normalize(up)) == doctest::Approx(0.0f).epsilon(1e-4f));
    }
}

TEST_CASE("FirstPersonBasis clamps the pitch into its band, including at near-vertical ups")
{
    const std::array<vec3, 4> ups{vec3(0.0f, 1.0f, 0.0f), vec3(1.0f, 0.0f, 0.0f),
                                  glm::normalize(vec3(0.1f, 0.99f, 0.0f)),
                                  glm::normalize(vec3(0.0f, -1.0f, 0.05f))};
    for (const vec3& up : ups)
    {
        const vec3 forward = PerpendicularTo(up);

        // A pitch far past the band resolves as if it were exactly the limit: the forward's
        // elevation above the horizon plane is sin(clampedPitch).
        const CameraBasis high = FirstPersonBasis(up, forward, 0.0f, 3.0f, -1.2f, 1.2f);
        CHECK(std::asin(glm::clamp(glm::dot(high.Forward, glm::normalize(up)), -1.0f, 1.0f)) ==
              doctest::Approx(1.2f).epsilon(1e-3f));

        const CameraBasis low = FirstPersonBasis(up, forward, 0.0f, -3.0f, -1.2f, 1.2f);
        CHECK(std::asin(glm::clamp(glm::dot(low.Forward, glm::normalize(up)), -1.0f, 1.0f)) ==
              doctest::Approx(-1.2f).epsilon(1e-3f));

        // Even at a near-vertical up the basis stays finite and orthonormal.
        CHECK(std::isfinite(high.Forward.x));
        CHECK(glm::dot(high.Right, high.Up) == doctest::Approx(0.0f).epsilon(1e-4f));
    }
}

TEST_CASE("A rig walked a full lap inside an Axial field keeps a level horizon and returns its yaw")
{
    // A ring habitat with a tilted axis: Axial gravity about a spin axis skew to world up, so up is
    // radially inward on the inner wall and sweeps through every direction as the character walks
    // the lap. The rig's horizon (its right axis) must stay along the ring axis — level — at every
    // sample, and the camera forward must return to its start after a full lap. A world-up rig
    // rolls the horizon somewhere on the lap, which is asserted against alongside to prove the test
    // discriminates; the axis is tilted precisely so world up is not trivially perpendicular to the
    // motion (an axis-aligned ring would let a world-up rig pass by accident).
    constexpr f32 Radius = 4.0f;
    const vec3 Axis = glm::normalize(vec3(1.0f, 2.0f, 2.0f));
    // Two orthonormal spokes spanning the ring plane (perpendicular to the axis).
    const vec3 spokeU = glm::normalize(glm::cross(Axis, vec3(1.0f, 0.0f, 0.0f)));
    const vec3 spokeV = glm::cross(Axis, spokeU);
    const GravitySourceInstance source{
        .Kind = GravityKind::Axial,
        .Direction = Axis,
        .Origin = vec3(0.0f),
        .Magnitude = 9.81f,
        .Bounds = Region{.HalfExtents = vec3(100.0f)},
    };
    const std::array<GravitySourceInstance, 1> sources{source};

    constexpr u32 Samples = 256;
    vec3 firstForward(0.0f);
    vec3 previousForward(0.0f);
    bool worldUpEverTilted = false;
    for (u32 i = 0; i <= Samples; ++i)
    {
        const f32 angle = glm::two_pi<f32>() * static_cast<f32>(i) / static_cast<f32>(Samples);
        const vec3 outward = std::cos(angle) * spokeU + std::sin(angle) * spokeV;
        const vec3 position = Radius * outward;
        const vec3 up = -glm::normalize(EvaluateGravity(sources, position));
        const vec3 tangent =
            -std::sin(angle) * spokeU + std::cos(angle) * spokeV; // walking forward

        const CameraBasis basis = FirstPersonBasis(up, tangent, 0.0f, 0.0f, -1.4f, 1.4f);

        // The horizon is perpendicular to the character's up at every point on the lap — the ring
        // axis, in fact — so the view never rolls no matter where on the wall the character stands.
        CHECK(glm::dot(basis.Right, up) == doctest::Approx(0.0f).epsilon(1e-3f));
        CHECK(std::abs(glm::dot(basis.Right, Axis)) == doctest::Approx(1.0f).epsilon(1e-3f));

        // The counter-example: a rig taking right = cross(forward, worldUp) tilts off the
        // character's up somewhere on the lap. Record that it does, to prove the test discriminates.
        const vec3 worldUpRight = glm::normalize(glm::cross(tangent, vec3(0.0f, 1.0f, 0.0f)));
        if (std::abs(glm::dot(worldUpRight, up)) > 0.1f)
        {
            worldUpEverTilted = true;
        }

        if (i == 0)
        {
            firstForward = basis.Forward;
        }
        else
        {
            // No discontinuity: the forward turns by a small amount each sample, never jumps.
            CHECK(glm::dot(basis.Forward, previousForward) > 0.99f);
        }
        previousForward = basis.Forward;

        if (i == Samples)
        {
            // Accumulated yaw returns to its starting value after the full lap.
            CHECK(VecApprox(basis.Forward, firstForward, 1e-3f));
        }
    }
    CHECK(worldUpEverTilted);
}

TEST_CASE("FirstPersonCamera anchors the eye at the offset and bobs only when it has amplitude")
{
    const FirstPersonRig rig{.EyeOffset = vec3(0.0f, 1.6f, 0.0f), .BobAmplitude = 0.0f};
    const CameraLook look{};

    // World up, forward -Z, no bob: the eye sits exactly at the passed anchor and looks down -Z.
    const Transform t = FirstPersonCamera(vec3(2.0f, 0.5f, -3.0f), vec3(0.0f, 1.0f, 0.0f),
                                          vec3(0.0f, 0.0f, -1.0f), look, rig, 0.0f);
    CHECK(VecApprox(t.Position, vec3(2.0f, 0.5f, -3.0f)));
    CHECK(VecApprox(t.Rotation * vec3(0.0f, 0.0f, -1.0f), vec3(0.0f, 0.0f, -1.0f)));

    // With amplitude the eye leaves the anchor by up to the amplitude; a quarter phase is a pure
    // vertical peak.
    const FirstPersonRig bobbing{.BobAmplitude = 0.1f};
    const Transform peak =
        FirstPersonCamera(vec3(0.0f), vec3(0.0f, 1.0f, 0.0f), vec3(0.0f, 0.0f, -1.0f), look,
                          bobbing, glm::half_pi<f32>());
    CHECK(peak.Position.y == doctest::Approx(0.1f).epsilon(1e-4f));
}

TEST_CASE("The View-phase rig reads the target's finalized up and writes the camera pose")
{
    TypeRegistry registry;
    RegisterBuiltinTypes(registry);
    const Unique<Scene> scene = Scene::Create(registry);

    // A target rolled so its local +Y (and thus its up) maps to world +X, standing at a known place.
    const Entity target = scene->CreateEntity();
    Transform targetPose;
    targetPose.Position = vec3(5.0f, 0.0f, 0.0f);
    targetPose.Rotation = glm::angleAxis(glm::radians(-90.0f), vec3(0.0f, 0.0f, 1.0f));
    scene->Add<Transform>(target, targetPose);
    scene->Add<CharacterState>(target, CharacterState{.Up = vec3(1.0f, 0.0f, 0.0f)});

    const Entity camera = scene->CreateEntity();
    scene->Add<Transform>(camera, Transform{});
    scene->Add<FirstPersonRig>(camera, FirstPersonRig{.Target = target,
                                                      .EyeOffset = vec3(0.0f, 1.6f, 0.0f),
                                                      .MinPitch = -1.4f,
                                                      .MaxPitch = 1.4f});

    CameraRigSystem rig;
    ContextStorage storage;
    rig.OnUpdate(*scene, 0.016f, storage.Make());

    // EyeOffset is +1.6 along the target's local +Y, which its rotation maps onto world +X: the eye
    // sits 1.6 past the target along +X. The camera up is the target's up (+X), horizon level.
    const Transform& out = scene->Get<Transform>(camera);
    CHECK(VecApprox(out.Position, vec3(6.6f, 0.0f, 0.0f)));
    CHECK(glm::dot(out.Rotation * vec3(0.0f, 1.0f, 0.0f), vec3(1.0f, 0.0f, 0.0f)) > 0.99f);
}

TEST_CASE("The rig leaves a camera with an unwired target untouched")
{
    TypeRegistry registry;
    RegisterBuiltinTypes(registry);
    const Unique<Scene> scene = Scene::Create(registry);

    const Entity camera = scene->CreateEntity();
    Transform pose;
    pose.Position = vec3(1.0f, 2.0f, 3.0f);
    scene->Add<Transform>(camera, pose);
    scene->Add<FirstPersonRig>(camera, FirstPersonRig{.Target = Entity::Null});

    CameraRigSystem rig;
    ContextStorage storage;
    rig.OnUpdate(*scene, 0.016f, storage.Make());

    CHECK(VecApprox(scene->Get<Transform>(camera).Position, vec3(1.0f, 2.0f, 3.0f)));
}

TEST_CASE("A socket-anchored eye resolves to the socket's world position")
{
    // A device-free Mesh carrying an eye socket, adopted resident, on the target — the anchor the
    // rig must resolve to instead of the numeric EyeOffset.
    Renderer::Context context;
    TaskSystem tasks;
    TypeRegistry registry;
    RegisterBuiltinTypes(registry);
    const Unique<AssetManager> assets = CreateUnique<AssetManager>(context, tasks, registry);
    const Unique<Scene> scene = Scene::Create(registry);

    const Ref<Mesh> mesh = Mesh::Create(MeshInfo{
        .Name = "eye",
        .Sockets = {MeshSocket{.Name = "Eye", .Position = vec3(0.0f, 1.7f, 0.2f)}},
    });

    const Entity target = scene->CreateEntity();
    scene->Add<Transform>(target, Transform{.Position = vec3(10.0f, 0.0f, -4.0f)});
    scene->Add<MeshRenderer>(target, MeshRenderer{.Mesh = assets->Adopt<Mesh>(mesh)});

    const Entity camera = scene->CreateEntity();
    scene->Add<Transform>(camera, Transform{});
    scene->Add<FirstPersonRig>(
        camera, FirstPersonRig{.Target = target, .EyeOffset = vec3(0.0f), .EyeSocket = "Eye"});

    CameraRigSystem rig;
    ContextStorage storage;
    rig.OnUpdate(*scene, 0.016f, storage.Make());

    // With a zero EyeOffset the eye lands exactly on the socket's world position: the target's
    // position plus the socket's mesh-space offset (the target is unrotated).
    CHECK(VecApprox(scene->Get<Transform>(camera).Position, vec3(10.0f, 1.7f, -3.8f)));
}
