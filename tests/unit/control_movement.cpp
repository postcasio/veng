// Input → Intent → Movement: the engine movement integration (Intent → Transform via
// Mover) and the control-mapping uniformity it buys. Pure CPU — no Context, no Vulkan
// symbol touched; builds a real Scene over a TypeRegistry and drives the real
// MovementSystem.
//
// The control mapping itself (PlayerInput → Intent) is game policy living in the example,
// so this suite exercises the engine half and the abstract-producer uniformity: a control
// system, an AI system, and a raw Intent write all drive the same movement result. The
// always-present headless Input (Input(nullptr), all-zeros) is constructed directly to
// prove a neutral reading produces a zero Intent and no motion.

#include <doctest/doctest.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <Veng/Input.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Movement.h>
#include <Veng/Scene/Resolve.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SceneSystem.h>

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

    // A SystemContext over a real headless Input (all-zeros) and never-dereferenced asset
    // storage. The movement system ignores the context; the control test reads the Input.
    struct ContextStorage
    {
        Input HeadlessInput{nullptr};
        alignas(16) unsigned char AssetsBytes[64]{};

        SystemContext Make()
        {
            return SystemContext{
                .Assets = *reinterpret_cast<AssetManager*>(AssetsBytes),
                .Input = HeadlessInput,
            };
        }
    };

    // The control mapping under test mirrors the example's PlayerInput → Intent policy,
    // so this suite can assert the produced Intent without linking the game module.
    Intent MapInputToIntent(const PlayerInput& input)
    {
        Intent intent;
        intent.Move = input.Move;
        intent.Look = input.Look;
        intent.Actions = input.Buttons;
        return intent;
    }

    Entity MakePawn(Scene& scene, vec3 position, const Mover& mover)
    {
        const Entity pawn = scene.CreateEntity();
        Transform transform;
        transform.Position = position;
        scene.Add<Transform>(pawn, transform);
        scene.Add<Intent>(pawn, Intent{});
        scene.Add<Mover>(pawn, mover);
        return pawn;
    }
}

TEST_CASE("IntegrateMovement scales the local move by delta and speed")
{
    Transform transform;
    const Mover mover{.MoveSpeed = 3.0f, .TurnSpeed = 1.0f};
    const Intent intent{.Move = vec3(0.0f, 0.0f, 1.0f)};

    IntegrateMovement(transform, intent, mover, 0.5f);

    // No rotation, so local +Z maps straight to world +Z: 1 * 3 * 0.5 = 1.5.
    CHECK(VecApprox(transform.Position, vec3(0.0f, 0.0f, 1.5f)));
}

TEST_CASE("IntegrateMovement rotates the local move into the transform's orientation")
{
    Transform transform;
    // Face the pawn 90 degrees about world up, so its local +Z points along world +X.
    transform.Rotation = glm::angleAxis(glm::radians(90.0f), vec3(0.0f, 1.0f, 0.0f));
    const Mover mover{.MoveSpeed = 2.0f, .TurnSpeed = 1.0f};
    const Intent intent{.Move = vec3(0.0f, 0.0f, 1.0f)};

    IntegrateMovement(transform, intent, mover, 1.0f);

    CHECK(VecApprox(transform.Position, vec3(2.0f, 0.0f, 0.0f)));
}

TEST_CASE("IntegrateMovement yaws by the look delta times turn speed")
{
    Transform transform;
    const Mover mover{.MoveSpeed = 1.0f, .TurnSpeed = 2.0f};
    const Intent intent{.Look = vec2(0.5f, 0.0f)};

    IntegrateMovement(transform, intent, mover, 1.0f);

    // Yaw angle = look.x * turnSpeed * delta = 0.5 * 2 * 1 = 1 radian about world up.
    const quat expected = glm::angleAxis(1.0f, vec3(0.0f, 1.0f, 0.0f));
    CHECK(glm::abs(glm::dot(transform.Rotation, expected)) == doctest::Approx(1.0f).epsilon(1e-4f));
}

TEST_CASE("IntegrateMovement with a zero Intent leaves the transform unchanged")
{
    Transform transform;
    transform.Position = vec3(2.0f, 3.0f, 4.0f);
    transform.Rotation = glm::angleAxis(0.3f, glm::normalize(vec3(1.0f, 1.0f, 0.0f)));
    const Transform before = transform;

    IntegrateMovement(transform, Intent{}, Mover{}, 0.5f);

    CHECK(VecApprox(transform.Position, before.Position));
    CHECK(glm::abs(glm::dot(transform.Rotation, before.Rotation)) ==
          doctest::Approx(1.0f).epsilon(1e-5f));
}

TEST_CASE("MovementSystem integrates each pawn's Intent through its Mover")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    const Entity pawn = MakePawn(*scene, vec3(0.0f), Mover{.MoveSpeed = 4.0f, .TurnSpeed = 1.0f});
    scene->Get<Intent>(pawn).Move = vec3(0.0f, 0.0f, 1.0f);

    MovementSystem movement;
    ContextStorage storage;
    movement.OnUpdate(*scene, 0.25f, storage.Make());

    CHECK(VecApprox(scene->Get<Transform>(pawn).Position, vec3(0.0f, 0.0f, 1.0f)));
}

TEST_CASE("MovementSystem falls back to a default Mover when a pawn has none")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    const Entity pawn = scene->CreateEntity();
    scene->Add<Transform>(pawn, Transform{});
    scene->Add<Intent>(pawn, Intent{.Move = vec3(0.0f, 0.0f, 1.0f)});

    MovementSystem movement;
    ContextStorage storage;
    movement.OnUpdate(*scene, 1.0f, storage.Make());

    // The default Mover's MoveSpeed (4.0) drives the integration.
    CHECK(VecApprox(scene->Get<Transform>(pawn).Position, vec3(0.0f, 0.0f, 4.0f)));
}

TEST_CASE("Control mapping turns a synthetic PlayerInput into the expected Intent")
{
    const PlayerInput input{
        .Move = vec3(1.0f, 0.0f, -1.0f), .Look = vec2(0.2f, -0.1f), .Buttons = 0b101u};
    const Intent intent = MapInputToIntent(input);

    CHECK(VecApprox(intent.Move, vec3(1.0f, 0.0f, -1.0f)));
    CHECK(intent.Look.x == doctest::Approx(0.2f));
    CHECK(intent.Look.y == doctest::Approx(-0.1f));
    CHECK(intent.Actions == 0b101u);
}

TEST_CASE("A headless Input reads all-zeros, so the produced Intent is zero and nothing moves")
{
    // The control system reads Veng::Input unconditionally; the headless service reports the
    // neutral all-zeros state, identical to an idle windowed frame.
    const Input headless{nullptr};
    PlayerInput captured;
    captured.Move = vec3(headless.IsKeyDown(Key::D) ? 1.0f : 0.0f, 0.0f,
                         headless.IsKeyDown(Key::W) ? 1.0f : 0.0f);
    captured.Look = headless.GetMouseDelta();

    const Intent intent = MapInputToIntent(captured);
    CHECK(VecApprox(intent.Move, vec3(0.0f)));
    CHECK(intent.Look.x == doctest::Approx(0.0f));
    CHECK(intent.Look.y == doctest::Approx(0.0f));

    // Feeding that zero Intent through the movement system leaves the pawn still.
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);
    const Entity pawn = MakePawn(*scene, vec3(1.0f, 2.0f, 3.0f), Mover{});
    scene->Get<Intent>(pawn) = intent;

    MovementSystem movement;
    ContextStorage storage;
    movement.OnUpdate(*scene, 0.5f, storage.Make());

    CHECK(VecApprox(scene->Get<Transform>(pawn).Position, vec3(1.0f, 2.0f, 3.0f)));
}

namespace
{
    // An AI producer: writes an Intent directly, with no PlayerInput, no Possesses, no
    // player at all — proving the movement system is agnostic to who produced the Intent.
    class AiSystem final : public SceneSystem
    {
    public:
        void OnUpdate(Scene& scene, const f32, const SystemContext&) override
        {
            scene.Each<Intent>([](Entity, Intent& intent)
                               { intent.Move = vec3(0.0f, 0.0f, 1.0f); });
        }
    };
}

TEST_CASE("AI uniformity: a system writing Intent directly drives the same movement")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    const Entity pawn = MakePawn(*scene, vec3(0.0f), Mover{.MoveSpeed = 2.0f, .TurnSpeed = 1.0f});

    AiSystem ai;
    MovementSystem movement;
    ContextStorage storage;

    ai.OnUpdate(*scene, 1.0f, storage.Make());
    movement.OnUpdate(*scene, 1.0f, storage.Make());

    // Same result a player-produced Intent would give: 1 * 2 * 1 along local +Z.
    CHECK(VecApprox(scene->Get<Transform>(pawn).Position, vec3(0.0f, 0.0f, 2.0f)));
}

TEST_CASE("Moving a possessed pawn does not change a Viewer's resolved camera")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    // A camera entity at a fixed pose, and a separate seat that both views through it and
    // possesses a movable pawn — possession and view are independent references.
    const Entity camera = scene->CreateEntity();
    Transform cameraTransform;
    cameraTransform.Position = vec3(0.0f, 5.0f, 10.0f);
    scene->Add<Transform>(camera, cameraTransform);
    scene->Add<Camera>(camera, Camera{});

    const Entity pawn = MakePawn(*scene, vec3(0.0f), Mover{.MoveSpeed = 4.0f, .TurnSpeed = 1.0f});
    scene->Get<Intent>(pawn).Move = vec3(1.0f, 0.0f, 0.0f);

    const Entity seat = scene->CreateEntity();
    scene->Add<Viewer>(seat, Viewer{.Camera = camera});
    scene->Add<Possesses>(seat, Possesses{.Pawn = pawn});

    const optional<CameraView> before = ResolveCameraView(*scene, seat, 1.0f);
    REQUIRE(before.has_value());

    MovementSystem movement;
    ContextStorage storage;
    movement.OnUpdate(*scene, 1.0f, storage.Make());

    // The pawn moved, but the camera entity is untouched, so the resolved view is identical.
    REQUIRE_FALSE(VecApprox(scene->Get<Transform>(pawn).Position, vec3(0.0f)));
    const optional<CameraView> after = ResolveCameraView(*scene, seat, 1.0f);
    REQUIRE(after.has_value());
    CHECK(VecApprox(after->GetPosition(), before->GetPosition()));
}
