// Plan 05's client→server input flow, exercised device-free: the redundant per-tick input packet
// codec, the server jitter buffer's feed-at-matching-tick under seeded loss/reorder (the control
// system re-derives the same Intent through the unchanged pipeline), the buffer's slew and phase
// decay, and the authority filter the builtin Sim advancers consult. Pure CPU — no window, no
// device, no Vulkan symbol touched.

#include <doctest/doctest.h>

#include <glm/glm.hpp>

#include <Veng/Net/Replication.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Movement.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SceneSystem.h>

using namespace Veng;

namespace
{
    // Arbitrary distinct non-zero action ids, mirroring a game's minted actions.
    constexpr ActionId MoveAction{0xA1};
    constexpr ActionId JumpAction{0xC3};

    TypeRegistry MakeRegistry()
    {
        TypeRegistry registry;
        RegisterBuiltinTypes(registry);
        return registry;
    }

    // A move-only resolved input for one tick: a held 2D Move plus a jump at the given phase.
    ActionState MakeState(const vec2 move, const ActionPhase jumpPhase = ActionPhase::None)
    {
        ActionState state;
        state.Actions = {
            ActionSample{.Id = MoveAction, .Value = move, .Phase = ActionPhase::Ongoing},
            ActionSample{.Id = JumpAction,
                         .Value = vec2(jumpPhase == ActionPhase::None ? 0.0f : 1.0f, 0.0f),
                         .Phase = jumpPhase},
        };
        return state;
    }

    bool SameState(const ActionState& a, const ActionState& b)
    {
        if (a.Actions.size() != b.Actions.size())
        {
            return false;
        }
        for (usize i = 0; i < a.Actions.size(); ++i)
        {
            const ActionSample& x = a.Actions[i];
            const ActionSample& y = b.Actions[i];
            if (x.Id != y.Id || x.Phase != y.Phase)
            {
                return false;
            }
            if (glm::any(glm::greaterThan(glm::abs(x.Value - y.Value), vec2(1e-6f))))
            {
                return false;
            }
        }
        return true;
    }

    // A SystemContext over never-dereferenced service storage, with a settable NetRole. HasAuthority
    // (and the movement systems that call it) read only the scene and context.Role.
    struct FakeContext
    {
        alignas(16) unsigned char AssetsBytes[64]{};
        alignas(16) unsigned char InputBytes[64]{};
        alignas(16) unsigned char TasksBytes[64]{};
        NetRole Role = NetRole::Server;

        SystemContext Make()
        {
            return SystemContext{
                .Assets = *reinterpret_cast<AssetManager*>(AssetsBytes),
                .Input = *reinterpret_cast<Input*>(InputBytes),
                .Tasks = *reinterpret_cast<TaskSystem*>(TasksBytes),
                .Role = Role,
            };
        }
    };

    // The game's control mapping under test: PlayerInput → Intent, reading actions by name.
    Intent ControlMap(const PlayerInput& input)
    {
        Intent intent;
        const vec2 move = input.GetValue(MoveAction);
        intent.Move = vec3(move.x, 0.0f, move.y);
        return intent;
    }
}

TEST_CASE("EncodeInputPacket round-trips the ack and a contiguous tick run")
{
    const TypeRegistry registry = MakeRegistry();

    const vector<ActionState> records = {
        MakeState(vec2(1.0f, 0.0f)),
        MakeState(vec2(0.0f, 1.0f), ActionPhase::Started),
        MakeState(vec2(-1.0f, 0.0f)),
    };
    const vector<u8> bytes =
        EncodeInputPacket(/*acked=*/7, /*firstClientTick=*/5, records, registry);

    const Result<InputPacket> decoded = DecodeInputPacket(bytes, registry);
    REQUIRE(decoded.has_value());
    CHECK(decoded->AckedServerTick == 7);
    REQUIRE(decoded->Inputs.size() == 3);
    CHECK(decoded->Inputs[0].ClientTick == 5);
    CHECK(decoded->Inputs[1].ClientTick == 6);
    CHECK(decoded->Inputs[2].ClientTick == 7);
    CHECK(SameState(decoded->Inputs[0].State, records[0]));
    CHECK(SameState(decoded->Inputs[1].State, records[1]));
    CHECK(SameState(decoded->Inputs[2].State, records[2]));
}

TEST_CASE("InputSendBuffer keeps the last N ticks and encodes them redundantly")
{
    const TypeRegistry registry = MakeRegistry();

    InputSendBuffer send(InputSendBuffer::Settings{.Redundancy = 3});
    for (u64 tick = 0; tick <= 4; ++tick)
    {
        send.Stamp(tick, MakeState(vec2(static_cast<f32>(tick), 0.0f)));
    }
    CHECK(send.Size() == 3);

    const Result<InputPacket> decoded = DecodeInputPacket(send.Encode(0, registry), registry);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->Inputs.size() == 3);
    CHECK(decoded->Inputs[0].ClientTick == 2);
    CHECK(decoded->Inputs[1].ClientTick == 3);
    CHECK(decoded->Inputs[2].ClientTick == 4);
    CHECK(decoded->Inputs[2].State.Actions[0].Value.x == doctest::Approx(4.0f));
}

TEST_CASE("An input-idle send buffer still carries its ack")
{
    const TypeRegistry registry = MakeRegistry();

    const InputSendBuffer send;
    const Result<InputPacket> decoded = DecodeInputPacket(send.Encode(42, registry), registry);
    REQUIRE(decoded.has_value());
    CHECK(decoded->AckedServerTick == 42);
    CHECK(decoded->Inputs.empty());
}

TEST_CASE("DecodeInputPacket rejects a truncated header but recovers a truncated record run")
{
    const TypeRegistry registry = MakeRegistry();

    // A packet too short to carry the 20-byte header is a hard reject.
    const vector<u8> stub(8, 0);
    CHECK_FALSE(DecodeInputPacket(stub, registry).has_value());

    // A full packet truncated mid-run recovers the intact records and stops at the tear.
    const vector<ActionState> records = {
        MakeState(vec2(1.0f, 0.0f)),
        MakeState(vec2(0.0f, 1.0f)),
        MakeState(vec2(-1.0f, 0.0f)),
    };
    vector<u8> bytes = EncodeInputPacket(0, 10, records, registry);
    bytes.resize(bytes.size() - 4); // lop the tail off the last record

    const Result<InputPacket> decoded = DecodeInputPacket(bytes, registry);
    REQUIRE(decoded.has_value());
    CHECK(decoded->Inputs.size() < records.size());
    for (const TickedInput& input : decoded->Inputs)
    {
        CHECK(input.ClientTick >= 10);
    }
}

TEST_CASE("DecayInputPhases decays edges and holds levels")
{
    ActionState state;
    state.Actions = {
        ActionSample{.Id = ActionId{1}, .Phase = ActionPhase::Started},
        ActionSample{.Id = ActionId{2}, .Phase = ActionPhase::Ongoing},
        ActionSample{.Id = ActionId{3}, .Phase = ActionPhase::Completed},
        ActionSample{.Id = ActionId{4}, .Phase = ActionPhase::None},
    };

    const ActionState decayed = DecayInputPhases(state);
    CHECK(decayed.Actions[0].Phase == ActionPhase::Ongoing); // Started → Ongoing (held persists)
    CHECK(decayed.Actions[1].Phase == ActionPhase::Ongoing); // Ongoing unchanged
    CHECK(decayed.Actions[2].Phase == ActionPhase::None);    // Completed → None (edge gone)
    CHECK(decayed.Actions[3].Phase == ActionPhase::None);    // None unchanged
}

TEST_CASE("The jitter buffer feeds inputs at their client tick despite loss and reorder")
{
    const TypeRegistry registry = MakeRegistry();

    constexpr u64 Ticks = 8;
    InputSendBuffer send(InputSendBuffer::Settings{.Redundancy = 3});
    vector<ActionState> produced; // produced[t-1] is tick t's input
    vector<vector<u8>> packets;   // packets[t-1] is the packet sent on producing tick t
    for (u64 tick = 1; tick <= Ticks; ++tick)
    {
        const ActionState state = MakeState(vec2(static_cast<f32>(tick), -static_cast<f32>(tick)));
        produced.push_back(state);
        send.Stamp(tick, state);
        packets.push_back(send.Encode(0, registry));
    }

    // A lossy, reordering channel: drop packets 2 and 5 entirely (their ticks ride the redundant
    // overlap of later packets), and deliver the survivors out of order. A generous target depth
    // keeps the buffer from slewing, isolating the loss/reorder tolerance under test.
    const vector<usize> deliveryOrder = {0, 3, 2, 6, 5, 7};
    InputJitterBuffer jitter(InputJitterBuffer::Settings{.TargetDepth = 100});
    for (const usize index : deliveryOrder)
    {
        const Result<InputPacket> decoded = DecodeInputPacket(packets[index], registry);
        REQUIRE(decoded.has_value());
        jitter.Ingest(*decoded);
    }

    // Draining the buffer yields every tick exactly once, in ascending order, values intact — the
    // redundancy covered the two dropped packets and the reorder collapsed against the tick key.
    for (u64 tick = 1; tick <= Ticks; ++tick)
    {
        const optional<ActionState> consumed = jitter.Consume();
        REQUIRE(consumed.has_value());
        CHECK(SameState(*consumed, produced[tick - 1]));
    }
    CHECK(jitter.Depth() == 0);
}

TEST_CASE("The jitter buffer duplicates the last input with decayed phases on underrun")
{
    InputJitterBuffer jitter;

    InputPacket packet;
    packet.Inputs.push_back(
        TickedInput{.ClientTick = 1, .State = MakeState(vec2(1.0f, 0.0f), ActionPhase::Started)});
    jitter.Ingest(packet);

    // The real tick fires the jump edge; the coasted underrun tick holds the move but decays the
    // edge — the jump never re-fires while the held move persists.
    const optional<ActionState> real = jitter.Consume();
    REQUIRE(real.has_value());
    CHECK(real->Actions[1].Phase == ActionPhase::Started);

    const optional<ActionState> coasted = jitter.Consume();
    REQUIRE(coasted.has_value());
    CHECK(coasted->Actions[0].Value.x == doctest::Approx(1.0f)); // move held
    // The Started edge decays to Ongoing: the button reads as still held, but WasTriggered no longer
    // fires — a redundant/coasted tick never re-triggers the press.
    CHECK(coasted->Actions[1].Phase == ActionPhase::Ongoing);
    PlayerInput coastedInput;
    coastedInput.State = *coasted;
    CHECK_FALSE(coastedInput.WasTriggered(JumpAction));
    CHECK(coastedInput.IsHeld(JumpAction));

    // A fresh buffer coasts nothing — nullopt until the first input arrives.
    InputJitterBuffer empty;
    CHECK_FALSE(empty.Consume().has_value());
}

TEST_CASE("ConsumeCount counts only consumes that yield an input, not empty polls")
{
    InputJitterBuffer jitter;

    // Empty polls before the first input has ever arrived return nullopt and must not be counted:
    // ConsumeCount is an actual-inputs-consumed / idle-cost metric, not a call count.
    CHECK_FALSE(jitter.Consume().has_value());
    CHECK_FALSE(jitter.Consume().has_value());
    CHECK(jitter.ConsumeCount() == 0);
    CHECK(jitter.UnderrunCount() == 0);

    InputPacket packet;
    packet.Inputs.push_back(TickedInput{.ClientTick = 1, .State = MakeState(vec2(1.0f, 0.0f))});
    jitter.Ingest(packet);

    // A fresh buffered tick is a real consume: counted, and not an underrun.
    REQUIRE(jitter.Consume().has_value());
    CHECK(jitter.ConsumeCount() == 1);
    CHECK(jitter.UnderrunCount() == 0);

    // A coasted underrun still feeds the seat a (decayed) input, so it counts as an actual consume —
    // and as an underrun. Both counters advance together on this path.
    REQUIRE(jitter.Consume().has_value());
    CHECK(jitter.ConsumeCount() == 2);
    CHECK(jitter.UnderrunCount() == 1);
}

TEST_CASE("The jitter buffer drops the oldest on overrun to bound latency")
{
    InputJitterBuffer jitter(InputJitterBuffer::Settings{.TargetDepth = 2});

    InputPacket packet;
    for (u64 tick = 1; tick <= 10; ++tick)
    {
        packet.Inputs.push_back(TickedInput{
            .ClientTick = tick, .State = MakeState(vec2(static_cast<f32>(tick), 0.0f))});
    }
    jitter.Ingest(packet);
    CHECK(jitter.Depth() == 10);

    // One consume slews the backlog down: it drops the stale oldest ticks and returns a recent one,
    // leaving the buffer at the target depth rather than the whole backlog behind.
    const optional<ActionState> consumed = jitter.Consume();
    REQUIRE(consumed.has_value());
    CHECK(jitter.Depth() <= 2);
    CHECK(jitter.LastConsumedTick() > 1); // the oldest ticks were dropped, not consumed in order
    CHECK(consumed->Actions[0].Value.x > 1.0f);
}

TEST_CASE("A scripted client input drives the server pawn through the unchanged pipeline")
{
    TypeRegistry registry = MakeRegistry();

    // Script a client's per-tick input, redundantly packetized, then delivered with two dropped
    // packets — the same loss the redundancy covers.
    constexpr u64 Ticks = 8;
    InputSendBuffer send(InputSendBuffer::Settings{.Redundancy = 3});
    vector<ActionState> produced;
    vector<vector<u8>> packets;
    for (u64 tick = 1; tick <= Ticks; ++tick)
    {
        const ActionState state = MakeState(vec2(1.0f, 0.5f));
        produced.push_back(state);
        send.Stamp(tick, state);
        packets.push_back(send.Encode(0, registry));
    }

    InputJitterBuffer jitter(InputJitterBuffer::Settings{.TargetDepth = 100});
    for (usize index = 0; index < packets.size(); ++index)
    {
        if (index == 1 || index == 4)
        {
            continue; // drop these packets; their ticks ride later packets' overlap
        }
        const Result<InputPacket> decoded = DecodeInputPacket(packets[index], registry);
        REQUIRE(decoded.has_value());
        jitter.Ingest(*decoded);
    }

    // Server scene: a server-authoritative pawn whose PlayerInput the wire fills, plus the seat.
    Unique<Scene> scene = Scene::Create(registry);
    const Entity pawn = scene->CreateEntity();
    scene->Add<Transform>(pawn);
    scene->Add<Intent>(pawn);
    scene->Add<Mover>(pawn);
    scene->Add<Authority>(pawn, Authority{.Tier = Tier::Server});
    auto& seatInput = scene->Add<PlayerInput>(pawn);

    FakeContext ctx;
    ctx.Role = NetRole::Server;
    MovementSystem movement;
    constexpr f32 Delta = 1.0f / 60.0f;

    // Each server tick: feed the consumed input into the seat's PlayerInput, re-derive Intent through
    // the unchanged control mapping, then run the real MovementSystem.
    for (u64 tick = 1; tick <= Ticks; ++tick)
    {
        const optional<ActionState> consumed = jitter.Consume();
        REQUIRE(consumed.has_value());
        seatInput.State = *consumed;
        scene->Get<Intent>(pawn) = ControlMap(seatInput);
        movement.OnUpdate(*scene, Delta, ctx.Make());
    }

    // Reference: the same inputs applied in order directly through the identical control + movement
    // math. The server's pawn tracks it exactly — the wire changed nothing downstream of PlayerInput.
    Transform reference;
    const Mover mover;
    for (const ActionState& state : produced)
    {
        PlayerInput input;
        input.State = state;
        IntegrateMovement(reference, ControlMap(input), mover, Delta);
    }

    const Transform& result = scene->Get<Transform>(pawn);
    CHECK(glm::all(glm::lessThan(glm::abs(result.Position - reference.Position), vec3(1e-4f))));
}

TEST_CASE("The authority filter gates the builtin Sim advancers by role and tier")
{
    TypeRegistry registry = MakeRegistry();
    Unique<Scene> scene = Scene::Create(registry);

    const Entity server = scene->CreateEntity();
    scene->Add<Authority>(server, Authority{.Tier = Tier::Server});
    const Entity local = scene->CreateEntity();
    scene->Add<Authority>(local, Authority{.Tier = Tier::Local});
    const Entity remote = scene->CreateEntity();
    scene->Add<Authority>(remote, Authority{.Tier = Tier::Remote});
    const Entity defaulted = scene->CreateEntity(); // no Authority component ⇒ Server-tier

    FakeContext serverPeer;
    serverPeer.Role = NetRole::Server;
    const SystemContext serverCtx = serverPeer.Make();
    CHECK(HasAuthority(serverCtx, *scene, server));
    CHECK(HasAuthority(serverCtx, *scene, defaulted));
    CHECK(HasAuthority(serverCtx, *scene, local));
    CHECK_FALSE(HasAuthority(serverCtx, *scene, remote));

    FakeContext clientPeer;
    clientPeer.Role = NetRole::Client;
    const SystemContext clientCtx = clientPeer.Make();
    CHECK_FALSE(HasAuthority(clientCtx, *scene, server));
    CHECK_FALSE(HasAuthority(clientCtx, *scene, defaulted));
    CHECK(HasAuthority(clientCtx, *scene, local)); // client-local state keeps simulating
    CHECK_FALSE(HasAuthority(clientCtx, *scene, remote));
}

TEST_CASE("MovementSystem advances a pawn on the server but a client leaves it to the snapshot")
{
    TypeRegistry registry = MakeRegistry();
    MovementSystem movement;
    constexpr f32 Delta = 1.0f / 60.0f;

    const auto runPawn = [&](const NetRole role, const Tier tier) -> vec3
    {
        Unique<Scene> scene = Scene::Create(registry);
        const Entity pawn = scene->CreateEntity();
        scene->Add<Transform>(pawn);
        scene->Add<Intent>(pawn, Intent{.Move = vec3(0.0f, 0.0f, 1.0f)});
        scene->Add<Authority>(pawn, Authority{.Tier = tier});
        FakeContext ctx;
        ctx.Role = role;
        movement.OnUpdate(*scene, Delta, ctx.Make());
        return scene->Get<Transform>(pawn).Position;
    };

    // The server advances its Server-tier pawn; a client skips it (the snapshot stream owns it).
    CHECK(glm::length(runPawn(NetRole::Server, Tier::Server)) > 0.0f);
    CHECK(glm::length(runPawn(NetRole::Client, Tier::Server)) == doctest::Approx(0.0f));

    // A client still advances its own Local-tier state — an AI/server Intent producer is unaffected
    // where the peer holds authority.
    CHECK(glm::length(runPawn(NetRole::Client, Tier::Local)) > 0.0f);
    CHECK(glm::length(runPawn(NetRole::Server, Tier::Local)) > 0.0f);
}
