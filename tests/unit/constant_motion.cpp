// ConstantMotion: the engine's autonomous constant-velocity integration (a drift + spin
// rate of change of the Transform) and the local/world frame distinction. Pure CPU — no
// Context, no Vulkan symbol touched; the deterministic core IntegrateConstantMotion is
// exercised directly, then the real ConstantMotionSystem over a Scene.

#include <doctest/doctest.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Motion.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SceneSystem.h>

using namespace Veng;

namespace
{
    bool VecApprox(const vec3& a, const vec3& b, const f32 eps = 1e-4f)
    {
        return glm::all(glm::lessThan(glm::abs(a - b), vec3(eps)));
    }

    bool QuatApprox(const quat& a, const quat& b, const f32 eps = 1e-4f)
    {
        // Quaternions double-cover rotations, so q and -q are equal; compare by |dot|.
        return glm::abs(glm::dot(a, b)) == doctest::Approx(1.0f).epsilon(eps);
    }

    // A SystemContext the motion system never reads: it touches neither the Input nor the
    // AssetManager, so backing storage is never dereferenced.
    struct ContextStorage
    {
        alignas(16) unsigned char InputBytes[64]{};
        alignas(16) unsigned char AssetsBytes[64]{};

        SystemContext Make()
        {
            return SystemContext{
                .Assets = *reinterpret_cast<AssetManager*>(AssetsBytes),
                .Input = *reinterpret_cast<Input*>(InputBytes),
            };
        }
    };
}

TEST_CASE("IntegrateConstantMotion scales the world linear velocity by delta")
{
    Transform transform;
    const ConstantMotion motion{.LinearVelocity = vec3(0.0f, 0.0f, 2.0f),
                                .Space = MotionSpace::World};

    IntegrateConstantMotion(transform, motion, 0.5f);

    // World space: the velocity adds directly, 2 * 0.5 = 1 along world +Z.
    CHECK(VecApprox(transform.Position, vec3(0.0f, 0.0f, 1.0f)));
}

TEST_CASE("IntegrateConstantMotion applies a local linear velocity in the entity's frame")
{
    Transform transform;
    // Face 90 degrees about world up, so the entity's local +Z points along world +X.
    transform.Rotation = glm::angleAxis(glm::radians(90.0f), vec3(0.0f, 1.0f, 0.0f));
    const ConstantMotion motion{.LinearVelocity = vec3(0.0f, 0.0f, 1.0f),
                                .Space = MotionSpace::Local};

    IntegrateConstantMotion(transform, motion, 1.0f);

    CHECK(VecApprox(transform.Position, vec3(1.0f, 0.0f, 0.0f)));
}

TEST_CASE("IntegrateConstantMotion spins by the angular velocity magnitude as radians/sec")
{
    Transform transform;
    // Axis-angle vector: 0.5 rad/sec about world up.
    const ConstantMotion motion{.AngularVelocity = vec3(0.0f, 0.5f, 0.0f),
                                .Space = MotionSpace::World};

    IntegrateConstantMotion(transform, motion, 2.0f);

    // 0.5 * 2 = 1 radian about world up.
    const quat expected = glm::angleAxis(1.0f, vec3(0.0f, 1.0f, 0.0f));
    CHECK(QuatApprox(transform.Rotation, expected));
}

TEST_CASE("Local and world spin differ once the entity is already rotated")
{
    // Start tilted so local and parent axes diverge; spin about Y in each frame.
    const quat start = glm::angleAxis(glm::radians(90.0f), vec3(1.0f, 0.0f, 0.0f));
    const vec3 angular = vec3(0.0f, 1.0f, 0.0f);

    Transform world;
    world.Rotation = start;
    IntegrateConstantMotion(world, {.AngularVelocity = angular, .Space = MotionSpace::World}, 1.0f);

    Transform local;
    local.Rotation = start;
    IntegrateConstantMotion(local, {.AngularVelocity = angular, .Space = MotionSpace::Local}, 1.0f);

    // World pre-multiplies (about the parent Y), local post-multiplies (about the entity's
    // own Y); from a tilted start the two land on distinct orientations.
    const quat step = glm::angleAxis(1.0f, angular);
    CHECK(QuatApprox(world.Rotation, glm::normalize(step * start)));
    CHECK(QuatApprox(local.Rotation, glm::normalize(start * step)));
    CHECK_FALSE(QuatApprox(world.Rotation, local.Rotation));
}

TEST_CASE("IntegrateConstantMotion with zero velocities leaves the transform unchanged")
{
    Transform transform;
    transform.Position = vec3(2.0f, 3.0f, 4.0f);
    transform.Rotation = glm::angleAxis(0.3f, glm::normalize(vec3(1.0f, 1.0f, 0.0f)));
    const Transform before = transform;

    IntegrateConstantMotion(transform, ConstantMotion{}, 0.5f);

    CHECK(VecApprox(transform.Position, before.Position));
    CHECK(QuatApprox(transform.Rotation, before.Rotation, 1e-5f));
}

TEST_CASE("ConstantMotionSystem integrates every (Transform, ConstantMotion) entity")
{
    TypeRegistry registry;
    RegisterBuiltinTypes(registry);
    const Unique<Scene> scene = Scene::Create(registry);

    const Entity spinner = scene->CreateEntity();
    scene->Add<Transform>(spinner, Transform{});
    scene->Add<ConstantMotion>(spinner, ConstantMotion{.AngularVelocity = vec3(0.0f, 1.0f, 0.0f),
                                                       .Space = MotionSpace::World});

    // An entity without a ConstantMotion is left alone by the query.
    const Entity still = scene->CreateEntity();
    scene->Add<Transform>(still, Transform{});

    ConstantMotionSystem motion;
    ContextStorage storage;
    motion.OnUpdate(*scene, 0.25f, storage.Make());

    const quat expected = glm::angleAxis(0.25f, vec3(0.0f, 1.0f, 0.0f));
    CHECK(QuatApprox(scene->Get<Transform>(spinner).Rotation, expected));
    CHECK(QuatApprox(scene->Get<Transform>(still).Rotation, quat(1.0f, 0.0f, 0.0f, 0.0f)));
}
