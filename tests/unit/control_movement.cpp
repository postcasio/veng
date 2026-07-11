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

#include <algorithm>
#include <array>

#include <Veng/Asset/InputMappingContext.h>
#include <Veng/Input.h>
#include <Veng/Input/Actions.h>
#include <Veng/InputEvents.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/InputMappingSystem.h>
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

    // The game's named actions, mirroring the example so the produced Intent is asserted
    // without linking the game module. Arbitrary distinct non-zero ids.
    constexpr ActionId Move{0xA1};
    constexpr ActionId Look{0xB2};
    constexpr ActionId Jump{0xC3};

    // Builds a resolved PlayerInput carrying the given action values — the shape the engine's
    // InputMappingSystem produces from a seat's contexts, hand-built here so the mapping is
    // testable without the resolver.
    PlayerInput MakeInput(const vec2 move, const vec2 look, const bool jump)
    {
        PlayerInput input;
        input.State.Actions = {
            ActionSample{.Id = Move, .Value = move, .Phase = ActionPhase::Ongoing},
            ActionSample{.Id = Look, .Value = look, .Phase = ActionPhase::Ongoing},
            ActionSample{.Id = Jump,
                         .Value = vec2(jump ? 1.0f : 0.0f, 0.0f),
                         .Phase = jump ? ActionPhase::Ongoing : ActionPhase::None}};
        return input;
    }

    // The control mapping under test mirrors the example's PlayerInput → Intent policy,
    // reading actions by name, so this suite asserts the produced Intent without the game
    // module. Move.x strafes, Move.y advances (mapped to -Z, the pawn's forward); only the
    // yaw drives the pawn (pitch tilts the follow camera).
    Intent MapInputToIntent(const PlayerInput& input)
    {
        constexpr f32 YawSensitivity = 0.05f;
        const vec2 move = input.GetValue(Move);
        const vec2 look = input.GetValue(Look);

        Intent intent;
        intent.Move = vec3(move.x, 0.0f, -move.y);
        intent.Look = vec2(-look.x * YawSensitivity, 0.0f);
        intent.Actions = input.IsHeld(Jump) ? 1u : 0u;
        return intent;
    }

    // Keyboard control codes the fake and the bindings agree on.
    constexpr u32 KeyW = 1;
    constexpr u32 KeyD = 2;

    // A scripted raw input surface: keys down are a set, neutral by default.
    struct FakeRawInput final : RawInputView
    {
        vector<u32> KeysDown;

        [[nodiscard]] bool IsKeyDown(u32 code) const override
        {
            return std::ranges::find(KeysDown, code) != KeysDown.end();
        }
        [[nodiscard]] bool IsButtonDown(InputDeviceType, u32) const override { return false; }
        [[nodiscard]] f32 GetAxis(InputDeviceType, u32) const override { return 0.0f; }
    };

    // A WASD → 2D Move + Jump context mirroring the example's gameplay bindings.
    ResolvedContext GameplayContext()
    {
        return ResolvedContext{
            .Actions = {InputAction{.Id = Move, .Name = "Move", .Kind = ActionKind::Axis2D},
                        InputAction{.Id = Jump, .Name = "Jump", .Kind = ActionKind::Button}},
            .Bindings = {Binding{.Source = {.Device = InputDeviceType::Keyboard, .Control = KeyD},
                                 .Action = Move,
                                 .Axis = AxisComponent::X,
                                 .Scale = 1.0f},
                         Binding{.Source = {.Device = InputDeviceType::Keyboard, .Control = KeyW},
                                 .Action = Move,
                                 .Axis = AxisComponent::Y,
                                 .Scale = 1.0f}}};
    }

    // Builds a resident AssetHandle<InputMappingContext> for the given context, without an
    // AssetManager: a detached cache entry holding the constructed context, wired into a
    // type-erased handle the way a prefab spawn rehydrates one. This lets the system test seed a
    // seat's InputContextStack without a device or a real asset load.
    AssetHandle<InputMappingContext> MakeResidentContext(const ResolvedContext& context)
    {
        const Ref<InputMappingContext> resource = InputMappingContext::Create(
            vector<InputAction>(context.Actions.begin(), context.Actions.end()),
            vector<Binding>(context.Bindings.begin(), context.Bindings.end()));
        auto entry = CreateRef<Detail::AssetCacheEntry>(Detail::AssetCacheEntry{
            .Id = AssetId{}, .Type = AssetType::InputMap, .Resource = Detail::RefAny(resource)});

        AssetHandle<InputMappingContext> handle;
        Detail::RehydrateHandleField(&handle, AssetId{}, std::move(entry));
        return handle;
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

TEST_CASE("Control mapping turns a resolved PlayerInput into the expected Intent")
{
    // Move.x = strafe right, Move.y = advance forward; forward maps to the pawn's local -Z.
    const PlayerInput input = MakeInput(vec2(1.0f, 1.0f), vec2(0.2f, -0.1f), true);
    const Intent intent = MapInputToIntent(input);

    CHECK(VecApprox(intent.Move, vec3(1.0f, 0.0f, -1.0f)));
    // Yaw is the negated mouse-X delta scaled by the sensitivity; pitch does not drive the pawn.
    CHECK(intent.Look.x == doctest::Approx(-0.2f * 0.05f));
    CHECK(intent.Look.y == doctest::Approx(0.0f));
    CHECK(intent.Actions == 1u);
}

TEST_CASE("A neutral resolved PlayerInput produces a zero Intent and nothing moves")
{
    // In headless the InputMappingSystem resolves every action to None over the neutral
    // snapshot, so the seat's PlayerInput carries no active actions — an empty ActionState.
    const PlayerInput neutral;

    const Intent intent = MapInputToIntent(neutral);
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

VE_SYSTEM(AiSystem, 0x5025CEE0E52DBA62ULL, "AI");

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

TEST_CASE("End to end: scripted raw input resolves into PlayerInput and maps to the pawn's Intent")
{
    // The joined-up path the pure action_resolve suite does not cover: resolve a scripted
    // raw snapshot against a seat's context into a PlayerInput, then map it to an Intent.
    // D + W presses drive Move right and forward; forward maps to the pawn's local -Z.
    FakeRawInput raw;
    raw.KeysDown = {KeyD, KeyW};

    const ResolvedContext context = GameplayContext();
    const std::array active{context};

    PlayerInput player;
    player.State = ResolveActions(active, raw, player.State);

    CHECK(player.GetValue(Move).x == doctest::Approx(1.0f));
    CHECK(player.GetValue(Move).y == doctest::Approx(1.0f));

    const Intent intent = MapInputToIntent(player);
    CHECK(VecApprox(intent.Move, vec3(1.0f, 0.0f, -1.0f)));

    // Feeding the produced Intent through the movement system moves the pawn on -Z (forward).
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);
    const Entity pawn = MakePawn(*scene, vec3(0.0f), Mover{.MoveSpeed = 2.0f, .TurnSpeed = 1.0f});
    scene->Get<Intent>(pawn) = intent;

    MovementSystem movement;
    ContextStorage storage;
    movement.OnUpdate(*scene, 1.0f, storage.Make());

    // Move = (1,0,-1) at speed 2 over 1s, no rotation: +2 on X, -2 on Z.
    CHECK(VecApprox(scene->Get<Transform>(pawn).Position, vec3(2.0f, 0.0f, -2.0f)));
}

TEST_CASE("InputMappingSystem resolves each seat's PlayerInput; a neutral snapshot yields all-None")
{
    // The system wiring itself, driven with a real headless Input (all-zeros). The seat carries
    // a Viewer + InputContextStack + PlayerInput + SeatInput; the neutral snapshot resolves every
    // declared action to a None sample with zero value — the headless contract with no guard. The
    // SeatInput is what marks the seat as reading local devices, so InputMappingSystem resolves it.
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    const Entity seat = scene->CreateEntity();
    scene->Add<Viewer>(seat, Viewer{});
    scene->Add<PlayerInput>(seat, PlayerInput{});
    scene->Add<InputContextStack>(
        seat, InputContextStack{.Active = {MakeResidentContext(GameplayContext())}});
    scene->Add<SeatInput>(seat, SeatInput{});

    InputMappingSystem mapping;
    ContextStorage storage;
    mapping.OnUpdate(*scene, 0.016f, storage.Make());

    const PlayerInput& resolved = scene->Get<PlayerInput>(seat);
    // Both declared actions get a sample even with no active binding.
    REQUIRE(resolved.State.Actions.size() == 2);
    CHECK(VecApprox(vec3(resolved.GetValue(Move), 0.0f), vec3(0.0f)));
    CHECK_FALSE(resolved.IsHeld(Jump));
    CHECK_FALSE(resolved.WasTriggered(Jump));
}

TEST_CASE("InputMappingSystem accumulates a release edge across a multi-step frame")
{
    // A once-per-frame reader (a View system) must still see a release that a later Sim step of the
    // same frame overwrote in Phase. The system ORs each step's edge into ReleasedThisFrame and
    // resets it on the frame's first step (SystemContext::FirstStepThisFrame), so
    // WasReleasedThisFrame survives where the per-tick WasReleased does not.
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    // A context binding Jump to a real key so the headless Input can drive it by event.
    const ResolvedContext jumpContext{
        .Actions = {InputAction{.Id = Jump, .Name = "Jump", .Kind = ActionKind::Button}},
        .Bindings = {Binding{.Source = {.Device = InputDeviceType::Keyboard,
                                        .Control = static_cast<u32>(Key::Space)},
                             .Action = Jump,
                             .Axis = AxisComponent::Whole,
                             .Scale = 1.0f}}};

    const Entity seat = scene->CreateEntity();
    scene->Add<Viewer>(seat, Viewer{});
    scene->Add<PlayerInput>(seat, PlayerInput{});
    scene->Add<InputContextStack>(seat,
                                  InputContextStack{.Active = {MakeResidentContext(jumpContext)}});
    scene->Add<SeatInput>(seat, SeatInput{});

    InputMappingSystem mapping;
    ContextStorage storage;
    Input& input = storage.HeadlessInput;

    const auto tick = [&](const bool firstStep)
    {
        SystemContext context = storage.Make();
        context.FirstStepThisFrame = firstStep;
        mapping.OnUpdate(*scene, 0.016f, context);
    };
    const PlayerInput& resolved = scene->Get<PlayerInput>(seat);

    // Frame 0: press Space and run one step — Jump is held.
    input.BeginFrame(true);
    input.ApplyEvent(KeyPressedEvent(Key::Space, 0, 0));
    tick(/*firstStep=*/true);
    CHECK(resolved.IsHeld(Jump));

    // Frame 1 rolls (committing the press), then Space releases. Two Sim steps run this frame with
    // the button up on both: step 0 sees the Completed edge, step 1 overwrites Phase to None.
    input.BeginFrame(true);
    input.ApplyEvent(KeyReleasedEvent(Key::Space, 0, 0));
    tick(/*firstStep=*/true);
    tick(/*firstStep=*/false);

    // The per-tick edge was overwritten by the second step — the symptom a once-per-frame reader
    // hit — but the frame-accumulated edge survived, so the release is observable after the ticks.
    CHECK_FALSE(resolved.WasReleased(Jump));
    CHECK(resolved.WasReleasedThisFrame(Jump));
    CHECK_FALSE(resolved.IsHeld(Jump));

    // Frame 2: the accumulator resets on the first step, so a fresh frame with no new edge is clean.
    input.BeginFrame(true);
    tick(/*firstStep=*/true);
    CHECK_FALSE(resolved.WasReleasedThisFrame(Jump));
}

TEST_CASE(
    "A PlayerInput entity round-trips through the reflection serializer's new ActionState shape")
{
    const TypeRegistry registry = MakeRegistry();

    // A resolved PlayerInput carrying a Move axis and a triggered Jump — the shape a prefab
    // persists and a net layer replicates.
    PlayerInput src;
    src.State.Actions = {
        ActionSample{.Id = Move, .Value = vec2(0.5f, -0.25f), .Phase = ActionPhase::Ongoing},
        ActionSample{.Id = Jump, .Value = vec2(1.0f, 0.0f), .Phase = ActionPhase::Started}};

    vector<u8> bytes;
    WriteFields(bytes, &src, registry.Info(registry.IdOf<PlayerInput>()), registry);

    PlayerInput dst;
    const VoidResult read =
        ReadFields(bytes, &dst, registry.Info(registry.IdOf<PlayerInput>()), registry);
    REQUIRE(read.has_value());

    REQUIRE(dst.State.Actions.size() == 2);
    CHECK(dst.GetValue(Move).x == doctest::Approx(0.5f));
    CHECK(dst.GetValue(Move).y == doctest::Approx(-0.25f));
    CHECK(dst.WasTriggered(Jump));
    CHECK(dst.IsHeld(Jump));
}
