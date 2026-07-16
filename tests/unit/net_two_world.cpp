// Two-world in-process integration: a server Scene + ServerHost and a client Scene + ClientHost
// over a LoopbackTransport (and, for the abuse cases, a FaultInjectionTransport), stepped tick by
// tick in one process — no ICD, no socket, no wall clock. Where the per-plan slices each proved one
// layer (the codec, the jitter buffer, the join glue), these consolidate the whole stack into
// end-to-end scenarios: join → play → leave with field-wise convergence, a scripted input round-trip
// driving the server pawn and the client seeing it interpolated, convergence after seeded
// loss/reorder/duplication, a hostile input stream the server survives, and two clients each seeing
// both pawns while driving only its own. Deterministic by construction (fixed tick, injected time,
// seeded faults) — the regression net every later net change runs against.

#include <doctest/doctest.h>

#include <glm/geometric.hpp>

#include <Veng/Net/Client.h>
#include <Veng/Net/FaultInjectionTransport.h>
#include <Veng/Net/Host.h>
#include <Veng/Net/InputFeed.h>
#include <Veng/Net/LoopbackTransport.h>
#include <Veng/Net/Replication.h>
#include <Veng/Net/Server.h>
#include <Veng/Net/Transport.h>
#include <Veng/Net/WorldEnvelope.h>
#include <Veng/Net/WorldKey.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Movement.h>
#include <Veng/Scene/RemoteInterpolationSystem.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SystemRegistry.h>
#include <Veng/World.h>
#include <Veng/WorldDirectory.h>
#include <Veng/WorldRunner.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <unordered_map>
#include <utility>

using namespace Veng;
using namespace Veng::Net;

namespace
{
    // The sample's one game action: a 2D move axis the control mapping turns into an Intent.
    constexpr ActionId MoveAction{0xA1};

    // A dependency-free prefab and the seat spawn never dereference the manager, so a
    // never-dereferenced reference is safe (the net_join_flow.cpp / game_mode.cpp precedent).
    AssetManager& FakeAssets()
    {
        alignas(16) static unsigned char bytes[64]{};
        return *reinterpret_cast<AssetManager*>(bytes);
    }

    // A held 2D move for one tick — the resolved input a client seat's InputMappingSystem would fill.
    ActionState MoveState(const vec2 move)
    {
        ActionState state;
        state.Actions = {
            ActionSample{.Id = MoveAction, .Value = move, .Phase = ActionPhase::Ongoing}};
        return state;
    }

    // The game's control mapping (the unchanged action → Intent step): move on the XZ plane.
    Intent ControlMap(const PlayerInput& input)
    {
        const vec2 move = input.GetValue(MoveAction);
        return Intent{.Move = vec3(move.x, 0.0f, move.y)};
    }

    // A SystemContext over never-dereferenced service storage with a settable role — MovementSystem
    // and the interpolation system read only the scene, delta, and context.Role (net_input_flow.cpp).
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

    const ConnectionConfig FastConfig{
        .ResendInterval = 0.02, .KeepaliveInterval = 0.05, .TimeoutInterval = 5.0};

    constexpr AssetId LevelId{0x00000000000000AAULL};

    // The server half of the whole world drive, minus the transport: a scene, a ServerHost, the
    // per-connection jitter buffers, and the emulated game-mode spawn rule + control/movement sim. It
    // is the ServerHost/InputFeed path Application drives, stepped by hand.
    struct ServerWorld
    {
        TypeRegistry Types;
        Unique<Scene> World;
        Unique<ServerHost> Host;
        std::unordered_map<u64, InputJitterBuffer> Jitter;
        std::unordered_map<ConnectionId, Entity> Pawns;
        MovementSystem Movement;
        // When set, SimStep consumes input scheduled for the tick (the ahead-of-server model) rather
        // than the v1 arrival-front consume; the clock-sync scenario exercises this path.
        bool ScheduledConsume = false;

        explicit ServerWorld(Transport& transport, f32 interestRadius = 0.0f, bool quantize = false)
        {
            RegisterBuiltinTypes(Types);
            World = Scene::Create(Types);
            Result<Unique<ServerHost>> host = ServerHost::Create(ServerHostInfo{
                .Server = ServerInfo{.TransportOverride = &transport, .Connection = FastConfig},
                .World = *World,
                .Assets = FakeAssets(),
                .LevelId = LevelId,
                .Replication =
                    ReplicationServer::Settings{.SnapshotInterval = 2, .QuantizeSpatial = quantize},
                .Interest = InterestSettings{.Radius = interestRadius, .MinDwellSnapshots = 2},
            });
            REQUIRE(host.has_value());
            Host = std::move(*host);
        }

        // The game-mode spawn rule, net-unaware: pawn any connection-owned seat that has no live
        // pawn, threading the seat's owner onto the pawn (Authority::Owner), and despawn a pawn whose
        // seat has gone. The pawn is a Server-tier (Transform, Intent, Mover) entity — the movement
        // pipeline's target, replicated by full state (no mesh needed for a convergence test).
        void RunSpawnRule()
        {
            for (const ConnectionId id : Host->Server().Connections())
            {
                const Entity seat = Host->SeatFor(id);
                if (seat.IsNull())
                {
                    continue;
                }
                auto& possesses = World->Get<Possesses>(seat);
                const bool pawned = !possesses.Pawn.IsNull() && World->IsAlive(possesses.Pawn);
                if (pawned)
                {
                    continue;
                }
                const Entity pawn = World->CreateEntity();
                World->Add<Transform>(pawn);
                World->Add<Intent>(pawn);
                World->Add<Mover>(pawn);
                World->Add<Authority>(pawn, Authority{.Tier = Tier::Server, .Owner = id});
                possesses.Pawn = pawn;
                Pawns[id] = pawn;
            }

            // A departed connection's seat is torn down by the host; reap its orphaned pawn.
            for (auto it = Pawns.begin(); it != Pawns.end();)
            {
                if (Host->SeatFor(it->first).IsNull())
                {
                    if (World->IsAlive(it->second))
                    {
                        World->DestroyEntity(it->second);
                    }
                    it = Pawns.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        // One server sim step at @p tick: stamp the change tick, spawn/reap pawns, feed each
        // connection's buffered wire input into its seat, re-derive Intent through the unchanged
        // control mapping, and advance authoritative movement (Server role, so Server-tier pawns run).
        void SimStep(u64 tick, f32 delta)
        {
            World->SetChangeTick(tick);
            RunSpawnRule();
            if (ScheduledConsume)
            {
                FeedSeatInputs(*Host, Jitter, WorldInstanceId{}, *World, tick);
            }
            else
            {
                FeedSeatInputs(*Host, Jitter, WorldInstanceId{}, *World);
            }

            for (const auto& [id, pawn] : Pawns)
            {
                const Entity seat = Host->SeatFor(id);
                if (seat.IsNull() || !World->IsAlive(pawn))
                {
                    continue;
                }
                if (const PlayerInput* input = World->TryGet<PlayerInput>(seat))
                {
                    World->Get<Intent>(pawn) = ControlMap(*input);
                }
            }

            FakeContext ctx;
            ctx.Role = NetRole::Server;
            Movement.OnUpdate(*World, delta, ctx.Make());
        }

        // The transport/net-pump half of a frame: send this tick's stream to ready seats, accept new
        // connections, ingest each connection's redundant input into its jitter buffer.
        void NetPump(f64 now, u64 tick)
        {
            Host->Pump(now, tick);
            IngestConnectionInputs(*Host, Jitter, InputJitterBuffer::Settings{}, Types);
        }
    };

    // The client half: a connection, a ClientHost with a Local-tier camera + a local input seat, the
    // input send window, and the View-phase interpolation system that renders the remote pawns.
    struct ClientWorld
    {
        TypeRegistry Types;
        Unique<Net::Client> Client;
        Unique<ClientHost> Host;
        InputSendBuffer Send;
        RemoteInterpolationSystem Interp;
        Unique<Scene> ClientScene;
        Entity LocalSeat = Entity::Null;
        Entity LocalCamera = Entity::Null;
        Entity OwnPawn = Entity::Null;

        explicit ClientWorld(Transport& transport, WorldKey key = DefaultWorldKey)
        {
            RegisterBuiltinTypes(Types);
            Client = *Net::Client::Connect(
                ClientInfo{.TransportOverride = &transport, .Connection = FastConfig});
            Send = InputSendBuffer(InputSendBuffer::Settings{.Redundancy = 3});
            Interp.SetSettings(RemoteInterpolationSystem::Settings{
                .SnapshotInterval = 2, .InterpolationDelayIntervals = 2, .SimTickRate = 60.0});

            Host = ClientHost::Create(ClientHostInfo{
                .Client = *Client,
                .Assets = FakeAssets(),
                .WorldKey = key,
                .LoadLevel = [this](AssetId) -> Scene*
                {
                    // The client scene the join loads into: a Local-tier camera the join wires to the
                    // own pawn, and a Local-tier input seat whose PlayerInput a scripted input fills
                    // and the send window carries. The caller owns the scene (the host borrows it); the
                    // server-authoritative pawns arrive from the stream.
                    ClientScene = Scene::Create(Types);
                    LocalCamera = ClientScene->CreateEntity();
                    ClientScene->Add<Transform>(LocalCamera);
                    ClientScene->Add<Camera>(LocalCamera);
                    ClientScene->Add<CameraFollow>(LocalCamera);
                    ClientScene->Add<Authority>(LocalCamera, Authority{.Tier = Tier::Local});

                    LocalSeat = ClientScene->CreateEntity();
                    ClientScene->Add<Viewer>(LocalSeat, Viewer{.Camera = LocalCamera});
                    ClientScene->Add<SeatInput>(LocalSeat);
                    ClientScene->Add<PlayerInput>(LocalSeat);
                    ClientScene->Add<Authority>(LocalSeat, Authority{.Tier = Tier::Local});
                    return ClientScene.get();
                },
                .ResolvePrefab = [](AssetId) -> Ref<Prefab> { return nullptr; },
                .OnPossession =
                    [this](Scene& world, const Entity pawn)
                {
                    OwnPawn = pawn;
                    if (!LocalCamera.IsNull() && world.IsAlive(LocalCamera))
                    {
                        world.Get<CameraFollow>(LocalCamera).Target = pawn;
                    }
                },
                // This suite is the interpolation-only regression floor: the own pawn stays a Remote
                // mirror rendered in the past, so it predicts nothing (prediction + reconciliation
                // convergence is net_reconciliation.cpp's gate). Without this the default policy would
                // promote it to Predicted and it would drift, uncorrected, off the arrival-front feed.
                .Prediction = [](const Scene&, Entity) { return vector<Entity>{}; },
            });
        }

        // The client half of a frame: advance the join flow + apply the stream, run the View-phase
        // interpolation, then stamp the scripted input and send the redundant window.
        void Frame(f64 now, u64 clientTick, f32 delta, const optional<ActionState>& scripted)
        {
            Host->Pump(now);

            if (Scene* world = Host->World())
            {
                FakeContext ctx;
                ctx.Role = NetRole::Client;
                Interp.OnUpdate(*world, delta, ctx.Make());

                if (scripted.has_value() && !LocalSeat.IsNull() && world->IsAlive(LocalSeat))
                {
                    world->Get<PlayerInput>(LocalSeat).State = *scripted;
                }
                StampLocalSeatInput(Send, *world, clientTick);
            }

            if (Client->State() == ClientState::Connected && Host->CurrentJoinId() != ControlJoinId)
            {
                (void)Client->Server().Send(
                    Channel::UnreliableSequenced,
                    EncodeWorldEnvelope(Host->CurrentJoinId(), Send.Encode(0, Types)));
            }
        }
    };
}

TEST_CASE("Two worlds join, play, converge field-wise, and leave")
{
    auto [serverT, clientT] = LoopbackTransport::CreatePair();
    ServerWorld server(*serverT);
    ClientWorld client(*clientT);

    // Hold +x for a while, then a quiescent tail of explicit zero-move input: the server pawn
    // advances, then halts, so the client's past-lagged interpolation clock catches up to the newest
    // sample and the displayed pose converges onto the server's final one.
    const ActionState move = MoveState(vec2(1.0f, 0.0f));
    const ActionState stop = MoveState(vec2(0.0f, 0.0f));

    f64 now = 0.0;
    constexpr f32 Delta = 1.0f / 60.0f;
    for (u64 tick = 1; tick <= 110; ++tick)
    {
        now += Delta;
        server.SimStep(tick, Delta);
        server.NetPump(now, tick);
        client.Frame(now, tick, Delta, tick <= 60 ? move : stop);
    }

    REQUIRE(client.Client->State() == ClientState::Connected);
    REQUIRE(client.Host->IsJoined());
    REQUIRE(client.Host->World() != nullptr);

    // The connection got a seat, the spawn rule pawned it, and the pawn moved on the server.
    const ConnectionId id = client.Client->AssignedId();
    const Entity serverPawn = server.Pawns.at(id);
    const f32 serverX = server.World->Get<Transform>(serverPawn).Position.x;
    CHECK(serverX > 0.5f);

    // The own seat bound as a replicated Remote-tier Viewer, and the client wired its camera to the
    // replicated pawn — the possession round-tripped with no bespoke message.
    const Entity clientSeat = client.Host->Seat();
    REQUIRE_FALSE(clientSeat.IsNull());
    Scene& world = *client.Host->World();
    CHECK(world.Get<Authority>(clientSeat).Tier == Tier::Remote);
    const Entity clientPawn = client.OwnPawn;
    REQUIRE_FALSE(clientPawn.IsNull());
    CHECK(world.Get<CameraFollow>(client.LocalCamera).Target == clientPawn);

    // Field-wise convergence: after the long quiescent tail the interpolation clock has caught up to
    // the newest received sample, so the client's displayed pose equals the server's authoritative
    // one (interpolation holds at the newest sample, never extrapolates past it).
    const vec3 serverPos = server.World->Get<Transform>(serverPawn).Position;
    const vec3 clientPos = world.Get<Transform>(clientPawn).Position;
    CHECK(glm::length(clientPos - serverPos) < 0.05f);

    // Leave: the client disconnects, the server surfaces the event, tears the seat down, and the
    // spawn rule reaps the orphaned pawn — the world empties of the departed player.
    client.Client->Disconnect();
    bool sawLeave = false;
    for (u64 tick = 111; tick <= 150; ++tick)
    {
        now += Delta;
        server.SimStep(tick, Delta);
        server.NetPump(now, tick);
        for (const NetEvent& event : server.Host->Events())
        {
            if (event.Type == NetEventType::Disconnected && event.Id == id)
            {
                sawLeave = true;
            }
        }
        client.Client->Pump(now);
    }
    CHECK(sawLeave);
    CHECK(server.Host->SeatFor(id).IsNull());
    CHECK_FALSE(server.World->IsAlive(serverPawn));
}

TEST_CASE("A scripted client input round-trips: server pawn moves, client renders it in the past")
{
    auto [serverT, clientT] = LoopbackTransport::CreatePair();
    ServerWorld server(*serverT);
    ClientWorld client(*clientT);

    // Script a +z hold: the wire carries it to the server seat, the control system re-derives the
    // Intent, movement advances the Server-tier pawn along +z.
    const ActionState move = MoveState(vec2(0.0f, 1.0f));

    f64 now = 0.0;
    constexpr f32 Delta = 1.0f / 60.0f;
    ConnectionId id = ServerConnectionId;
    for (u64 tick = 1; tick <= 80; ++tick)
    {
        now += Delta;
        server.SimStep(tick, Delta);
        server.NetPump(now, tick);
        client.Frame(now, tick, Delta, move);
        if (!server.Host->Server().Connections().empty())
        {
            id = server.Host->Server().Connections().front();
        }
    }

    REQUIRE(id != ServerConnectionId);
    const Entity serverPawn = server.Pawns.at(id);

    // The server pawn advanced along +z from the wire input alone — the scripted client input drove
    // it through the unchanged PlayerInput → Intent → Movement pipeline.
    const f32 serverZ = server.World->Get<Transform>(serverPawn).Position.z;
    CHECK(serverZ > 0.3f);

    // The client renders the pawn in the past: it has moved along +z but lags the live server pose
    // (interpolation delay), never leading it.
    Scene& world = *client.Host->World();
    const Entity clientPawn = client.OwnPawn;
    REQUIRE_FALSE(clientPawn.IsNull());
    const f32 clientZ = world.Get<Transform>(clientPawn).Position.z;
    CHECK(clientZ > 0.05f);
    CHECK(clientZ <= serverZ + 1e-3f);
}

TEST_CASE("The two worlds converge after a burst of seeded loss, reorder, and duplication")
{
    auto [serverT, clientT] = LoopbackTransport::CreatePair();

    // Both ends ride a lossy, reordering, duplicating link; a generous resend/timeout lets the
    // reliable channel punch the handshake and spawn stream through, and the redundant input window +
    // idempotent snapshots ride the unreliable losses.
    const FaultInjectionConfig faults{
        .DropRate = 0.25f, .DuplicateRate = 0.1f, .ReorderRate = 0.2f, .Seed = 1337};
    FaultInjectionTransport serverLink(*serverT, faults);
    FaultInjectionTransport clientLink(*clientT, faults);

    ServerWorld server(serverLink);
    ClientWorld client(clientLink);

    const ActionState move = MoveState(vec2(1.0f, 0.0f));
    const ActionState stop = MoveState(vec2(0.0f, 0.0f));

    f64 now = 0.0;
    constexpr f32 Delta = 1.0f / 60.0f;
    ConnectionId id = ServerConnectionId;
    for (u64 tick = 1; tick <= 460; ++tick)
    {
        now += Delta;
        server.SimStep(tick, Delta);
        server.NetPump(now, tick);
        // A long quiescent tail lets the lossy latest-wins stream converge onto the halted pose.
        client.Frame(now, tick, Delta, tick <= 300 ? move : stop);
        if (!server.Host->Server().Connections().empty())
        {
            id = server.Host->Server().Connections().front();
        }
    }

    // The join completed despite the lossy link (the reliable handshake resent until acked), and the
    // pawn spawned exactly once — no duplicate on the reordered/duplicated spawn stream.
    REQUIRE(client.Client->State() == ClientState::Connected);
    REQUIRE(client.Host->IsJoined());
    REQUIRE(id != ServerConnectionId);
    const Entity serverPawn = server.Pawns.at(id);
    CHECK(client.Host->Replication().Map().Size() >= 2); // the seat + the pawn, no doubles

    Scene& world = *client.Host->World();
    const Entity clientPawn = client.OwnPawn;
    REQUIRE_FALSE(clientPawn.IsNull());

    // After the run the latest-wins snapshots have converged the client's displayed pose onto the
    // server's authoritative one — the loss burst cost bandwidth, never correctness.
    const vec3 serverPos = server.World->Get<Transform>(serverPawn).Position;
    const vec3 clientPos = world.Get<Transform>(clientPawn).Position;
    CHECK(glm::length(clientPos - serverPos) < 0.2f);
}

TEST_CASE("A hostile input stream drops rather than faulting the serving server")
{
    auto [serverT, clientT] = LoopbackTransport::CreatePair();
    ServerWorld server(*serverT);
    ClientWorld client(*clientT);

    f64 now = 0.0;
    constexpr f32 Delta = 1.0f / 60.0f;

    // Join cleanly first.
    for (u64 tick = 1; tick <= 30; ++tick)
    {
        now += Delta;
        server.SimStep(tick, Delta);
        server.NetPump(now, tick);
        client.Frame(now, tick, Delta, std::nullopt);
    }
    REQUIRE(client.Client->State() == ClientState::Connected);
    const ConnectionId id = client.Client->AssignedId();

    // The client now sends garbage on the unreliable input channel: a too-short stub, and random
    // bytes too large to be a valid input packet. IngestConnectionInputs must drop each, never fault.
    for (u64 tick = 31; tick <= 70; ++tick)
    {
        now += Delta;
        const vector<u8> stub(4, 0xFFu);
        (void)client.Client->Server().Send(Channel::UnreliableSequenced, stub);
        vector<u8> garbage(48);
        for (usize i = 0; i < garbage.size(); ++i)
        {
            garbage[i] = static_cast<u8>((tick * 31 + i * 7) & 0xFF);
        }
        (void)client.Client->Server().Send(Channel::UnreliableSequenced, garbage);

        server.SimStep(tick, Delta);
        server.NetPump(now, tick);
        client.Client->Pump(now);
    }

    // The server is still serving: the connection is live, its seat intact, and a clean input after
    // the garbage still feeds the seat — the hostile stream neither killed the connection nor the sim.
    REQUIRE(server.Host->Server().Connections().size() == 1);
    const Entity seat = server.Host->SeatFor(id);
    REQUIRE_FALSE(seat.IsNull());

    const ActionState move = MoveState(vec2(1.0f, 0.0f));
    bool fed = false;
    for (u64 tick = 71; tick <= 110 && !fed; ++tick)
    {
        now += Delta;
        client.Frame(now, tick, Delta, move);
        server.SimStep(tick, Delta);
        server.NetPump(now, tick);
        if (server.World->Has<PlayerInput>(seat))
        {
            const vec2 value = server.World->Get<PlayerInput>(seat).GetValue(MoveAction);
            if (glm::length(value - vec2(1.0f, 0.0f)) < 1e-4f)
            {
                fed = true;
            }
        }
    }
    CHECK(fed);
}

TEST_CASE("Scheduled consume: a client running ahead underruns ~never under steady input")
{
    // The ahead-of-server model the tick-offset slew produces: the client's sim tick runs a few ticks
    // ahead of the server (RTT/2 + jitter + margin — roughly three to four ticks at a 60 Hz sim over a
    // ~50 ms link), so the input it stamps for tick T has arrived by the time the server's scheduled
    // consume reaches T. Modeled device-free by stepping the client Lead ticks ahead of the server on
    // one shared tick epoch, and consuming with the scheduled ConsumeForTick path.
    auto [serverT, clientT] = LoopbackTransport::CreatePair();
    ServerWorld server(*serverT);
    server.ScheduledConsume = true;
    ClientWorld client(*clientT);

    constexpr u64 Lead = 4;
    const ActionState move = MoveState(vec2(1.0f, 0.0f));

    f64 now = 0.0;
    constexpr f32 Delta = 1.0f / 60.0f;
    ConnectionId id = ServerConnectionId;
    for (u64 step = 1; step <= 240; ++step)
    {
        now += Delta;
        // The client leads by Lead ticks: this iteration it stamps and sends tick `step`, while the
        // server — lagging — consumes tick `step - Lead`, for which the input has long since arrived.
        client.Frame(now, step, Delta, move);
        if (step > Lead)
        {
            const u64 serverTick = step - Lead;
            server.SimStep(serverTick, Delta);
            server.NetPump(now, serverTick);
        }
        if (!server.Host->Server().Connections().empty())
        {
            id = server.Host->Server().Connections().front();
        }
    }

    REQUIRE(id != ServerConnectionId);
    REQUIRE(server.Pawns.contains(id));

    // The scheduled input drove the server pawn along +x every tick — real wire input, not a coasted
    // edge (a chronic underrun would still hold the +x value but is what this asserts against).
    CHECK(server.World->Get<Transform>(server.Pawns.at(id)).Position.x > 0.5f);

    // With the client leading, each server tick's input was already buffered when the scheduled consume
    // reached it: the server consumed a fresh input almost every tick, so underrun-duplication is
    // negligible — a small startup transient before the first input arrives aside.
    const InputJitterBuffer& buffer =
        server.Jitter.at(InputBufferKey(id, server.Host->CurrentJoin(id)));
    REQUIRE(buffer.ConsumeCount() > 150);
    CHECK(buffer.UnderrunCount() <= 5);
}

namespace
{
    // A multi-peer in-process medium so one server transport can talk to two client transports at
    // once (a LoopbackTransport pair is strictly point-to-point). The net_lifecycle.cpp Hub, reused.
    struct Hub
    {
        struct Packet
        {
            EndpointId From = EndpointId::None;
            vector<u8> Bytes;
        };

        std::unordered_map<u32, std::deque<Packet>> Queues;
        u32 NextEndpoint = 1;

        u32 Register()
        {
            const u32 id = NextEndpoint;
            NextEndpoint += 1;
            Queues[id];
            return id;
        }
    };

    class HubTransport final : public Transport
    {
    public:
        HubTransport(Ref<Hub> hub, u32 self, u32 resolveTo)
            : m_Hub(std::move(hub)), m_Self(self), m_ResolveTo(resolveTo)
        {
        }

        VoidResult Send(EndpointId to, std::span<const u8> bytes) override
        {
            const auto it = m_Hub->Queues.find(static_cast<u32>(to));
            if (it != m_Hub->Queues.end())
            {
                it->second.push_back(Hub::Packet{.From = static_cast<EndpointId>(m_Self),
                                                 .Bytes = vector<u8>(bytes.begin(), bytes.end())});
            }
            return {};
        }

        optional<Datagram> Receive() override
        {
            std::deque<Hub::Packet>& queue = m_Hub->Queues[m_Self];
            if (queue.empty())
            {
                return {};
            }
            m_Scratch = std::move(queue.front().Bytes);
            const EndpointId from = queue.front().From;
            queue.pop_front();
            return Datagram{.From = from, .Bytes = m_Scratch};
        }

        Result<EndpointId> Resolve(string_view, u16) override
        {
            return static_cast<EndpointId>(m_ResolveTo);
        }

    private:
        Ref<Hub> m_Hub;
        u32 m_Self;
        u32 m_ResolveTo;
        vector<u8> m_Scratch;
    };
}

TEST_CASE("Scheduled consume with the client ahead drives server input with ~zero underrun")
{
    auto [serverT, clientT] = LoopbackTransport::CreatePair();
    ServerWorld server(*serverT);
    server.ScheduledConsume = true; // the ahead-of-server tick model
    ClientWorld client(*clientT);

    // The client runs its sim tick ahead of the server (the converged tick-offset slew): the input it
    // stamps for tick T arrives before the server's scheduled consume of T. Four ticks of lead over a
    // zero-latency loopback stands in for the RTT/2 + jitter margin a real link's slew would size at,
    // say, 50 ms RTT — the mechanism the assertion pins is "input for T is buffered by tick T".
    constexpr u64 AheadTicks = 4;
    const ActionState move = MoveState(vec2(1.0f, 0.0f));

    f64 now = 0.0;
    constexpr f32 Delta = 1.0f / 60.0f;
    ConnectionId id = ServerConnectionId;

    const auto step = [&](u64 serverTick)
    {
        now += Delta;
        server.SimStep(serverTick, Delta);
        server.NetPump(now, serverTick);
        client.Frame(now, serverTick + AheadTicks, Delta, move);
        if (!server.Host->Server().Connections().empty())
        {
            id = server.Host->Server().Connections().front();
        }
    };

    // Warm up: join, spawn, and let the ahead-stamped input stream fill the buffer.
    for (u64 tick = 1; tick <= 60; ++tick)
    {
        step(tick);
    }
    REQUIRE(client.Host->IsJoined());
    REQUIRE(id != ServerConnectionId);
    const u64 jitterKey = InputBufferKey(id, server.Host->CurrentJoin(id));
    REQUIRE(server.Jitter.contains(jitterKey));

    // Over a steady window every scheduled consume finds its tick's input already buffered, so the
    // underrun (coast) count does not move — the input-timing win the ahead-of-server model buys.
    const u64 underrunBefore = server.Jitter.at(jitterKey).UnderrunCount();
    const u64 consumeBefore = server.Jitter.at(jitterKey).ConsumeCount();
    for (u64 tick = 61; tick <= 200; ++tick)
    {
        step(tick);
    }
    CHECK(server.Jitter.at(jitterKey).ConsumeCount() - consumeBefore == 140); // one per server tick
    CHECK(server.Jitter.at(jitterKey).UnderrunCount() - underrunBefore == 0); // none underran

    // The real (not coasted) input drove the server pawn: the held +x move advanced it.
    const Entity serverPawn = server.Pawns.at(id);
    CHECK(server.World->Get<Transform>(serverPawn).Position.x > 0.3f);
}

TEST_CASE("Two clients each see both pawns and drive only their own")
{
    const auto hub = CreateRef<Hub>();
    const u32 serverEndpoint = hub->Register();
    auto serverT = CreateUnique<HubTransport>(hub, serverEndpoint, serverEndpoint);
    ServerWorld server(*serverT);

    auto clientTa = CreateUnique<HubTransport>(hub, hub->Register(), serverEndpoint);
    auto clientTb = CreateUnique<HubTransport>(hub, hub->Register(), serverEndpoint);
    ClientWorld clientA(*clientTa);
    ClientWorld clientB(*clientTb);

    // A drives +x, B drives -x — distinct so each pawn's motion is identifiable on the other's view.
    const ActionState moveA = MoveState(vec2(1.0f, 0.0f));
    const ActionState moveB = MoveState(vec2(-1.0f, 0.0f));

    f64 now = 0.0;
    constexpr f32 Delta = 1.0f / 60.0f;
    for (u64 tick = 1; tick <= 120; ++tick)
    {
        now += Delta;
        server.SimStep(tick, Delta);
        server.NetPump(now, tick);
        clientA.Frame(now, tick, Delta, moveA);
        clientB.Frame(now, tick, Delta, moveB);
    }

    REQUIRE(clientA.Host->IsJoined());
    REQUIRE(clientB.Host->IsJoined());
    REQUIRE(server.Host->Server().Connections().size() == 2);

    const ConnectionId idA = clientA.Client->AssignedId();
    const ConnectionId idB = clientB.Client->AssignedId();
    REQUIRE(idA != idB);

    // The server holds two distinct pawns, one owned by each connection, moved in opposite directions.
    const Entity serverPawnA = server.Pawns.at(idA);
    const Entity serverPawnB = server.Pawns.at(idB);
    CHECK(server.World->Get<Transform>(serverPawnA).Position.x > 0.3f);
    CHECK(server.World->Get<Transform>(serverPawnB).Position.x < -0.3f);

    // Each client's own seat resolves to its own pawn — authority + ownership: A drives A's pawn only.
    CHECK_FALSE(clientA.OwnPawn.IsNull());
    CHECK_FALSE(clientB.OwnPawn.IsNull());

    const NetId netA = server.World->Get<NetIdentity>(serverPawnA).Id;
    const NetId netB = server.World->Get<NetIdentity>(serverPawnB).Id;
    CHECK(clientA.OwnPawn == clientA.Host->Replication().Map().Lookup(netA));
    CHECK(clientB.OwnPawn == clientB.Host->Replication().Map().Lookup(netB));

    // Each client sees *both* pawns as replicated Remote-tier mirrors — the other player's pawn is
    // present and interpolated, not just its own.
    Scene& worldA = *clientA.Host->World();
    Scene& worldB = *clientB.Host->World();
    const Entity aSeesB = clientA.Host->Replication().Map().Lookup(netB);
    const Entity bSeesA = clientB.Host->Replication().Map().Lookup(netA);
    REQUIRE_FALSE(aSeesB.IsNull());
    REQUIRE_FALSE(bSeesA.IsNull());
    CHECK(worldA.Get<Authority>(aSeesB).Tier == Tier::Remote);
    CHECK(worldB.Get<Authority>(bSeesA).Tier == Tier::Remote);

    // Each client drives only its own pawn: A's view of B's pawn shows B's -x motion, not A's +x —
    // A never moved B (authority stayed with each owner's input on the server).
    CHECK(worldA.Get<Transform>(aSeesB).Position.x < 0.0f);
    CHECK(worldB.Get<Transform>(bSeesA).Position.x > 0.0f);
}

TEST_CASE("Interest management: two clients far apart each hear only their own neighborhood")
{
    const auto hub = CreateRef<Hub>();
    const u32 serverEndpoint = hub->Register();
    auto serverT = CreateUnique<HubTransport>(hub, serverEndpoint, serverEndpoint);
    // A tight interest radius: the pawns start together (both visible) then drive apart past it.
    ServerWorld server(*serverT, /*interestRadius=*/0.1f);

    auto clientTa = CreateUnique<HubTransport>(hub, hub->Register(), serverEndpoint);
    auto clientTb = CreateUnique<HubTransport>(hub, hub->Register(), serverEndpoint);
    ClientWorld clientA(*clientTa);
    ClientWorld clientB(*clientTb);

    const ActionState moveA = MoveState(vec2(1.0f, 0.0f));  // +x
    const ActionState moveB = MoveState(vec2(-1.0f, 0.0f)); // -x

    f64 now = 0.0;
    constexpr f32 Delta = 1.0f / 60.0f;
    for (u64 tick = 1; tick <= 260; ++tick)
    {
        now += Delta;
        server.SimStep(tick, Delta);
        server.NetPump(now, tick);
        clientA.Frame(now, tick, Delta, moveA);
        clientB.Frame(now, tick, Delta, moveB);
    }

    REQUIRE(clientA.Host->IsJoined());
    REQUIRE(clientB.Host->IsJoined());
    REQUIRE(server.Host->Server().Connections().size() == 2);

    const ConnectionId idA = clientA.Client->AssignedId();
    const ConnectionId idB = clientB.Client->AssignedId();
    const Entity serverPawnA = server.Pawns.at(idA);
    const Entity serverPawnB = server.Pawns.at(idB);
    const NetId netA = server.World->Get<NetIdentity>(serverPawnA).Id;
    const NetId netB = server.World->Get<NetIdentity>(serverPawnB).Id;

    // The pawns drove well past the interest radius apart.
    const f32 separation = std::abs(server.World->Get<Transform>(serverPawnA).Position.x -
                                    server.World->Get<Transform>(serverPawnB).Position.x);
    CHECK(separation > 0.2f);

    // Each client still owns and sees its own pawn (owner-relevant + the query center)...
    CHECK_FALSE(clientA.Host->Replication().Map().Lookup(netA).IsNull());
    CHECK_FALSE(clientB.Host->Replication().Map().Lookup(netB).IsNull());

    // ...but no longer hears about the other's pawn — it left interest (a visibility despawn),
    // so the wire carried only each connection's neighborhood, not the world.
    CHECK(clientA.Host->Replication().Map().Lookup(netB).IsNull());
    CHECK(clientB.Host->Replication().Map().Lookup(netA).IsNull());
}

TEST_CASE("Two clients with quantization on: each sees the other's pawn move (not frozen at spawn)")
{
    const auto hub = CreateRef<Hub>();
    const u32 serverEndpoint = hub->Register();
    auto serverT = CreateUnique<HubTransport>(hub, serverEndpoint, serverEndpoint);
    ServerWorld server(*serverT, /*interestRadius=*/0.0f, /*quantize=*/true); // the real-app config

    auto clientTa = CreateUnique<HubTransport>(hub, hub->Register(), serverEndpoint);
    auto clientTb = CreateUnique<HubTransport>(hub, hub->Register(), serverEndpoint);
    ClientWorld clientA(*clientTa);
    ClientWorld clientB(*clientTb);

    const ActionState moveA = MoveState(vec2(1.0f, 0.0f));  // +x
    const ActionState moveB = MoveState(vec2(-1.0f, 0.0f)); // -x

    f64 now = 0.0;
    constexpr f32 Delta = 1.0f / 60.0f;
    for (u64 tick = 1; tick <= 160; ++tick)
    {
        now += Delta;
        server.SimStep(tick, Delta);
        server.NetPump(now, tick);
        clientA.Frame(now, tick, Delta, moveA);
        clientB.Frame(now, tick, Delta, moveB);
    }

    REQUIRE(clientA.Host->IsJoined());
    REQUIRE(clientB.Host->IsJoined());
    const ConnectionId idA = clientA.Client->AssignedId();
    const ConnectionId idB = clientB.Client->AssignedId();
    const NetId netA = server.World->Get<NetIdentity>(server.Pawns.at(idA)).Id;
    const NetId netB = server.World->Get<NetIdentity>(server.Pawns.at(idB)).Id;

    Scene& worldA = *clientA.Host->World();
    Scene& worldB = *clientB.Host->World();
    const Entity aSeesB = clientA.Host->Replication().Map().Lookup(netB);
    const Entity bSeesA = clientB.Host->Replication().Map().Lookup(netA);
    REQUIRE_FALSE(aSeesB.IsNull());
    REQUIRE_FALSE(bSeesA.IsNull());

    // The remote pawn must have MOVED from its spawn position — the frozen-at-spawn symptom would
    // leave these at ~0. Quantized snapshots must keep the remote's interpolated pose updating.
    CHECK(worldA.Get<Transform>(aSeesB).Position.x < -0.1f); // A sees B move -x
    CHECK(worldB.Get<Transform>(bSeesA).Position.x > 0.1f);  // B sees A move +x
}

TEST_CASE("A late-joining client drives the server pawn only when its tick epoch is seeded")
{
    // The failure the running app hit: the server runs for a while before a client joins, so the two
    // processes' SimClock tick epochs are far apart. With scheduled consume the server feeds each
    // seat the input STAMPED AT ITS OWN TICK NUMBER, so a client stamping input on an unrelated epoch
    // is never matched — the authoritative pawn is never driven. The fix seeds the client's clock to
    // the server's tick at join; here the test supplies the aligned tick the seed produces.
    constexpr u64 Lead = 4;
    const ActionState move = MoveState(vec2(1.0f, 0.0f));
    constexpr f32 Delta = 1.0f / 60.0f;

    SUBCASE("seeded: the client's tick is aligned to the server epoch + lead, and input drives the "
            "pawn")
    {
        auto [serverT, clientT] = LoopbackTransport::CreatePair();
        ServerWorld server(*serverT);
        server.ScheduledConsume = true;
        ClientWorld client(*clientT);

        f64 now = 0.0;
        ConnectionId id = ServerConnectionId;
        // The server runs alone to a high tick — a long-running server the client joins late.
        for (u64 s = 1; s <= 600; ++s)
        {
            now += Delta;
            server.SimStep(s, Delta);
            server.NetPump(now, s);
        }
        // The client joins; its tick is seeded to the server epoch + lead (what Application now does).
        for (u64 s = 601; s <= 900; ++s)
        {
            now += Delta;
            client.Frame(now, s + Lead, Delta, move);
            server.SimStep(s, Delta);
            server.NetPump(now, s);
            if (!server.Host->Server().Connections().empty())
            {
                id = server.Host->Server().Connections().front();
            }
        }
        REQUIRE(id != ServerConnectionId);
        REQUIRE(server.Pawns.contains(id));
        CHECK(server.World->Get<Transform>(server.Pawns.at(id)).Position.x > 0.3f);
    }

    SUBCASE("unseeded: the client stamps its own epoch and the server pawn never moves (the bug)")
    {
        auto [serverT, clientT] = LoopbackTransport::CreatePair();
        ServerWorld server(*serverT);
        server.ScheduledConsume = true;
        ClientWorld client(*clientT);

        f64 now = 0.0;
        ConnectionId id = ServerConnectionId;
        for (u64 s = 1; s <= 600; ++s)
        {
            now += Delta;
            server.SimStep(s, Delta);
            server.NetPump(now, s);
        }
        // No seeding: the client stamps input on its own epoch (tick 1, 2, 3, ...) while the server
        // consumes ticks 601+. Scheduled consume never matches — the pawn stays frozen at spawn.
        u64 clientTick = 0;
        for (u64 s = 601; s <= 900; ++s)
        {
            now += Delta;
            client.Frame(now, ++clientTick, Delta, move);
            server.SimStep(s, Delta);
            server.NetPump(now, s);
            if (!server.Host->Server().Connections().empty())
            {
                id = server.Host->Server().Connections().front();
            }
        }
        REQUIRE(id != ServerConnectionId);
        REQUIRE(server.Pawns.contains(id));
        CHECK(server.World->Get<Transform>(server.Pawns.at(id)).Position.x < 0.05f);
    }
}

TEST_CASE("A client join loads into the WorldRunner's world #0, not a parallel scene")
{
    auto [serverT, clientT] = LoopbackTransport::CreatePair();
    ServerWorld server(*serverT);

    // The client owns its world through a device-free WorldRunner exactly as Application does: world
    // #0 is opened as an empty join target, and the ClientHost's LoadLevel installs the accepted level
    // into that runner-owned world rather than into a parallel scene the host owns. This guards the
    // ClientHost scene-ownership restructure: a silent regression to a parallel client scene is caught
    // here rather than downstream.
    TypeRegistry clientTypes;
    RegisterBuiltinTypes(clientTypes);
    SystemRegistry clientSystems;
    WorldRunner runner(WorldRunnerInfo{.Types = &clientTypes, .Systems = &clientSystems});

    const WorldInstanceId world0 =
        runner.OpenWorld(WorldOpenInfo{.SimTickRate = 60, .StartSimulation = false});

    Unique<Net::Client> client = *Net::Client::Connect(
        ClientInfo{.TransportOverride = clientT.get(), .Connection = FastConfig});
    const InputSendBuffer send(InputSendBuffer::Settings{.Redundancy = 3});
    Entity localCamera = Entity::Null;
    Entity localSeat = Entity::Null;
    Entity ownPawn = Entity::Null;

    Unique<ClientHost> host = ClientHost::Create(ClientHostInfo{
        .Client = *client,
        .Assets = FakeAssets(),
        .LoadLevel = [&](AssetId) -> Scene*
        {
            // Load into a fresh scene the runner takes ownership of (InstallScene), replacing world
            // #0's empty placeholder — so the joined scene is world #0, not a parallel one.
            Unique<Scene> scene = Scene::Create(clientTypes);
            localCamera = scene->CreateEntity();
            scene->Add<Transform>(localCamera);
            scene->Add<Camera>(localCamera);
            scene->Add<CameraFollow>(localCamera);
            scene->Add<Authority>(localCamera, Authority{.Tier = Tier::Local});
            localSeat = scene->CreateEntity();
            scene->Add<Viewer>(localSeat, Viewer{.Camera = localCamera});
            scene->Add<Authority>(localSeat, Authority{.Tier = Tier::Local});
            return &runner.InstallScene(world0, std::move(scene));
        },
        .ResolvePrefab = [](AssetId) -> Ref<Prefab> { return nullptr; },
        .OnPossession =
            [&](Scene& possessScene, const Entity pawn)
        {
            ownPawn = pawn;
            if (!localCamera.IsNull() && possessScene.IsAlive(localCamera))
            {
                possessScene.Get<CameraFollow>(localCamera).Target = pawn;
            }
        },
        .Prediction = [](const Scene&, Entity) { return vector<Entity>{}; },
    });

    f64 now = 0.0;
    constexpr f32 Delta = 1.0f / 60.0f;
    for (u64 tick = 1; tick <= 90; ++tick)
    {
        now += Delta;
        server.SimStep(tick, Delta);
        server.NetPump(now, tick);
        host->Pump(now);
        if (client->State() == ClientState::Connected)
        {
            (void)client->Server().Send(Channel::UnreliableSequenced, send.Encode(0, clientTypes));
        }
    }

    REQUIRE(host->IsJoined());
    REQUIRE(host->World() != nullptr);

    // The join loaded into world #0: the host's scene IS the runner-owned world #0 scene — the
    // ClientHost owns no scene of its own.
    const World* w0 = runner.ResolveWorld(world0);
    REQUIRE(w0 != nullptr);
    CHECK(host->World() == &w0->GetScene());

    // The LoadLevel-authored local seat/camera live in that same runner-owned scene (proof it is the
    // installed scene, not the empty placeholder or a parallel one).
    Scene& world = w0->GetScene();
    REQUIRE_FALSE(localSeat.IsNull());
    CHECK(world.IsAlive(localSeat));
    CHECK(world.Has<Viewer>(localSeat));

    // The server-authoritative pawn spawned/streamed into world #0: replicated entities (NetIdentity)
    // landed here, the own seat bound Remote-tier, and the client possessed the replicated pawn — the
    // whole join stream applied into the WorldRunner's world #0, identically to the pre-change path.
    const ConnectionId id = client->AssignedId();
    REQUIRE(server.Pawns.contains(id));
    int replicated = 0;
    for (auto [entity, identity] : world.View<NetIdentity>())
    {
        (void)entity;
        (void)identity;
        ++replicated;
    }
    CHECK(replicated > 0);

    const Entity clientSeat = host->Seat();
    REQUIRE_FALSE(clientSeat.IsNull());
    CHECK(world.Get<Authority>(clientSeat).Tier == Tier::Remote);
    REQUIRE_FALSE(ownPawn.IsNull());
    CHECK(world.IsAlive(ownPawn));
    CHECK(world.Get<CameraFollow>(localCamera).Target == ownPawn);
}

TEST_CASE("Two worlds in one runner carry distinct NetRoles; authority gates each by its own role")
{
    // One WorldRunner, two worlds ticked serially each frame: A ticks Server-tier, B ticks
    // Client-tier — the per-world role the world drive stamps onto each world's SystemContext through
    // the runner's BuildContext seam, fed from a world→role map rather than a process-global role.
    // Each world holds an identical authored Server-tier pawn (Transform + Intent + Mover). The
    // authority filter runs MovementSystem only where the pawn's tier matches the world's role, so the
    // Server world advances its pawn while the Client world leaves its own frozen (it would instead
    // arrive from a snapshot stream) — per-world authority with a single runner and no transport.
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;
    systems.Register<MovementSystem>(); // the authoritative advancer HasAuthority gates

    WorldRunner runner(WorldRunnerInfo{.Types = &types, .Systems = &systems});

    // Never-dereferenced fake services: MovementSystem reads only the scene, the delta, and the role.
    alignas(16) unsigned char assetsBytes[64]{};
    alignas(16) unsigned char inputBytes[64]{};
    alignas(16) unsigned char tasksBytes[64]{};
    const auto makeContext = [&](const u64 tick, const f32 alpha, const NetRole role)
    {
        return SystemContext{
            .Assets = *reinterpret_cast<AssetManager*>(assetsBytes),
            .Input = *reinterpret_cast<Input*>(inputBytes),
            .Tasks = *reinterpret_cast<TaskSystem*>(tasksBytes),
            .Tick = tick,
            .Alpha = alpha,
            .Role = role,
        };
    };

    // Open a world driven by MovementSystem alone, seeding one authored Server-tier pawn moving +x.
    const auto openWorld = [&](const NetRole role)
    {
        return runner.OpenWorld(WorldOpenInfo{
            .SimTickRate = 60,
            .StartSimulation = true,
            .EmptySimulation = true,
            .OnLoaded =
                [](WorldInstanceId, Scene& scene, ResidencyBatch&)
            {
                const Entity pawn = scene.CreateEntity();
                scene.Add<Transform>(pawn);
                scene.Add<Intent>(pawn, Intent{.Move = vec3(1.0f, 0.0f, 0.0f)});
                scene.Add<Mover>(pawn);
                scene.Add<Authority>(pawn, Authority{.Tier = Tier::Server});
            },
            .MakeStartContext = [&makeContext, role]() { return makeContext(0, 0.0f, role); },
        });
    };

    const WorldInstanceId serverWorld = openWorld(NetRole::Server);
    const WorldInstanceId clientWorld = openWorld(NetRole::Client);

    // The host-side world→role map the drive consults: Server for A, Client for B.
    std::unordered_map<u64, NetRole> roles;
    roles[serverWorld.Value] = NetRole::Server;
    roles[clientWorld.Value] = NetRole::Client;

    constexpr f32 Delta = 1.0f / 60.0f;
    for (int frame = 0; frame < 30; ++frame)
    {
        runner.Tick(WorldTickInfo{
            .Delta = Delta,
            .BuildContext = [&](const WorldInstanceId world, const Scene&, const u64 tick,
                                const f32 alpha, bool)
            { return makeContext(tick, alpha, roles.at(world.Value)); },
        });
    }

    // The single Server-tier pawn in each world.
    const auto pawnOf = [](const Scene& scene)
    {
        Entity found = Entity::Null;
        for (auto [entity, transform, mover] : scene.View<Transform, Mover>())
        {
            (void)transform;
            (void)mover;
            found = entity;
        }
        return found;
    };

    Scene& serverScene = runner.ResolveWorld(serverWorld)->GetScene();
    Scene& clientScene = runner.ResolveWorld(clientWorld)->GetScene();
    const Entity serverPawn = pawnOf(serverScene);
    const Entity clientPawn = pawnOf(clientScene);
    REQUIRE_FALSE(serverPawn.IsNull());
    REQUIRE_FALSE(clientPawn.IsNull());

    // The Server world advanced its authoritative pawn; the Client world left its identical pawn
    // frozen — a Client peer never advances Server-tier state, it displays the replicated mirror.
    CHECK(serverScene.Get<Transform>(serverPawn).Position.x > 0.5f);
    CHECK(clientScene.Get<Transform>(clientPawn).Position.x == doctest::Approx(0.0f));
}

TEST_CASE("One ServerHost hosts two worlds with isolated replication over separate connections")
{
    // The per-world replication ownership refactor: one ServerHost holds two hosted worlds, each with
    // its own ReplicationServer and NetId allocator, and demuxes each connection to its world's
    // instance. Two clients join over one server transport (a Hub, since a LoopbackTransport pair is
    // point-to-point), each presenting a distinct WorldKey — client A to world A over a clean link,
    // client B to world B over a lossy, reordering, duplicating one. The server drives each pawn's
    // Intent directly (no input path), so this isolates replication state. The asserts pin structural
    // isolation: each client hears only its own world's spawns/snapshots, the two worlds' NetId spaces
    // are independent (not one shared allocator), and B's fault burst leaves A's baseline uncorrupted.
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    Unique<Scene> sceneA = Scene::Create(types);
    Unique<Scene> sceneB = Scene::Create(types);

    const WorldInstanceId worldA{.Value = 1};
    const WorldInstanceId worldB{.Value = 2};
    const WorldKey keyA = WorldKey::FromU64(0xA);
    const WorldKey keyB = WorldKey::FromU64(0xB);

    const auto hub = CreateRef<Hub>();
    const u32 serverEndpoint = hub->Register();
    auto serverT = CreateUnique<HubTransport>(hub, serverEndpoint, serverEndpoint);

    // Both worlds are pre-registered under their keys: a client presenting keyA converges on world A,
    // keyB on world B — the get-or-create map keyed by the client-presented WorldKey.
    Result<Unique<ServerHost>> hostResult = ServerHost::Create(ServerHostInfo{
        .Server = ServerInfo{.TransportOverride = serverT.get(), .Connection = FastConfig},
        .WorldId = worldA,
        .Key = keyA,
        .World = *sceneA,
        .Assets = FakeAssets(),
        .LevelId = LevelId,
        .Replication = ReplicationServer::Settings{.SnapshotInterval = 2},
        .Interest = InterestSettings{.Radius = 0.0f},
    });
    REQUIRE(hostResult.has_value());
    Unique<ServerHost> host = std::move(*hostResult);
    host->AddWorld(ServerWorldInfo{
        .WorldId = worldB,
        .Key = keyB,
        .World = *sceneB,
        .LevelId = LevelId,
        .Replication = ReplicationServer::Settings{.SnapshotInterval = 2},
        .Interest = InterestSettings{.Radius = 0.0f},
    });

    std::unordered_map<ConnectionId, Entity> pawns;
    std::unordered_map<u64, InputJitterBuffer> jitter;
    MovementSystem movement;

    // The server frame: pawn each seated connection in its own world's scene, aim its Intent (A +x,
    // B -x, so each world's motion is distinct), and advance authoritative movement in both scenes.
    const auto driveServer = [&](u64 tick, f32 delta, bool moving)
    {
        sceneA->SetChangeTick(tick);
        sceneB->SetChangeTick(tick);
        for (const ConnectionId id : host->Server().Connections())
        {
            const WorldInstanceId world = host->WorldFor(id);
            Scene& scene = world == worldA ? *sceneA : *sceneB;
            const Entity seat = host->SeatFor(id);
            if (seat.IsNull())
            {
                continue;
            }
            auto& possesses = scene.Get<Possesses>(seat);
            if (possesses.Pawn.IsNull() || !scene.IsAlive(possesses.Pawn))
            {
                const Entity pawn = scene.CreateEntity();
                scene.Add<Transform>(pawn);
                scene.Add<Intent>(pawn);
                scene.Add<Mover>(pawn);
                scene.Add<Authority>(pawn, Authority{.Tier = Tier::Server, .Owner = id});
                possesses.Pawn = pawn;
                pawns[id] = pawn;
            }
            const f32 dir = world == worldA ? 1.0f : -1.0f;
            scene.Get<Intent>(possesses.Pawn).Move = moving ? vec3(dir, 0.0f, 0.0f) : vec3(0.0f);
        }
        FakeContext ctx;
        ctx.Role = NetRole::Server;
        movement.OnUpdate(*sceneA, delta, ctx.Make());
        movement.OnUpdate(*sceneB, delta, ctx.Make());
    };

    f64 now = 0.0;
    constexpr f32 Delta = 1.0f / 60.0f;
    u64 tick = 0;

    // Client A presents keyA, converging on world A.
    auto clientTa = CreateUnique<HubTransport>(hub, hub->Register(), serverEndpoint);
    ClientWorld clientA(*clientTa, keyA);
    while (!clientA.Host->IsJoined() && tick < 200)
    {
        ++tick;
        now += Delta;
        driveServer(tick, Delta, true);
        host->Pump(now, tick);
        IngestConnectionInputs(*host, jitter, InputJitterBuffer::Settings{}, types);
        clientA.Frame(now, tick, Delta, std::nullopt);
    }
    REQUIRE(clientA.Host->IsJoined());

    // Client B joins world B over a lossy, reordering, duplicating link — the faults ride only its
    // own connection's inbound stream, never A's.
    auto clientTbBase = CreateUnique<HubTransport>(hub, hub->Register(), serverEndpoint);
    const FaultInjectionConfig faults{
        .DropRate = 0.25f, .DuplicateRate = 0.1f, .ReorderRate = 0.2f, .Seed = 4242};
    FaultInjectionTransport clientTb(*clientTbBase, faults);
    ClientWorld clientB(clientTb, keyB);

    for (u64 step = 0; step < 460; ++step)
    {
        ++tick;
        now += Delta;
        // Move for the bulk of the run, then a long quiescent tail so both the clean and the lossy
        // latest-wins streams converge onto the halted pose.
        driveServer(tick, Delta, step < 320);
        host->Pump(now, tick);
        IngestConnectionInputs(*host, jitter, InputJitterBuffer::Settings{}, types);
        clientA.Frame(now, tick, Delta, std::nullopt);
        clientB.Frame(now, tick, Delta, std::nullopt);
    }

    REQUIRE(clientA.Host->IsJoined());
    REQUIRE(clientB.Host->IsJoined());
    REQUIRE(host->Server().Connections().size() == 2);

    const ConnectionId idA = clientA.Client->AssignedId();
    const ConnectionId idB = clientB.Client->AssignedId();
    REQUIRE(idA != idB);

    // The connections bound to distinct worlds.
    CHECK(host->WorldFor(idA) == worldA);
    CHECK(host->WorldFor(idB) == worldB);

    // Each world's pawn moved in its own direction on the server.
    REQUIRE(pawns.contains(idA));
    REQUIRE(pawns.contains(idB));
    const Entity serverPawnA = pawns.at(idA);
    const Entity serverPawnB = pawns.at(idB);
    CHECK(sceneA->Get<Transform>(serverPawnA).Position.x > 0.5f);
    CHECK(sceneB->Get<Transform>(serverPawnB).Position.x < -0.5f);

    // NetIds do not cross instances: each world allocates from its own counter, so the two worlds'
    // seats share a NetId value (both the first id) and their pawns share the next — proof the id
    // spaces are independent, not one shared allocator handing out 1, 2, 3, 4.
    const NetId seatNetA = sceneA->Get<NetIdentity>(host->SeatFor(idA)).Id;
    const NetId seatNetB = sceneB->Get<NetIdentity>(host->SeatFor(idB)).Id;
    const NetId pawnNetA = sceneA->Get<NetIdentity>(serverPawnA).Id;
    const NetId pawnNetB = sceneB->Get<NetIdentity>(serverPawnB).Id;
    CHECK(seatNetA == seatNetB);
    CHECK(pawnNetA == pawnNetB);
    CHECK(seatNetA != pawnNetA);

    // Snapshots/spawns applied only to their own world's client: each client's replication map holds
    // exactly its own seat + pawn (2), never the peer world's entities (which share its NetId values).
    CHECK(clientA.Host->Replication().Map().Size() == 2);
    CHECK(clientB.Host->Replication().Map().Size() == 2);

    // The clean world converged tightly; the faulted world converged too (looser) — B's drop/reorder
    // burst cost bandwidth, never correctness, and never touched A's baseline.
    Scene& clientWorldA = *clientA.Host->World();
    Scene& clientWorldB = *clientB.Host->World();
    REQUIRE_FALSE(clientA.OwnPawn.IsNull());
    REQUIRE_FALSE(clientB.OwnPawn.IsNull());
    const vec3 clientPosA = clientWorldA.Get<Transform>(clientA.OwnPawn).Position;
    const vec3 clientPosB = clientWorldB.Get<Transform>(clientB.OwnPawn).Position;
    CHECK(glm::length(clientPosA - sceneA->Get<Transform>(serverPawnA).Position) < 0.05f);
    CHECK(glm::length(clientPosB - sceneB->Get<Transform>(serverPawnB).Position) < 0.2f);

    // Each client saw its own world's motion, not the peer's: A's pawn moved +x, B's -x.
    CHECK(clientPosA.x > 0.1f);
    CHECK(clientPosB.x < -0.1f);
}

// ---- The multiplexed transport: N worlds over one connection --------------------------------------

namespace
{
    // A client that multiplexes several joined worlds over its one connection, tracking each join's
    // scene and possessed pawn by JoinId. AutoJoin is off; the test calls Join per world.
    struct MultiplexClient
    {
        TypeRegistry Types;
        Unique<Net::Client> Client;
        Unique<ClientHost> Host;
        RemoteInterpolationSystem Interp;
        // The client scene each join loaded, keyed by the level id the reply named.
        std::unordered_map<u64, Unique<Scene>> Scenes;
        std::unordered_map<u64, Entity> PawnByJoin; // possessed pawn, keyed by JoinId

        explicit MultiplexClient(Transport& transport)
        {
            RegisterBuiltinTypes(Types);
            Interp.SetSettings(RemoteInterpolationSystem::Settings{
                .SnapshotInterval = 2, .InterpolationDelayIntervals = 2, .SimTickRate = 60.0});
            Client = *Net::Client::Connect(
                ClientInfo{.TransportOverride = &transport, .Connection = FastConfig});
            Host = ClientHost::Create(ClientHostInfo{
                .Client = *Client,
                .Assets = FakeAssets(),
                .AutoJoin = false,
                .LoadLevel = [this](AssetId level) -> Scene*
                {
                    Unique<Scene>& scene = Scenes[level.Value];
                    scene = Scene::Create(Types);
                    return scene.get();
                },
                .ResolvePrefab = [](AssetId) -> Ref<Prefab> { return nullptr; },
                .OnPossession =
                    [this](Scene& world, const Entity pawn)
                {
                    // Attribute the possession to the join whose scene this is.
                    for (const JoinId join : Host->Joins())
                    {
                        if (Host->World(join) == &world)
                        {
                            PawnByJoin[join] = pawn;
                        }
                    }
                },
                .Prediction = [](const Scene&, Entity) { return vector<Entity>{}; },
            });
        }

        // Advance the join flow + apply each world's stream, then run the View-phase interpolation for
        // every joined world so each remote pose tracks its own world's buffered snapshots.
        void Frame(f64 now, f32 delta)
        {
            Host->Pump(now);
            FakeContext ctx;
            ctx.Role = NetRole::Client;
            for (const JoinId join : Host->Joins())
            {
                if (Scene* world = Host->World(join))
                {
                    Interp.OnUpdate(*world, delta, ctx.Make());
                }
            }
        }
    };

    // Pawns and moves each seat in its own world's scene; scenes are borrowed by WorldInstanceId value.
    void DriveWorlds(ServerHost& host, const std::unordered_map<u64, Scene*>& scenes,
                     const std::unordered_map<u64, f32>& dirByWorld, MovementSystem& movement,
                     std::unordered_map<u64, Entity>& pawnByWorld, u64 tick, f32 delta)
    {
        for (const auto& [value, scene] : scenes)
        {
            scene->SetChangeTick(tick);
        }
        for (const ConnectionId id : host.Server().Connections())
        {
            for (const JoinId join : host.JoinsFor(id))
            {
                const WorldInstanceId world = host.WorldForJoin(id, join);
                Scene& scene = *scenes.at(world.Value);
                const Entity seat = host.SeatFor(id, join);
                if (seat.IsNull())
                {
                    continue;
                }
                auto& possesses = scene.Get<Possesses>(seat);
                if (possesses.Pawn.IsNull() || !scene.IsAlive(possesses.Pawn))
                {
                    const Entity pawn = scene.CreateEntity();
                    scene.Add<Transform>(pawn);
                    scene.Add<Intent>(pawn);
                    scene.Add<Mover>(pawn);
                    scene.Add<Authority>(pawn, Authority{.Tier = Tier::Server, .Owner = id});
                    possesses.Pawn = pawn;
                    pawnByWorld[world.Value] = pawn;
                }
                scene.Get<Intent>(possesses.Pawn).Move =
                    vec3(dirByWorld.at(world.Value), 0.0f, 0.0f);
            }
        }
        FakeContext ctx;
        ctx.Role = NetRole::Server;
        for (const auto& [value, scene] : scenes)
        {
            movement.OnUpdate(*scene, delta, ctx.Make());
        }
    }
}

TEST_CASE("One client multiplexes two worlds over one connection; identical NetIds route by JoinId")
{
    auto [serverT, clientT] = LoopbackTransport::CreatePair();

    TypeRegistry serverTypes;
    RegisterBuiltinTypes(serverTypes);
    Unique<Scene> sceneA = Scene::Create(serverTypes);
    Unique<Scene> sceneB = Scene::Create(serverTypes);
    const WorldInstanceId worldA{.Value = 1};
    const WorldInstanceId worldB{.Value = 2};
    const WorldKey keyA = WorldKey::FromU64(0xA);
    const WorldKey keyB = WorldKey::FromU64(0xB);
    const AssetId levelA{0x00000000000000A1ULL};
    const AssetId levelB{0x00000000000000B2ULL};

    Result<Unique<ServerHost>> hostR = ServerHost::Create(ServerHostInfo{
        .Server = ServerInfo{.TransportOverride = serverT.get(), .Connection = FastConfig},
        .WorldId = worldA,
        .Key = keyA,
        .World = *sceneA,
        .Assets = FakeAssets(),
        .LevelId = levelA,
        .Replication = ReplicationServer::Settings{.SnapshotInterval = 2},
        .Interest = InterestSettings{.Radius = 0.0f},
    });
    REQUIRE(hostR.has_value());
    Unique<ServerHost> host = std::move(*hostR);
    host->AddWorld(
        ServerWorldInfo{.WorldId = worldB,
                        .Key = keyB,
                        .World = *sceneB,
                        .LevelId = levelB,
                        .Replication = ReplicationServer::Settings{.SnapshotInterval = 2},
                        .Interest = InterestSettings{.Radius = 0.0f}});

    MultiplexClient client(*clientT);
    const std::unordered_map<u64, Scene*> scenes{{worldA.Value, sceneA.get()},
                                                 {worldB.Value, sceneB.get()}};
    const std::unordered_map<u64, f32> dirs{{worldA.Value, 1.0f}, {worldB.Value, -1.0f}};
    MovementSystem movement;
    std::unordered_map<u64, Entity> pawnByWorld;
    std::unordered_map<u64, InputJitterBuffer> jitter;

    f64 now = 0.0;
    constexpr f32 Delta = 1.0f / 60.0f;
    bool joined = false;
    for (u64 tick = 1; tick <= 260; ++tick)
    {
        now += Delta;
        DriveWorlds(*host, scenes, dirs, movement, pawnByWorld, tick, Delta);
        host->Pump(now, tick);
        IngestConnectionInputs(*host, jitter, InputJitterBuffer::Settings{}, serverTypes);
        client.Frame(now, Delta);
        if (!joined && client.Client->State() == ClientState::Connected)
        {
            client.Host->Join(keyA);
            client.Host->Join(keyB);
            joined = true;
        }
    }

    // Both worlds joined over the one connection, each under its own JoinId.
    REQUIRE(client.Host->Joins().size() == 2);
    const JoinId joinA = client.Host->Joins()[0]; // keyA requested first ⇒ lower JoinId
    const JoinId joinB = client.Host->Joins()[1];
    Scene* clientA = client.Host->World(joinA);
    Scene* clientB = client.Host->World(joinB);
    REQUIRE(clientA != nullptr);
    REQUIRE(clientB != nullptr);
    CHECK(clientA != clientB);

    // The two worlds allocate NetIds from their own counters, so the pawn shares a NetId value (2)
    // across both worlds — proof the wire key is (JoinId, NetId), not NetId alone.
    const NetId pawnNetA = sceneA->Get<NetIdentity>(pawnByWorld.at(worldA.Value)).Id;
    const NetId pawnNetB = sceneB->Get<NetIdentity>(pawnByWorld.at(worldB.Value)).Id;
    CHECK(pawnNetA == pawnNetB);

    // The identical NetId routes to the correct scene by JoinId: join A's map resolves it to a pawn
    // that moved +x in scene A, join B's to one that moved -x in scene B.
    const Entity aPawn = client.Host->Replication(joinA).Map().Lookup(pawnNetA);
    const Entity bPawn = client.Host->Replication(joinB).Map().Lookup(pawnNetB);
    REQUIRE_FALSE(aPawn.IsNull());
    REQUIRE_FALSE(bPawn.IsNull());
    CHECK(clientA->IsAlive(aPawn));
    CHECK(clientB->IsAlive(bPawn));
    CHECK(clientA->Get<Transform>(aPawn).Position.x > 0.1f);
    CHECK(clientB->Get<Transform>(bPawn).Position.x < -0.1f);

    // Each joined world keeps its own clock: both per-JoinId controllers tracked a server tick.
    CHECK(client.Host->LastServerTick(joinA) > 0);
    CHECK(client.Host->LastServerTick(joinB) > 0);
}

TEST_CASE(
    "With the auto-join off, each joined WorldKey lands in its own runner world, not world #0")
{
    // The client complement of the server's per-WorldKey worlds, exercised through the Application
    // primitives it composes: a device-free WorldRunner, a ClientHost with AutoJoin off, and a per-join
    // pending-target queue whose LoadLevel installs each reply's scene into the runner world its join
    // opened. It guards the client-side per-world join binding: a front-end world #0 the client owns
    // independently must survive every join untouched, and two distinct WorldKeys must land in two
    // distinct runner worlds — never a second join installing over world #0's scene (the use-after-free
    // the single-managed-world client caused).
    auto [serverT, clientT] = LoopbackTransport::CreatePair();

    TypeRegistry serverTypes;
    RegisterBuiltinTypes(serverTypes);
    Unique<Scene> sceneA = Scene::Create(serverTypes);
    Unique<Scene> sceneB = Scene::Create(serverTypes);
    const WorldInstanceId worldA{.Value = 1};
    const WorldInstanceId worldB{.Value = 2};
    const WorldKey keyA = WorldKey::FromU64(0xA);
    const WorldKey keyB = WorldKey::FromU64(0xB);
    const AssetId levelA{0x00000000000000A1ULL};
    const AssetId levelB{0x00000000000000B2ULL};

    Result<Unique<ServerHost>> hostR = ServerHost::Create(ServerHostInfo{
        .Server = ServerInfo{.TransportOverride = serverT.get(), .Connection = FastConfig},
        .WorldId = worldA,
        .Key = keyA,
        .World = *sceneA,
        .Assets = FakeAssets(),
        .LevelId = levelA,
        .Replication = ReplicationServer::Settings{.SnapshotInterval = 2},
        .Interest = InterestSettings{.Radius = 0.0f},
    });
    REQUIRE(hostR.has_value());
    Unique<ServerHost> host = std::move(*hostR);
    host->AddWorld(
        ServerWorldInfo{.WorldId = worldB,
                        .Key = keyB,
                        .World = *sceneB,
                        .LevelId = levelB,
                        .Replication = ReplicationServer::Settings{.SnapshotInterval = 2},
                        .Interest = InterestSettings{.Radius = 0.0f}});

    // The client owns its worlds through a WorldRunner exactly as Application does.
    TypeRegistry clientTypes;
    RegisterBuiltinTypes(clientTypes);
    SystemRegistry clientSystems;
    WorldRunner runner(WorldRunnerInfo{.Types = &clientTypes, .Systems = &clientSystems});

    // A front-end world #0 the client owns independently of any join; it is never a join target, so it
    // must stay untouched — the crash was a second join freeing world #0's scene out from under it.
    const WorldInstanceId frontEnd =
        runner.OpenWorld(WorldOpenInfo{.SimTickRate = 60, .StartSimulation = false});
    Scene* const frontEndScene = &runner.ResolveWorld(frontEnd)->GetScene();
    const Entity sentinel = frontEndScene->CreateEntity();

    Unique<Net::Client> client = *Net::Client::Connect(
        ClientInfo{.TransportOverride = clientT.get(), .Connection = FastConfig});
    const InputSendBuffer send(InputSendBuffer::Settings{.Redundancy = 3});

    // The join's runner-world target queue: each join pushes the world it opened, LoadLevel pops it FIFO
    // (replies arrive in request order over the reliable channel) — Application::JoinWorld's mechanism.
    std::deque<WorldInstanceId> pending;
    std::unordered_map<u64, Entity>
        seatByWorld; // the LoadLevel-authored local seat, by runner world

    Unique<ClientHost> clientHost = ClientHost::Create(ClientHostInfo{
        .Client = *client,
        .Assets = FakeAssets(),
        .AutoJoin = false,
        .LoadLevel = [&](AssetId) -> Scene*
        {
            REQUIRE_FALSE(pending.empty());
            const WorldInstanceId target = pending.front();
            pending.pop_front();
            Unique<Scene> scene = Scene::Create(clientTypes);
            const Entity camera = scene->CreateEntity();
            scene->Add<Transform>(camera);
            scene->Add<Camera>(camera);
            scene->Add<CameraFollow>(camera);
            scene->Add<Authority>(camera, Authority{.Tier = Tier::Local});
            const Entity seat = scene->CreateEntity();
            scene->Add<Viewer>(seat, Viewer{.Camera = camera});
            scene->Add<Authority>(seat, Authority{.Tier = Tier::Local});
            seatByWorld[target.Value] = seat;
            return &runner.InstallScene(target, std::move(scene));
        },
        .ResolvePrefab = [](AssetId) -> Ref<Prefab> { return nullptr; },
        .Prediction = [](const Scene&, Entity) { return vector<Entity>{}; },
    });

    // Application::JoinWorld's shape: open a fresh runner world, queue it, request the join.
    auto joinWorld = [&](const WorldKey& key) -> WorldInstanceId
    {
        const WorldInstanceId world =
            runner.OpenWorld(WorldOpenInfo{.SimTickRate = 60, .StartSimulation = false});
        pending.push_back(world);
        clientHost->Join(key);
        return world;
    };

    const std::unordered_map<u64, Scene*> scenes{{worldA.Value, sceneA.get()},
                                                 {worldB.Value, sceneB.get()}};
    const std::unordered_map<u64, f32> dirs{{worldA.Value, 1.0f}, {worldB.Value, -1.0f}};
    MovementSystem movement;
    std::unordered_map<u64, Entity> pawnByWorld;
    std::unordered_map<u64, InputJitterBuffer> jitter;

    WorldInstanceId gameA{};
    WorldInstanceId gameB{};
    f64 now = 0.0;
    constexpr f32 Delta = 1.0f / 60.0f;
    bool joined = false;
    for (u64 tick = 1; tick <= 200; ++tick)
    {
        now += Delta;
        DriveWorlds(*host, scenes, dirs, movement, pawnByWorld, tick, Delta);
        host->Pump(now, tick);
        IngestConnectionInputs(*host, jitter, InputJitterBuffer::Settings{}, serverTypes);
        clientHost->Pump(now);
        if (client->State() == ClientState::Connected)
        {
            if (!joined)
            {
                gameA = joinWorld(keyA);
                gameB = joinWorld(keyB);
                joined = true;
            }
            (void)client->Server().Send(Channel::UnreliableSequenced, send.Encode(0, clientTypes));
        }
    }

    // Both joins landed, each into its own opened runner world — neither is the front-end world #0.
    REQUIRE(clientHost->Joins().size() == 2);
    CHECK(gameA != frontEnd);
    CHECK(gameB != frontEnd);
    CHECK(gameA != gameB);

    const JoinId joinA = clientHost->Joins()[0]; // keyA requested first ⇒ lower JoinId
    const JoinId joinB = clientHost->Joins()[1];

    // The join's scene IS the runner world it opened (the ClientHost owns no scene of its own) — the
    // per-world binding the fix installs, keyed FIFO to the requesting join.
    const World* runnerA = runner.ResolveWorld(gameA);
    const World* runnerB = runner.ResolveWorld(gameB);
    REQUIRE(runnerA != nullptr);
    REQUIRE(runnerB != nullptr);
    Scene* const clientA = &runnerA->GetScene();
    Scene* const clientB = &runnerB->GetScene();
    CHECK(clientHost->World(joinA) == clientA);
    CHECK(clientHost->World(joinB) == clientB);
    CHECK(clientA != clientB);

    // The front-end world #0 survived both joins untouched: same scene object, sentinel still alive —
    // the second join never freed or overwrote a peer world's scene (the use-after-free regression).
    CHECK(&runner.ResolveWorld(frontEnd)->GetScene() == frontEndScene);
    CHECK(frontEndScene->IsAlive(sentinel));
    CHECK(clientA != frontEndScene);
    CHECK(clientB != frontEndScene);

    // Each joined world is presentable: its LoadLevel-authored local seat lives in the runner-owned
    // scene, and the server pawn streamed in and moved in its own direction (proof both worlds' streams
    // applied to their own scene, not one clobbering the other).
    CHECK(clientA->IsAlive(seatByWorld.at(gameA.Value)));
    CHECK(clientB->IsAlive(seatByWorld.at(gameB.Value)));

    const NetId pawnNetA = sceneA->Get<NetIdentity>(pawnByWorld.at(worldA.Value)).Id;
    const NetId pawnNetB = sceneB->Get<NetIdentity>(pawnByWorld.at(worldB.Value)).Id;
    const Entity aPawn = clientHost->Replication(joinA).Map().Lookup(pawnNetA);
    const Entity bPawn = clientHost->Replication(joinB).Map().Lookup(pawnNetB);
    REQUIRE_FALSE(aPawn.IsNull());
    REQUIRE_FALSE(bPawn.IsNull());
    CHECK(clientA->IsAlive(aPawn));
    CHECK(clientB->IsAlive(bPawn));
    CHECK(clientA->Get<Transform>(aPawn).Position.x > 0.1f);
    CHECK(clientB->Get<Transform>(bPawn).Position.x < -0.1f);
}

TEST_CASE("A drop/reorder burst on the shared connection leaves both multiplexed worlds converging")
{
    auto [serverT, clientTBase] = LoopbackTransport::CreatePair();
    // The whole connection (both worlds' streams) rides a lossy, reordering, duplicating link — the
    // worlds share the channels, so the honest guarantee is convergence, not stream independence.
    const FaultInjectionConfig faults{
        .DropRate = 0.2f, .DuplicateRate = 0.1f, .ReorderRate = 0.15f, .Seed = 909};
    FaultInjectionTransport clientLink(*clientTBase, faults);

    TypeRegistry serverTypes;
    RegisterBuiltinTypes(serverTypes);
    Unique<Scene> sceneA = Scene::Create(serverTypes);
    Unique<Scene> sceneB = Scene::Create(serverTypes);
    const WorldInstanceId worldA{.Value = 1};
    const WorldInstanceId worldB{.Value = 2};
    const WorldKey keyA = WorldKey::FromU64(0xA);
    const WorldKey keyB = WorldKey::FromU64(0xB);

    Result<Unique<ServerHost>> hostR = ServerHost::Create(ServerHostInfo{
        .Server = ServerInfo{.TransportOverride = serverT.get(), .Connection = FastConfig},
        .WorldId = worldA,
        .Key = keyA,
        .World = *sceneA,
        .Assets = FakeAssets(),
        .LevelId = AssetId{0x00000000000000A1ULL},
        .Replication = ReplicationServer::Settings{.SnapshotInterval = 2},
        .Interest = InterestSettings{.Radius = 0.0f},
    });
    REQUIRE(hostR.has_value());
    Unique<ServerHost> host = std::move(*hostR);
    host->AddWorld(
        ServerWorldInfo{.WorldId = worldB,
                        .Key = keyB,
                        .World = *sceneB,
                        .LevelId = AssetId{0x00000000000000B2ULL},
                        .Replication = ReplicationServer::Settings{.SnapshotInterval = 2},
                        .Interest = InterestSettings{.Radius = 0.0f}});

    MultiplexClient client(clientLink);
    const std::unordered_map<u64, Scene*> scenes{{worldA.Value, sceneA.get()},
                                                 {worldB.Value, sceneB.get()}};
    MovementSystem movement;
    std::unordered_map<u64, Entity> pawnByWorld;
    std::unordered_map<u64, InputJitterBuffer> jitter;

    f64 now = 0.0;
    constexpr f32 Delta = 1.0f / 60.0f;
    bool joined = false;
    for (u64 tick = 1; tick <= 620; ++tick)
    {
        now += Delta;
        // Drive both worlds, then halt with a long quiescent tail so the lossy latest-wins streams
        // converge onto the stopped pose.
        const f32 speed = tick < 420 ? 1.0f : 0.0f;
        const std::unordered_map<u64, f32> dirs{{worldA.Value, speed}, {worldB.Value, -speed}};
        DriveWorlds(*host, scenes, dirs, movement, pawnByWorld, tick, Delta);
        host->Pump(now, tick);
        IngestConnectionInputs(*host, jitter, InputJitterBuffer::Settings{}, serverTypes);
        client.Frame(now, Delta);
        if (!joined && client.Client->State() == ClientState::Connected)
        {
            client.Host->Join(keyA);
            client.Host->Join(keyB);
            joined = true;
        }
    }

    REQUIRE(client.Host->Joins().size() == 2);
    const JoinId joinA = client.Host->Joins()[0];
    const JoinId joinB = client.Host->Joins()[1];
    const NetId netA = sceneA->Get<NetIdentity>(pawnByWorld.at(worldA.Value)).Id;
    const NetId netB = sceneB->Get<NetIdentity>(pawnByWorld.at(worldB.Value)).Id;
    const Entity aPawn = client.Host->Replication(joinA).Map().Lookup(netA);
    const Entity bPawn = client.Host->Replication(joinB).Map().Lookup(netB);
    REQUIRE_FALSE(aPawn.IsNull());
    REQUIRE_FALSE(bPawn.IsNull());

    // Both worlds converged despite the loss burst — neither's baseline was corrupted by the other.
    const vec3 serverA = sceneA->Get<Transform>(pawnByWorld.at(worldA.Value)).Position;
    const vec3 serverB = sceneB->Get<Transform>(pawnByWorld.at(worldB.Value)).Position;
    CHECK(glm::length(client.Host->World(joinA)->Get<Transform>(aPawn).Position - serverA) < 0.25f);
    CHECK(glm::length(client.Host->World(joinB)->Get<Transform>(bPawn).Position - serverB) < 0.25f);
    CHECK(serverA.x > 0.1f);  // A moved +x
    CHECK(serverB.x < -0.1f); // B moved -x
}

namespace
{
    // A ServerHost whose worlds are all get-or-create factory-opened (a distinct scene per WorldKey),
    // plus a never-joined primary. Tracks open/close counts so the convergence, cap, and reap
    // behaviors can be asserted. The primary is registered under a key no client presents.
    struct FactoryServer
    {
        TypeRegistry Types;
        Unique<Scene> Primary;
        Unique<ServerHost> Host;
        std::unordered_map<u64, Unique<Scene>> Scenes; // factory scenes, by WorldInstanceId value
        u64 NextWorld = 100;
        u64 NextLevel = 0x1000;
        u32 OpenCount = 0;
        u32 CloseCount = 0;
        vector<u64> Closed;
        std::unordered_map<u64, InputJitterBuffer> Jitter;

        explicit FactoryServer(Transport& transport, u32 maxHosted = 64, u32 maxPerConn = 4,
                               f64 dwell = 5.0, u32 maxPerInstance = 0)
        {
            RegisterBuiltinTypes(Types);
            Primary = Scene::Create(Types);
            Result<Unique<ServerHost>> host = ServerHost::Create(ServerHostInfo{
                .Server = ServerInfo{.TransportOverride = &transport, .Connection = FastConfig},
                .WorldId = WorldInstanceId{.Value = 1},
                .Key = WorldKey::FromU64(0xFFFFFFFFULL), // primary key, presented by no client
                .World = *Primary,
                .Assets = FakeAssets(),
                .LevelId = LevelId,
                .Replication = ReplicationServer::Settings{.SnapshotInterval = 2},
                .Interest = InterestSettings{.Radius = 0.0f},
                .MaxJoinedWorldsPerConnection = maxPerConn,
                .MaxHostedWorlds = maxHosted,
                .MaxPlayersPerInstance = maxPerInstance,
                .IdleKeepWarmDwell = dwell,
                .WorldFactory = [this](const WorldKey&,
                                       const TravelPayload&) -> optional<ServerWorldResolution>
                {
                    const u64 world = NextWorld++;
                    const u64 level = NextLevel++;
                    Unique<Scene>& scene = Scenes[world];
                    scene = Scene::Create(Types);
                    ++OpenCount;
                    return ServerWorldResolution{
                        .WorldId = WorldInstanceId{.Value = world},
                        .World = scene.get(),
                        .LevelId = AssetId{.Value = level},
                        .Replication = ReplicationServer::Settings{.SnapshotInterval = 2},
                        .Interest = InterestSettings{.Radius = 0.0f}};
                },
                .CloseWorld =
                    [this](WorldInstanceId id)
                {
                    ++CloseCount;
                    Closed.push_back(id.Value);
                    Scenes.erase(id.Value);
                },
            });
            REQUIRE(host.has_value());
            Host = std::move(*host);
        }

        void Pump(f64 now, u64 tick)
        {
            Host->Pump(now, tick);
            IngestConnectionInputs(*Host, Jitter, InputJitterBuffer::Settings{}, Types);
        }
    };
}

TEST_CASE("A second client presenting the same WorldKey converges on the existing instance")
{
    const auto hub = CreateRef<Hub>();
    const u32 serverEndpoint = hub->Register();
    auto serverT = CreateUnique<HubTransport>(hub, serverEndpoint, serverEndpoint);
    FactoryServer server(*serverT);

    const WorldKey shared = WorldKey::FromU64(0x5EED);

    auto clientTa = CreateUnique<HubTransport>(hub, hub->Register(), serverEndpoint);
    auto clientTb = CreateUnique<HubTransport>(hub, hub->Register(), serverEndpoint);
    ClientWorld clientA(*clientTa, shared);
    ClientWorld clientB(*clientTb, shared);

    f64 now = 0.0;
    constexpr f32 Delta = 1.0f / 60.0f;
    for (u64 tick = 1; tick <= 120; ++tick)
    {
        now += Delta;
        server.Pump(now, tick);
        clientA.Frame(now, tick, Delta, std::nullopt);
        clientB.Frame(now, tick, Delta, std::nullopt);
    }

    REQUIRE(clientA.Host->IsJoined());
    REQUIRE(clientB.Host->IsJoined());

    // The factory opened the shared world exactly once — the second presenter converged on it, not a
    // second instance. The host holds the primary plus that one factory world.
    CHECK(server.OpenCount == 1);
    CHECK(server.Host->HostedWorldCount() == 2);
    CHECK(server.Host->WorldFor(clientA.Client->AssignedId()) ==
          server.Host->WorldFor(clientB.Client->AssignedId()));
}

TEST_CASE(
    "A create-on-miss past MaxHostedWorlds is rejected; a join past the per-connection cap too")
{
    auto [serverT, clientT] = LoopbackTransport::CreatePair();
    // Primary + at most two factory worlds; a connection may join at most two worlds.
    FactoryServer server(*serverT, /*maxHosted=*/3, /*maxPerConn=*/2);

    MultiplexClient client(*clientT);

    f64 now = 0.0;
    constexpr f32 Delta = 1.0f / 60.0f;
    bool requested = false;
    for (u64 tick = 1; tick <= 160; ++tick)
    {
        now += Delta;
        server.Pump(now, tick);
        client.Frame(now, Delta);
        if (!requested && client.Client->State() == ClientState::Connected)
        {
            // Three distinct keys, but the per-connection cap is two: the third join is refused.
            client.Host->Join(WorldKey::FromU64(0x1));
            client.Host->Join(WorldKey::FromU64(0x2));
            client.Host->Join(WorldKey::FromU64(0x3));
            requested = true;
        }
    }

    // The per-connection cap (2) bounds the joins that land; the server opened only those two worlds,
    // never breaching the hosted-worlds cap.
    CHECK(client.Host->Joins().size() == 2);
    CHECK(server.OpenCount == 2);
    CHECK(server.Host->HostedWorldCount() == 3); // primary + two factory worlds
}

TEST_CASE(
    "A hosted world whose last join leaves is reaped after the idle dwell; a re-join reuses it")
{
    auto [serverT, clientT] = LoopbackTransport::CreatePair();
    constexpr f64 Dwell = 0.5;
    FactoryServer server(*serverT, /*maxHosted=*/64, /*maxPerConn=*/4, Dwell);

    const WorldKey key = WorldKey::FromU64(0xC0FFEE);

    // A minimal client we can construct, join, and drop under test control.
    const auto makeClient = [&](Unique<Net::Client>& client, Unique<ClientHost>& host,
                                Unique<Scene>& scene, TypeRegistry& types)
    {
        RegisterBuiltinTypes(types);
        client = *Net::Client::Connect(
            ClientInfo{.TransportOverride = clientT.get(), .Connection = FastConfig});
        host = ClientHost::Create(ClientHostInfo{
            .Client = *client,
            .Assets = FakeAssets(),
            .WorldKey = key,
            .LoadLevel = [&](AssetId) -> Scene*
            {
                scene = Scene::Create(types);
                return scene.get();
            },
            .ResolvePrefab = [](AssetId) -> Ref<Prefab> { return nullptr; },
        });
    };

    f64 now = 0.0;
    constexpr f32 Delta = 1.0f / 60.0f;

    {
        Unique<Net::Client> client;
        Unique<ClientHost> host;
        Unique<Scene> scene;
        TypeRegistry types;
        makeClient(client, host, scene, types);
        for (u64 tick = 1; tick <= 16 && !host->IsJoined(); ++tick)
        {
            now += Delta;
            server.Pump(now, tick);
            host->Pump(now);
        }
        REQUIRE(host->IsJoined());
        CHECK(server.OpenCount == 1);
        client->Disconnect();
        // Advance a little past the disconnect but not yet the dwell: the world stays warm.
        for (u64 tick = 17; tick <= 20; ++tick)
        {
            now += Delta;
            host->Pump(now);
            server.Pump(now, tick);
        }
        CHECK(server.CloseCount == 0);
        CHECK(server.Host->HostedWorldCount() == 2); // primary + the warm factory world
    }

    // Advance well past the idle dwell with no joins: the factory world is reaped.
    for (u64 tick = 21; tick <= 120; ++tick)
    {
        now += Delta;
        server.Pump(now, tick);
    }
    CHECK(server.CloseCount == 1);
    CHECK(server.Host->HostedWorldCount() == 1); // only the primary remains

    // A re-join opens a fresh instance for the key (the old one was reaped past the dwell).
    Unique<Net::Client> client2;
    Unique<ClientHost> host2;
    Unique<Scene> scene2;
    TypeRegistry types2;
    makeClient(client2, host2, scene2, types2);
    for (u64 tick = 121; tick <= 140 && !host2->IsJoined(); ++tick)
    {
        now += Delta;
        server.Pump(now, tick);
        host2->Pump(now);
    }
    REQUIRE(host2->IsJoined());
    CHECK(server.OpenCount == 2); // a second open: the reaped world did not survive the dwell
}

TEST_CASE("A join whose client-reconstructed world mismatches the echoed digest is rejected loudly")
{
    auto [serverT, clientT] = LoopbackTransport::CreatePair();

    TypeRegistry serverTypes;
    RegisterBuiltinTypes(serverTypes);
    Unique<Scene> serverScene = Scene::Create(serverTypes);

    // The server's world carries a nonzero content digest; the client will reconstruct a different one.
    Result<Unique<ServerHost>> hostR = ServerHost::Create(ServerHostInfo{
        .Server = ServerInfo{.TransportOverride = serverT.get(), .Connection = FastConfig},
        .World = *serverScene,
        .Assets = FakeAssets(),
        .LevelId = LevelId,
        .Digest = ContentDigest{.Lo = 0xDEADBEEF, .Hi = 0x1234},
        .Replication = ReplicationServer::Settings{.SnapshotInterval = 2},
    });
    REQUIRE(hostR.has_value());
    Unique<ServerHost> host = std::move(*hostR);

    Unique<Net::Client> client = *Net::Client::Connect(
        ClientInfo{.TransportOverride = clientT.get(), .Connection = FastConfig});

    TypeRegistry clientTypes;
    RegisterBuiltinTypes(clientTypes);
    bool loaded = false;
    Unique<ClientHost> clientHost = ClientHost::Create(ClientHostInfo{
        .Client = *client,
        .Assets = FakeAssets(),
        // The client's own digest of the joined key disagrees with the server's echoed one.
        .WorldDigest = [](const WorldKey&, const TravelPayload&)
        { return ContentDigest{.Lo = 0x0}; },
        .LoadLevel = [&](AssetId) -> Scene*
        {
            loaded = true;
            return nullptr;
        },
        .ResolvePrefab = [](AssetId) -> Ref<Prefab> { return nullptr; },
    });

    f64 now = 0.0;
    constexpr f32 Delta = 1.0f / 60.0f;
    for (u64 tick = 1; tick <= 30; ++tick)
    {
        now += Delta;
        serverScene->SetChangeTick(tick);
        host->Pump(now, tick);
        clientHost->Pump(now);
    }

    // The connection is up, but the join was rejected on the digest mismatch: no level was ever
    // loaded and no world was joined — the stream is never applied against a diverged scene.
    REQUIRE(client->State() == ClientState::Connected);
    CHECK_FALSE(loaded);
    CHECK_FALSE(clientHost->IsJoined());
    CHECK(clientHost->Joins().empty());
}

TEST_CASE("A join whose client-supplied per-key digest matches the echoed digest is admitted")
{
    auto [serverT, clientT] = LoopbackTransport::CreatePair();

    TypeRegistry serverTypes;
    RegisterBuiltinTypes(serverTypes);
    Unique<Scene> serverScene = Scene::Create(serverTypes);

    // The server echoes this per-world content digest; the client reconstructs the same value.
    constexpr ContentDigest ServerDigest{.Lo = 0x00ABCDEF, .Hi = 0x99};
    Result<Unique<ServerHost>> hostR = ServerHost::Create(ServerHostInfo{
        .Server = ServerInfo{.TransportOverride = serverT.get(), .Connection = FastConfig},
        .World = *serverScene,
        .Assets = FakeAssets(),
        .LevelId = LevelId,
        .Digest = ServerDigest,
        .Replication = ReplicationServer::Settings{.SnapshotInterval = 2},
    });
    REQUIRE(hostR.has_value());
    Unique<ServerHost> host = std::move(*hostR);

    Unique<Net::Client> client = *Net::Client::Connect(
        ClientInfo{.TransportOverride = clientT.get(), .Connection = FastConfig});

    TypeRegistry clientTypes;
    RegisterBuiltinTypes(clientTypes);
    Unique<Scene> clientScene;
    bool loaded = false;
    WorldKey seenKey;
    Unique<ClientHost> clientHost = ClientHost::Create(ClientHostInfo{
        .Client = *client,
        .Assets = FakeAssets(),
        // The per-key provider yields the digest the client expects for the key it joins; it matches
        // the server's echo for the auto-joined DefaultWorldKey, so the join is admitted.
        .WorldDigest = [&](const WorldKey& key, const TravelPayload&) -> ContentDigest
        {
            seenKey = key;
            return key == DefaultWorldKey ? ServerDigest : ContentDigest{};
        },
        .LoadLevel = [&](AssetId) -> Scene*
        {
            loaded = true;
            clientScene = Scene::Create(clientTypes);
            return clientScene.get();
        },
        .ResolvePrefab = [](AssetId) -> Ref<Prefab> { return nullptr; },
    });

    f64 now = 0.0;
    constexpr f32 Delta = 1.0f / 60.0f;
    for (u64 tick = 1; tick <= 30 && !clientHost->IsJoined(); ++tick)
    {
        now += Delta;
        serverScene->SetChangeTick(tick);
        host->Pump(now, tick);
        clientHost->Pump(now);
    }

    // The matching digest cleared validation: the level loaded and the world joined, the provider was
    // consulted with the joined key, and the stream applies against the accepted scene.
    REQUIRE(client->State() == ClientState::Connected);
    CHECK(seenKey == DefaultWorldKey);
    CHECK(loaded);
    CHECK(clientHost->IsJoined());
    CHECK(clientHost->Joins().size() == 1);
}

TEST_CASE("A peer with a stale ProtocolVersion is rejected at the connection handshake")
{
    auto [serverT, clientT] = LoopbackTransport::CreatePair();

    TypeRegistry serverTypes;
    RegisterBuiltinTypes(serverTypes);
    Unique<Scene> serverScene = Scene::Create(serverTypes);

    Result<Unique<ServerHost>> hostR = ServerHost::Create(ServerHostInfo{
        .Server = ServerInfo{.TransportOverride = serverT.get(), .Connection = FastConfig},
        .World = *serverScene,
        .Assets = FakeAssets(),
        .LevelId = LevelId,
    });
    REQUIRE(hostR.has_value());
    Unique<ServerHost> host = std::move(*hostR);

    // A client advertising a different protocol version is denied at the door.
    Unique<Net::Client> client = *Net::Client::Connect(ClientInfo{
        .ProtocolVersion = Net::ProtocolVersion + 1,
        .TransportOverride = clientT.get(),
        .Connection = FastConfig,
    });

    f64 now = 0.0;
    constexpr f32 Delta = 1.0f / 60.0f;
    for (u64 tick = 1; tick <= 20 && client->State() == ClientState::Connecting; ++tick)
    {
        now += Delta;
        serverScene->SetChangeTick(tick);
        host->Pump(now, tick);
        client->Pump(now);
    }

    CHECK(client->State() == ClientState::Denied);
    REQUIRE(client->GetDenyReason().has_value());
    CHECK(*client->GetDenyReason() == DenyReason::ProtocolMismatch);
    CHECK(host->Server().Connections().empty());
}

TEST_CASE("Input tagged with an ungranted or garbage JoinId is dropped, not routed")
{
    auto [serverT, clientT] = LoopbackTransport::CreatePair();
    ServerWorld server(*serverT);
    ClientWorld client(*clientT);

    f64 now = 0.0;
    constexpr f32 Delta = 1.0f / 60.0f;

    // Join cleanly first.
    for (u64 tick = 1; tick <= 20 && !client.Host->IsJoined(); ++tick)
    {
        now += Delta;
        server.SimStep(tick, Delta);
        server.NetPump(now, tick);
        client.Frame(now, tick, Delta, std::nullopt);
    }
    REQUIRE(client.Host->IsJoined());
    const ConnectionId id = client.Client->AssignedId();
    const Entity seat = server.Host->SeatFor(id);
    REQUIRE_FALSE(seat.IsNull());
    const JoinId granted = client.Host->CurrentJoinId();

    // Fire input tagged with an ungranted JoinId, a short/garbage frame, and an unmarked frame — each
    // must be dropped before any routing, never feeding the seat and never faulting the server.
    InputSendBuffer send(InputSendBuffer::Settings{.Redundancy = 3});
    send.Stamp(1, MoveState(vec2(1.0f, 0.0f)));
    for (u64 tick = 21; tick <= 50; ++tick)
    {
        now += Delta;
        const auto ungranted = static_cast<JoinId>(granted + 7);
        (void)client.Client->Server().Send(
            Channel::UnreliableSequenced,
            EncodeWorldEnvelope(ungranted, send.Encode(0, server.Types)));
        (void)client.Client->Server().Send(Channel::UnreliableSequenced, vector<u8>{0x00, 0x01});
        (void)client.Client->Server().Send(Channel::UnreliableSequenced,
                                           vector<u8>(24, static_cast<u8>(tick)));
        server.SimStep(tick, Delta);
        server.NetPump(now, tick);
        client.Client->Pump(now);
    }

    // The server is unscathed: the connection is live, the seat intact, and no ungranted input reached
    // the seat's PlayerInput (the world's own move stayed zero, never the injected +x).
    REQUIRE(server.Host->Server().Connections().size() == 1);
    REQUIRE_FALSE(server.Host->SeatFor(id).IsNull());
    if (server.World->Has<PlayerInput>(seat))
    {
        const vec2 fed = server.World->Get<PlayerInput>(seat).GetValue(MoveAction);
        CHECK(glm::length(fed - vec2(1.0f, 0.0f)) > 1e-4f);
    }

    // A clean, correctly-tagged input after the garbage still feeds the seat — the drops were surgical.
    bool fed = false;
    for (u64 tick = 51; tick <= 90 && !fed; ++tick)
    {
        now += Delta;
        client.Frame(now, tick, Delta, MoveState(vec2(1.0f, 0.0f)));
        server.SimStep(tick, Delta);
        server.NetPump(now, tick);
        if (server.World->Has<PlayerInput>(seat) &&
            glm::length(server.World->Get<PlayerInput>(seat).GetValue(MoveAction) -
                        vec2(1.0f, 0.0f)) < 1e-4f)
        {
            fed = true;
        }
    }
    CHECK(fed);
}

// ---- Instance placement (the get-or-place policy) ------------------------------------------------

TEST_CASE("MaxPlayersPerInstance = 0 converges every joiner of a key on one instance")
{
    // The default placement policy is convergence: with no per-instance cap, every connection
    // presenting one key lands in the single bucket the first join opened — byte-identical to the 1:1
    // get-or-create map. Three joiners, one factory-opened world.
    const auto hub = CreateRef<Hub>();
    const u32 serverEndpoint = hub->Register();
    auto serverT = CreateUnique<HubTransport>(hub, serverEndpoint, serverEndpoint);
    FactoryServer server(*serverT, /*maxHosted=*/64, /*maxPerConn=*/4, /*dwell=*/5.0,
                         /*maxPerInstance=*/0);

    const WorldKey shared = WorldKey::FromU64(0x5EED);

    vector<Unique<HubTransport>> transports;
    vector<Unique<ClientWorld>> clients;
    for (int i = 0; i < 3; ++i)
    {
        transports.push_back(CreateUnique<HubTransport>(hub, hub->Register(), serverEndpoint));
        clients.push_back(CreateUnique<ClientWorld>(*transports.back(), shared));
    }

    f64 now = 0.0;
    constexpr f32 Delta = 1.0f / 60.0f;
    for (u64 tick = 1; tick <= 160; ++tick)
    {
        now += Delta;
        server.Pump(now, tick);
        for (const Unique<ClientWorld>& client : clients)
        {
            client->Frame(now, tick, Delta, std::nullopt);
        }
    }

    std::unordered_map<u64, int> perWorld;
    for (const Unique<ClientWorld>& client : clients)
    {
        REQUIRE(client->Host->IsJoined());
        const WorldInstanceId world = server.Host->WorldFor(client->Client->AssignedId());
        REQUIRE(world.IsValid());
        perWorld[world.Value] += 1;
    }

    CHECK(perWorld.size() == 1);                 // one bucket
    CHECK(server.OpenCount == 1);                // opened exactly once
    CHECK(server.Host->HostedWorldCount() == 2); // primary + the one converged instance
}

TEST_CASE("MaxPlayersPerInstance buckets a key's joiners into capacity-bounded instances")
{
    // A per-instance cap of N buckets a busy key: the built-in policy places a joiner into the first
    // bucket under capacity and opens a fresh one when every bucket is full. 2N+1 joiners for one key
    // land in three buckets (N + N + 1), each its own instance (its own ReplicationServer).
    const auto hub = CreateRef<Hub>();
    const u32 serverEndpoint = hub->Register();
    auto serverT = CreateUnique<HubTransport>(hub, serverEndpoint, serverEndpoint);
    constexpr u32 Cap = 2;
    FactoryServer server(*serverT, /*maxHosted=*/64, /*maxPerConn=*/4, /*dwell=*/5.0,
                         /*maxPerInstance=*/Cap);

    const WorldKey busy = WorldKey::FromU64(0xB055);

    constexpr int Joiners = 2 * static_cast<int>(Cap) + 1; // five
    vector<Unique<HubTransport>> transports;
    vector<Unique<ClientWorld>> clients;
    for (int i = 0; i < Joiners; ++i)
    {
        transports.push_back(CreateUnique<HubTransport>(hub, hub->Register(), serverEndpoint));
        clients.push_back(CreateUnique<ClientWorld>(*transports.back(), busy));
    }

    f64 now = 0.0;
    constexpr f32 Delta = 1.0f / 60.0f;
    for (u64 tick = 1; tick <= 200; ++tick)
    {
        now += Delta;
        server.Pump(now, tick);
        for (const Unique<ClientWorld>& client : clients)
        {
            client->Frame(now, tick, Delta, std::nullopt);
        }
    }

    // Every joiner landed in some bucket, and the buckets partition the five seats three ways.
    std::unordered_map<u64, int> perWorld;
    for (const Unique<ClientWorld>& client : clients)
    {
        REQUIRE(client->Host->IsJoined());
        const WorldInstanceId world = server.Host->WorldFor(client->Client->AssignedId());
        REQUIRE(world.IsValid());
        perWorld[world.Value] += 1;
    }

    CHECK(perWorld.size() == 3);                 // three distinct buckets
    CHECK(server.OpenCount == 3);                // three factory opens
    CHECK(server.Host->HostedWorldCount() == 4); // primary + three buckets
    int total = 0;
    for (const auto& [world, count] : perWorld)
    {
        CHECK(count <= static_cast<int>(Cap)); // no bucket over capacity
        total += count;
    }
    CHECK(total == Joiners);
}

TEST_CASE("An emptied capacity bucket is reaped after the idle dwell; its peers stay")
{
    // Cap 1 forces one bucket per joiner; when a bucket's only seat leaves it idles out and is reaped
    // (dropping out of the key's list) while the other bucket, still joined, is untouched.
    const auto hub = CreateRef<Hub>();
    const u32 serverEndpoint = hub->Register();
    auto serverT = CreateUnique<HubTransport>(hub, serverEndpoint, serverEndpoint);
    constexpr f64 Dwell = 0.5;
    FactoryServer server(*serverT, /*maxHosted=*/64, /*maxPerConn=*/4, Dwell, /*maxPerInstance=*/1);

    const WorldKey busy = WorldKey::FromU64(0xF0FA);

    auto transportA = CreateUnique<HubTransport>(hub, hub->Register(), serverEndpoint);
    auto transportB = CreateUnique<HubTransport>(hub, hub->Register(), serverEndpoint);
    ClientWorld clientA(*transportA, busy);
    ClientWorld clientB(*transportB, busy);

    f64 now = 0.0;
    constexpr f32 Delta = 1.0f / 60.0f;
    for (u64 tick = 1; tick <= 120; ++tick)
    {
        now += Delta;
        server.Pump(now, tick);
        clientA.Frame(now, tick, Delta, std::nullopt);
        clientB.Frame(now, tick, Delta, std::nullopt);
    }

    REQUIRE(clientA.Host->IsJoined());
    REQUIRE(clientB.Host->IsJoined());
    const WorldInstanceId worldA = server.Host->WorldFor(clientA.Client->AssignedId());
    const WorldInstanceId worldB = server.Host->WorldFor(clientB.Client->AssignedId());
    REQUIRE(worldA != worldB); // the cap of 1 split them into two buckets
    CHECK(server.OpenCount == 2);
    CHECK(server.Host->HostedWorldCount() == 3); // primary + two buckets

    // A leaves; flush the graceful-close datagram (pump A so the leave is sent and the server processes
    // it), then drive past the dwell with only B (whose keepalive stays alive). A's bucket empties and
    // reaps; B's stays live.
    clientA.Client->Disconnect();
    for (u64 tick = 121; tick <= 140; ++tick)
    {
        now += Delta;
        server.Pump(now, tick);
        clientA.Frame(now, tick, Delta, std::nullopt);
        clientB.Frame(now, tick, Delta, std::nullopt);
    }
    for (u64 tick = 141; tick <= 220; ++tick)
    {
        now += Delta;
        server.Pump(now, tick);
        clientB.Frame(now, tick, Delta, std::nullopt);
    }

    REQUIRE(server.CloseCount == 1);
    CHECK(server.Closed.front() == worldA.Value); // the emptied bucket, not B's
    CHECK(server.Host->HostedWorldCount() == 2);  // primary + B's surviving bucket
    CHECK(clientB.Host->IsJoined());
    CHECK(server.Host->WorldFor(clientB.Client->AssignedId()) == worldB);
}

// ---- Runtime net activation (host stood up after the world is live) ------------------------------

TEST_CASE("A server host stood up after its world has ticked standalone still accepts a join")
{
    // The runtime-activation seam at the host layer: a world runs standalone (no transport bound),
    // then a ServerHost is constructed over that already-live scene and a client connects and joins it
    // by WorldKey — the "start hosting after launch" path, minus the process boundary.
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    Unique<Scene> world = Scene::Create(types);

    // Pre-activation: the world advances a few ticks with no host at all (standalone Server-tier).
    MovementSystem movement;
    const Entity mover = world->CreateEntity();
    world->Add<Transform>(mover);
    world->Add<Intent>(mover, Intent{.Move = vec3(1.0f, 0.0f, 0.0f)});
    world->Add<Mover>(mover);
    world->Add<Authority>(mover, Authority{.Tier = Tier::Server});
    FakeContext ctx;
    constexpr f32 Delta = 1.0f / 60.0f;
    for (u64 tick = 1; tick <= 30; ++tick)
    {
        world->SetChangeTick(tick);
        movement.OnUpdate(*world, Delta, ctx.Make());
    }
    const vec3 preHostPos = world->Get<Transform>(mover).Position;
    REQUIRE(preHostPos.x > 0.1f); // it moved before any host existed

    // Runtime activation: bind the transport and stand the host up over the live world.
    auto [serverT, clientT] = LoopbackTransport::CreatePair();
    Result<Unique<ServerHost>> host = ServerHost::Create(ServerHostInfo{
        .Server = ServerInfo{.TransportOverride = serverT.get(), .Connection = FastConfig},
        .WorldId = WorldInstanceId{.Value = 1},
        .World = *world,
        .Assets = FakeAssets(),
        .LevelId = LevelId,
        .Replication = ReplicationServer::Settings{.SnapshotInterval = 2},
        .Interest = InterestSettings{.Radius = 0.0f},
    });
    REQUIRE(host.has_value());

    ClientWorld client(*clientT);

    f64 now = 0.5;
    std::unordered_map<u64, InputJitterBuffer> jitter;
    for (u64 tick = 31; tick <= 90; ++tick)
    {
        now += Delta;
        world->SetChangeTick(tick);
        movement.OnUpdate(*world, Delta, ctx.Make());
        (*host)->Pump(now, tick);
        IngestConnectionInputs(**host, jitter, InputJitterBuffer::Settings{}, types);
        client.Frame(now, tick, Delta, std::nullopt);
    }

    // The client connected to the after-the-fact host and joined the world by DefaultWorldKey.
    CHECK(client.Client->State() == ClientState::Connected);
    CHECK(client.Host->IsJoined());
    CHECK((*host)->WorldFor(client.Client->AssignedId()).IsValid());
}

// ---- The travel payload and the world directory ---------------------------------------------------

namespace
{
    // Packs/unpacks a 1D position into a payload's opaque bytes — the game's own encoding, which the
    // engine never interprets. Used by the proximity-match placement policy and the payload cases.
    TravelPayload PosPayload(f32 x)
    {
        TravelPayload payload;
        payload.Type = 0x51; // an arbitrary game type tag
        payload.Bytes.resize(sizeof(f32));
        std::memcpy(payload.Bytes.data(), &x, sizeof(f32));
        return payload;
    }

    f32 PosOf(const TravelPayload& payload)
    {
        f32 x = 0.0f;
        if (payload.Bytes.size() >= sizeof(f32))
        {
            std::memcpy(&x, payload.Bytes.data(), sizeof(f32));
        }
        return x;
    }

    // A minimal client that joins keys with an explicit payload and tracks each join's scene, the
    // payload the reply echoed, and any make-before-break leave or travel denial the host surfaced.
    struct PayloadClient
    {
        TypeRegistry Types;
        Unique<Net::Client> Client;
        Unique<ClientHost> Host;
        vector<Unique<Scene>> Pending; // scenes handed to the host, kept alive here
        vector<JoinId> Left;
        vector<std::pair<WorldKey, JoinDenyReason>> Denied;

        explicit PayloadClient(Transport& transport)
        {
            RegisterBuiltinTypes(Types);
            Client = *Net::Client::Connect(
                ClientInfo{.TransportOverride = &transport, .Connection = FastConfig});
            Host = ClientHost::Create(ClientHostInfo{
                .Client = *Client,
                .Assets = FakeAssets(),
                .AutoJoin = false,
                .LoadLevel = [this](AssetId) -> Scene*
                {
                    // Install under the JoinId the host is about to assign: joins are answered in
                    // request order, so the next unfilled JoinId is this reply's. Simplest device-free
                    // stand-in: a fresh scene the host borrows.
                    Unique<Scene> scene = Scene::Create(Types);
                    Scene* raw = scene.get();
                    Pending.push_back(std::move(scene));
                    return raw;
                },
                .ResolvePrefab = [](AssetId) -> Ref<Prefab> { return nullptr; },
                .OnLeaveWorld = [this](const JoinId join) { Left.push_back(join); },
                .OnTravelDenied = [this](const WorldKey& key, const JoinDenyReason reason)
                { Denied.emplace_back(key, reason); },
            });
        }

        void Pump(f64 now) { Host->Pump(now); }
    };
}

TEST_CASE("The travel payload reaches Authorize, Placement, and WorldFactory, and is echoed to the "
          "client")
{
    auto [serverT, clientT] = LoopbackTransport::CreatePair();

    TypeRegistry serverTypes;
    RegisterBuiltinTypes(serverTypes);
    Unique<Scene> primary = Scene::Create(serverTypes);
    Unique<Scene> factoryScene = Scene::Create(serverTypes);

    const WorldKey key = WorldKey::FromU64(0xBEEF);
    const TravelPayload sent = PosPayload(3.5f);

    TravelPayload seenAuthorize;
    TravelPayload seenPlacement;
    TravelPayload seenFactory;
    bool authorizeSeen = false;
    bool placementSeen = false;
    bool factorySeen = false;

    Result<Unique<ServerHost>> hostR = ServerHost::Create(ServerHostInfo{
        .Server = ServerInfo{.TransportOverride = serverT.get(), .Connection = FastConfig},
        .WorldId = WorldInstanceId{.Value = 1},
        .Key = WorldKey::FromU64(0xFFFFFFFFULL), // the primary key, presented by no client
        .World = *primary,
        .Assets = FakeAssets(),
        .LevelId = LevelId,
        .Replication = ReplicationServer::Settings{.SnapshotInterval = 2},
        .Interest = InterestSettings{.Radius = 0.0f},
        .Authorize =
            [&](const JoinRequestInfo& request)
        {
            seenAuthorize = request.Payload;
            authorizeSeen = true;
            return true;
        },
        .WorldFactory = [&](const WorldKey&,
                            const TravelPayload& p) -> optional<ServerWorldResolution>
        {
            seenFactory = p;
            factorySeen = true;
            return ServerWorldResolution{.WorldId = WorldInstanceId{.Value = 100},
                                         .World = factoryScene.get(),
                                         .LevelId = LevelId,
                                         .Replication =
                                             ReplicationServer::Settings{.SnapshotInterval = 2},
                                         .Interest = InterestSettings{.Radius = 0.0f}};
        },
        .Placement = [&](const JoinRequestInfo& request,
                         std::span<const WorldPlacement>) -> optional<WorldInstanceId>
        {
            seenPlacement = request.Payload;
            placementSeen = true;
            return std::nullopt; // force the factory to open the world
        },
    });
    REQUIRE(hostR.has_value());
    Unique<ServerHost> host = std::move(*hostR);

    PayloadClient client(*clientT);

    f64 now = 0.0;
    constexpr f32 Delta = 1.0f / 60.0f;
    bool requested = false;
    for (u64 tick = 1; tick <= 40 && !client.Host->IsJoined(); ++tick)
    {
        now += Delta;
        primary->SetChangeTick(tick);
        factoryScene->SetChangeTick(tick);
        host->Pump(now, tick);
        client.Pump(now);
        if (!requested && client.Client->State() == ClientState::Connected)
        {
            client.Host->Join(key, sent);
            requested = true;
        }
    }

    REQUIRE(client.Host->IsJoined());
    CHECK(authorizeSeen);
    CHECK(seenAuthorize == sent);
    CHECK(placementSeen);
    CHECK(seenPlacement == sent);
    CHECK(factorySeen);
    CHECK(seenFactory == sent);

    // The reply echoed the bucket's recorded payload (here the request's), so the client's factory-
    // parameterized reconstruction has its inputs.
    CHECK(client.Host->JoinPayload(client.Host->CurrentJoinId()) == sent);
}

TEST_CASE("The client world digest folds the echoed travel payload: a matching fold joins, a "
          "diverged one is rejected loudly")
{
    // A world parameterized by payload rather than key attests the payload too: the server's factory
    // folds the opening payload into the echoed digest, and the client's WorldDigest hook receives
    // the reply's echoed payload to fold the same way — so both peers attest the same generation
    // inputs, key *and* payload.
    const auto fold = [](const TravelPayload& p)
    { return ContentDigest{.Lo = 0xF01DULL ^ static_cast<u64>(PosOf(p) * 1000.0f), .Hi = 0x9}; };

    auto [serverT, clientT] = LoopbackTransport::CreatePair();

    TypeRegistry serverTypes;
    RegisterBuiltinTypes(serverTypes);
    Unique<Scene> primary = Scene::Create(serverTypes);
    Unique<Scene> factoryScene = Scene::Create(serverTypes);

    const WorldKey key = WorldKey::FromU64(0xF00D);
    const TravelPayload sent = PosPayload(3.5f);

    Result<Unique<ServerHost>> hostR = ServerHost::Create(ServerHostInfo{
        .Server = ServerInfo{.TransportOverride = serverT.get(), .Connection = FastConfig},
        .WorldId = WorldInstanceId{.Value = 1},
        .Key = WorldKey::FromU64(0xFFFFFFFFULL), // the primary key, presented by no client
        .World = *primary,
        .Assets = FakeAssets(),
        .LevelId = LevelId,
        .Replication = ReplicationServer::Settings{.SnapshotInterval = 2},
        .WorldFactory = [&](const WorldKey&,
                            const TravelPayload& p) -> optional<ServerWorldResolution>
        {
            return ServerWorldResolution{.WorldId = WorldInstanceId{.Value = 100},
                                         .World = factoryScene.get(),
                                         .LevelId = LevelId,
                                         .Digest = fold(p),
                                         .Replication =
                                             ReplicationServer::Settings{.SnapshotInterval = 2}};
        },
    });
    REQUIRE(hostR.has_value());
    Unique<ServerHost> host = std::move(*hostR);

    Unique<Net::Client> client = *Net::Client::Connect(
        ClientInfo{.TransportOverride = clientT.get(), .Connection = FastConfig});

    TypeRegistry clientTypes;
    RegisterBuiltinTypes(clientTypes);
    Unique<Scene> clientScene;
    bool loaded = false;
    TravelPayload seenByDigest;
    bool matching = false;

    SUBCASE("a digest folding the echoed payload matches the server's fold and joins")
    {
        matching = true;
    }
    SUBCASE("a digest folding diverged payload data mismatches and the join is rejected") {}

    Unique<ClientHost> clientHost = ClientHost::Create(ClientHostInfo{
        .Client = *client,
        .Assets = FakeAssets(),
        .AutoJoin = false,
        // The matching client folds the echoed payload itself; the diverged one folds a different
        // parameter value (a client whose reconstruction inputs disagree with the bucket's).
        .WorldDigest = [&](const WorldKey&, const TravelPayload& p) -> ContentDigest
        {
            seenByDigest = p;
            return matching ? fold(p) : fold(PosPayload(7.25f));
        },
        .LoadLevel = [&](AssetId) -> Scene*
        {
            loaded = true;
            clientScene = Scene::Create(clientTypes);
            return clientScene.get();
        },
        .ResolvePrefab = [](AssetId) -> Ref<Prefab> { return nullptr; },
    });

    f64 now = 0.0;
    constexpr f32 Delta = 1.0f / 60.0f;
    bool requested = false;
    for (u64 tick = 1; tick <= 40 && !clientHost->IsJoined(); ++tick)
    {
        now += Delta;
        primary->SetChangeTick(tick);
        factoryScene->SetChangeTick(tick);
        host->Pump(now, tick);
        clientHost->Pump(now);
        if (!requested && client->State() == ClientState::Connected)
        {
            clientHost->Join(key, sent);
            requested = true;
        }
    }

    REQUIRE(client->State() == ClientState::Connected);
    // Either way the hook received the reply's echoed payload to fold from.
    CHECK(seenByDigest == sent);
    if (matching)
    {
        CHECK(loaded);
        CHECK(clientHost->IsJoined());
    }
    else
    {
        // The payload-folded digests disagree, so the join is refused before any level load or
        // stream apply — exactly the key-mismatch fail-loud path, now payload-sensitive.
        CHECK_FALSE(loaded);
        CHECK_FALSE(clientHost->IsJoined());
        CHECK(clientHost->Joins().empty());
    }
}

TEST_CASE("A payload-bucketing placement policy converges near params and splits far ones")
{
    // The proximity-match enabler: the placement policy reads each requester's payload and each live
    // bucket's recorded payload, converging requests whose positions are close and opening a fresh
    // bucket for a far one — matching quality expressed over data no WorldKey encodes.
    const auto hub = CreateRef<Hub>();
    const u32 serverEndpoint = hub->Register();
    auto serverT = CreateUnique<HubTransport>(hub, serverEndpoint, serverEndpoint);

    TypeRegistry serverTypes;
    RegisterBuiltinTypes(serverTypes);
    Unique<Scene> primary = Scene::Create(serverTypes);
    std::unordered_map<u64, Unique<Scene>> factoryScenes;
    u64 nextWorld = 100;
    u64 nextLevel = 0x3000;
    u32 openCount = 0;
    constexpr f32 Radius = 1.0f;

    Result<Unique<ServerHost>> hostR = ServerHost::Create(ServerHostInfo{
        .Server = ServerInfo{.TransportOverride = serverT.get(), .Connection = FastConfig},
        .WorldId = WorldInstanceId{.Value = 1},
        .Key = WorldKey::FromU64(0xFFFFFFFFULL),
        .World = *primary,
        .Assets = FakeAssets(),
        .LevelId = LevelId,
        .Replication = ReplicationServer::Settings{.SnapshotInterval = 2},
        .Interest = InterestSettings{.Radius = 0.0f},
        .WorldFactory = [&](const WorldKey&,
                            const TravelPayload&) -> optional<ServerWorldResolution>
        {
            const u64 world = nextWorld++;
            Unique<Scene>& scene = factoryScenes[world];
            scene = Scene::Create(serverTypes);
            ++openCount;
            return ServerWorldResolution{.WorldId = WorldInstanceId{.Value = world},
                                         .World = scene.get(),
                                         .LevelId = AssetId{.Value = nextLevel++},
                                         .Replication =
                                             ReplicationServer::Settings{.SnapshotInterval = 2},
                                         .Interest = InterestSettings{.Radius = 0.0f}};
        },
        .Placement = [&](const JoinRequestInfo& request,
                         std::span<const WorldPlacement> buckets) -> optional<WorldInstanceId>
        {
            const f32 want = PosOf(request.Payload);
            for (const WorldPlacement& bucket : buckets)
            {
                if (std::abs(PosOf(bucket.Payload) - want) <= Radius)
                {
                    return bucket.World; // converge on a near bucket
                }
            }
            return std::nullopt; // far from every bucket: open a fresh one
        },
    });
    REQUIRE(hostR.has_value());
    Unique<ServerHost> host = std::move(*hostR);

    const WorldKey key = WorldKey::FromU64(0x5EED);
    // Three requesters: near, near, far.
    const f32 positions[3] = {0.0f, 0.5f, 100.0f};
    vector<Unique<HubTransport>> transports;
    vector<Unique<PayloadClient>> clients;
    for (const f32 pos : positions)
    {
        transports.push_back(CreateUnique<HubTransport>(hub, hub->Register(), serverEndpoint));
        clients.push_back(CreateUnique<PayloadClient>(*transports.back()));
        (void)pos;
    }

    f64 now = 0.0;
    constexpr f32 Delta = 1.0f / 60.0f;
    std::array<bool, 3> requested{};
    for (u64 tick = 1; tick <= 200; ++tick)
    {
        now += Delta;
        primary->SetChangeTick(tick);
        for (auto& [value, scene] : factoryScenes)
        {
            scene->SetChangeTick(tick);
        }
        host->Pump(now, tick);
        for (usize i = 0; i < clients.size(); ++i)
        {
            clients[i]->Pump(now);
            if (!requested[i] && clients[i]->Client->State() == ClientState::Connected)
            {
                clients[i]->Host->Join(key, PosPayload(positions[i]));
                requested[i] = true;
            }
        }
    }

    for (const Unique<PayloadClient>& client : clients)
    {
        REQUIRE(client->Host->IsJoined());
    }

    const WorldInstanceId w0 = host->WorldFor(clients[0]->Client->AssignedId());
    const WorldInstanceId w1 = host->WorldFor(clients[1]->Client->AssignedId());
    const WorldInstanceId w2 = host->WorldFor(clients[2]->Client->AssignedId());

    // The two near requests converged on one bucket; the far one opened a second — two factory opens.
    CHECK(w0 == w1);
    CHECK(w2 != w0);
    CHECK(openCount == 2);
    CHECK(host->HostedWorldCount() == 3); // primary + two buckets
}

TEST_CASE(
    "A client travel request lands a directed travel make-before-break, and a denied one stays put")
{
    // Two pre-registered worlds; the client joins A, then travels to B. The server directs the travel
    // (join B, leave A once B is ready). The make-before-break guarantee: A stays joined until B lands.
    const WorldKey keyA = WorldKey::FromU64(0xA);
    const WorldKey keyB = WorldKey::FromU64(0xB);

    const auto build = [&](bool authorizeB, Unique<Scene>& sceneA, Unique<Scene>& sceneB,
                           TypeRegistry& types, Transport& transport) -> Unique<ServerHost>
    {
        RegisterBuiltinTypes(types);
        sceneA = Scene::Create(types);
        sceneB = Scene::Create(types);
        Result<Unique<ServerHost>> hostR = ServerHost::Create(ServerHostInfo{
            .Server = ServerInfo{.TransportOverride = &transport, .Connection = FastConfig},
            .WorldId = WorldInstanceId{.Value = 1},
            .Key = keyA,
            .World = *sceneA,
            .Assets = FakeAssets(),
            .LevelId = AssetId{0x00000000000000A1ULL},
            .Replication = ReplicationServer::Settings{.SnapshotInterval = 2},
            .Interest = InterestSettings{.Radius = 0.0f},
            // Deny key B in the snap-back subcase; A is always allowed.
            .Authorize = [keyB, authorizeB](const JoinRequestInfo& request)
            { return authorizeB || !(request.Key == keyB); },
        });
        REQUIRE(hostR.has_value());
        Unique<ServerHost> host = std::move(*hostR);
        host->AddWorld(
            ServerWorldInfo{.WorldId = WorldInstanceId{.Value = 2},
                            .Key = keyB,
                            .World = *sceneB,
                            .LevelId = AssetId{0x00000000000000B2ULL},
                            .Replication = ReplicationServer::Settings{.SnapshotInterval = 2},
                            .Interest = InterestSettings{.Radius = 0.0f}});
        return host;
    };

    SUBCASE("make-before-break: A stays until B is ready, then A is left")
    {
        auto [serverT, clientT] = LoopbackTransport::CreatePair();
        TypeRegistry serverTypes;
        Unique<Scene> sceneA;
        Unique<Scene> sceneB;
        Unique<ServerHost> host = build(true, sceneA, sceneB, serverTypes, *serverT);

        PayloadClient client(*clientT);

        f64 now = 0.0;
        constexpr f32 Delta = 1.0f / 60.0f;
        bool joinedA = false;
        bool travelled = false;
        bool joinedAOnce = false;
        bool sawGap =
            false; // a frame with no join at all would break the make-before-break guarantee
        JoinId joinA = ControlJoinId;
        for (u64 tick = 1; tick <= 120; ++tick)
        {
            now += Delta;
            sceneA->SetChangeTick(tick);
            sceneB->SetChangeTick(tick);
            host->Pump(now, tick);
            client.Pump(now);
            if (!joinedA && client.Client->State() == ClientState::Connected)
            {
                client.Host->Join(keyA);
                joinedA = true;
            }
            if (client.Host->IsJoined())
            {
                joinedAOnce = true;
            }
            if (joinedA && !travelled && client.Host->IsJoined())
            {
                joinA = client.Host->CurrentJoinId();
                client.Host->Travel(keyB);
                travelled = true;
            }
            // Make-before-break: once A landed, the client is never join-less — A stays until B is ready,
            // and B is installed in the same step A is left (never a gap where the client left nothing).
            if (joinedAOnce && client.Host->Joins().empty())
            {
                sawGap = true;
            }
        }

        // A was never dropped before B was ready (no gap), then A was left and only B remains.
        CHECK_FALSE(sawGap);
        REQUIRE(client.Host->Joins().size() == 1);
        CHECK(client.Host->Joins().front() != joinA);
        REQUIRE(client.Left.size() == 1);
        CHECK(client.Left.front() == joinA);
        CHECK(client.Denied.empty());
    }

    SUBCASE("snap-back: a denied destination never leaves the old join, and surfaces the reason")
    {
        auto [serverT, clientT] = LoopbackTransport::CreatePair();
        TypeRegistry serverTypes;
        Unique<Scene> sceneA;
        Unique<Scene> sceneB;
        Unique<ServerHost> host = build(false, sceneA, sceneB, serverTypes, *serverT);

        PayloadClient client(*clientT);

        f64 now = 0.0;
        constexpr f32 Delta = 1.0f / 60.0f;
        bool joinedA = false;
        bool travelled = false;
        JoinId joinA = ControlJoinId;
        for (u64 tick = 1; tick <= 120; ++tick)
        {
            now += Delta;
            sceneA->SetChangeTick(tick);
            sceneB->SetChangeTick(tick);
            host->Pump(now, tick);
            client.Pump(now);
            if (!joinedA && client.Client->State() == ClientState::Connected)
            {
                client.Host->Join(keyA);
                joinedA = true;
            }
            if (joinedA && !travelled && client.Host->IsJoined())
            {
                joinA = client.Host->CurrentJoinId();
                client.Host->Travel(keyB);
                travelled = true;
            }
        }

        // The denied travel never left A (the snap-back is "never left"), and the reason surfaced.
        REQUIRE(client.Host->Joins().size() == 1);
        CHECK(client.Host->Joins().front() == joinA);
        CHECK(client.Left.empty());
        REQUIRE(client.Denied.size() == 1);
        CHECK(client.Denied.front().first == keyB);
        CHECK(client.Denied.front().second == JoinDenyReason::NotAuthorized);
    }
}

TEST_CASE("The world directory reaps after the dwell, reuses warm, and never reaps a pinned world")
{
    // The standalone directory, device-free: no transport, no ServerHost — just Resolve/Pin/Unpin/reap
    // over a real (device-free) WorldRunner. Proves the lifetime policy every role shares.
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;
    WorldRunner runner(WorldRunnerInfo{.Types = &types, .Systems = &systems});

    u32 openCount = 0;
    u64 nextLevel = 0x4000;
    int closeHookCalls = 0;
    bool worldOpenAtHook = false; // the hook must precede the runner teardown

    Unique<WorldDirectory> dir = WorldDirectory::Create(WorldDirectoryInfo{
        .IdleKeepWarmDwell = 0.5,
        .Runner = &runner,
        .WorldFactory = [&](const WorldKey&,
                            const TravelPayload&) -> optional<ServerWorldResolution>
        {
            const WorldInstanceId world =
                runner.OpenWorld(WorldOpenInfo{.SimTickRate = 60, .StartSimulation = false});
            ++openCount;
            return ServerWorldResolution{.WorldId = world,
                                         .World = &runner.ResolveWorld(world)->GetScene(),
                                         .LevelId = AssetId{.Value = nextLevel++}};
        },
        .CloseWorld =
            [&](WorldInstanceId id)
        {
            ++closeHookCalls;
            // Hook-before-teardown: the world is still resolvable when the capture hook runs.
            worldOpenAtHook = runner.ResolveWorld(id) != nullptr;
        },
    });

    const WorldKey key = WorldKey::FromU64(0xA11CE);
    const TravelPayload noPayload;

    // Travel: resolve opens the world, then present pins it.
    const WorldResolveResult first = dir->Resolve(
        JoinRequestInfo{.Connection = ConnectionId{}, .Key = key, .Payload = noPayload},
        /*heldWorlds=*/0);
    REQUIRE(first.Outcome == WorldResolveOutcome::Opened);
    const WorldInstanceId world = first.World;
    CHECK(openCount == 1);
    dir->Pin(world); // presented

    // A pinned (presented) world never reaps, however far past the dwell.
    CHECK(dir->ReapIdle(1000.0).empty());
    CHECK(runner.ResolveWorld(world) != nullptr);

    // Unpin starts the dwell; within it the world stays warm and a re-travel reuses the warm instance
    // with no fresh factory call.
    dir->Unpin(world, 10.0);
    CHECK(dir->ReapIdle(10.2).empty()); // 0.2s < 0.5s dwell
    const WorldResolveResult reuse = dir->Resolve(
        JoinRequestInfo{.Connection = ConnectionId{}, .Key = key, .Payload = noPayload}, 0);
    CHECK(reuse.Outcome == WorldResolveOutcome::Placed);
    CHECK(reuse.World == world);
    CHECK(openCount == 1); // warm reuse: no second factory call
    dir->Pin(world);       // re-present, clearing the idle stamp
    CHECK(dir->ReapIdle(1000.0).empty());

    // Finally unpin and let the dwell elapse: the world is reaped, the capture hook running before the
    // runner teardown.
    dir->Unpin(world, 2000.0);
    const vector<WorldInstanceId> reaped = dir->ReapIdle(2001.0);
    REQUIRE(reaped.size() == 1);
    CHECK(reaped.front() == world);
    CHECK(closeHookCalls == 1);
    CHECK(worldOpenAtHook);                       // hook preceded teardown
    CHECK(runner.ResolveWorld(world) == nullptr); // the runner teardown followed
    CHECK(dir->WorldCount() == 0);
}

TEST_CASE("Two hosted worlds with different quantization envelopes each decode on one client")
{
    // The client-side per-key wire envelope: two worlds hosted with different spatial quantization
    // grids, both joined by one client whose WorldQuantization yields each key's matching grid — so
    // each world's remote pawn decodes correctly rather than one being misdecoded by a shared grid.
    auto [serverT, clientT] = LoopbackTransport::CreatePair();

    TypeRegistry serverTypes;
    RegisterBuiltinTypes(serverTypes);
    Unique<Scene> sceneA = Scene::Create(serverTypes);
    Unique<Scene> sceneB = Scene::Create(serverTypes);
    const WorldInstanceId worldA{.Value = 1};
    const WorldInstanceId worldB{.Value = 2};
    const WorldKey keyA = WorldKey::FromU64(0xA);
    const WorldKey keyB = WorldKey::FromU64(0xB);

    // Two envelopes: a tight metre-scale one and a wide system-scale one.
    const QuantizationSettings envA{.PositionQuantum = 0.001f, .PositionExtent = 16.0f};
    const QuantizationSettings envB{.PositionQuantum = 0.05f, .PositionExtent = 4096.0f};

    Result<Unique<ServerHost>> hostR = ServerHost::Create(ServerHostInfo{
        .Server = ServerInfo{.TransportOverride = serverT.get(), .Connection = FastConfig},
        .WorldId = worldA,
        .Key = keyA,
        .World = *sceneA,
        .Assets = FakeAssets(),
        .LevelId = AssetId{0x00000000000000A1ULL},
        .Replication = ReplicationServer::Settings{.SnapshotInterval = 2,
                                                   .QuantizeSpatial = true,
                                                   .Quantization = envA},
        .Interest = InterestSettings{.Radius = 0.0f},
    });
    REQUIRE(hostR.has_value());
    Unique<ServerHost> host = std::move(*hostR);
    host->AddWorld(
        ServerWorldInfo{.WorldId = worldB,
                        .Key = keyB,
                        .World = *sceneB,
                        .LevelId = AssetId{0x00000000000000B2ULL},
                        .Replication = ReplicationServer::Settings{.SnapshotInterval = 2,
                                                                   .QuantizeSpatial = true,
                                                                   .Quantization = envB},
                        .Interest = InterestSettings{.Radius = 0.0f}});

    // A client that decodes each key with its matching envelope through the per-key provider.
    TypeRegistry clientTypes;
    RegisterBuiltinTypes(clientTypes);
    Unique<Net::Client> client = *Net::Client::Connect(
        ClientInfo{.TransportOverride = clientT.get(), .Connection = FastConfig});
    RemoteInterpolationSystem interp;
    interp.SetSettings(RemoteInterpolationSystem::Settings{
        .SnapshotInterval = 2, .InterpolationDelayIntervals = 2, .SimTickRate = 60.0});
    std::unordered_map<u16, Unique<Scene>> scenes; // by JoinId
    std::deque<u16> loadOrder;                     // JoinId assignment is request order
    Unique<ClientHost> clientHost = ClientHost::Create(ClientHostInfo{
        .Client = *client,
        .Assets = FakeAssets(),
        .AutoJoin = false,
        .LoadLevel = [&](AssetId level) -> Scene*
        {
            Unique<Scene>& scene = scenes[level.Value & 0xFFFF];
            scene = Scene::Create(clientTypes);
            return scene.get();
        },
        .ResolvePrefab = [](AssetId) -> Ref<Prefab> { return nullptr; },
        .Prediction = [](const Scene&, Entity) { return vector<Entity>{}; },
        .WorldQuantization = [&](const WorldKey& key) -> QuantizationSettings
        { return key == keyA ? envA : envB; },
    });

    const std::unordered_map<u64, Scene*> serverScenes{{worldA.Value, sceneA.get()},
                                                       {worldB.Value, sceneB.get()}};
    // Distinct target positions within each envelope: A near the origin, B far out.
    const std::unordered_map<u64, f32> targetX{{worldA.Value, 4.0f}, {worldB.Value, 1000.0f}};
    std::unordered_map<u64, Entity> pawnByWorld;
    std::unordered_map<u64, InputJitterBuffer> jitter;

    f64 now = 0.0;
    constexpr f32 Delta = 1.0f / 60.0f;
    bool joined = false;
    for (u64 tick = 1; tick <= 260; ++tick)
    {
        now += Delta;
        for (const auto& [value, scene] : serverScenes)
        {
            scene->SetChangeTick(tick);
        }
        // Server frame: pawn each seat in its world and place the pawn at that world's target x.
        for (const ConnectionId id : host->Server().Connections())
        {
            for (const JoinId join : host->JoinsFor(id))
            {
                const WorldInstanceId world = host->WorldForJoin(id, join);
                Scene& scene = *serverScenes.at(world.Value);
                const Entity seat = host->SeatFor(id, join);
                if (seat.IsNull())
                {
                    continue;
                }
                auto& possesses = scene.Get<Possesses>(seat);
                if (possesses.Pawn.IsNull() || !scene.IsAlive(possesses.Pawn))
                {
                    const Entity pawn = scene.CreateEntity();
                    scene.Add<Transform>(pawn);
                    scene.Add<Authority>(pawn, Authority{.Tier = Tier::Server, .Owner = id});
                    possesses.Pawn = pawn;
                    pawnByWorld[world.Value] = pawn;
                }
                scene.Get<Transform>(possesses.Pawn).Position.x = targetX.at(world.Value);
            }
        }
        host->Pump(now, tick);
        IngestConnectionInputs(*host, jitter, InputJitterBuffer::Settings{}, serverTypes);

        clientHost->Pump(now);
        FakeContext ctx;
        ctx.Role = NetRole::Client;
        for (const JoinId join : clientHost->Joins())
        {
            if (Scene* world = clientHost->World(join))
            {
                interp.OnUpdate(*world, Delta, ctx.Make());
            }
        }
        if (!joined && client->State() == ClientState::Connected)
        {
            clientHost->Join(keyA);
            clientHost->Join(keyB);
            joined = true;
        }
    }

    REQUIRE(clientHost->Joins().size() == 2);
    const JoinId joinA = clientHost->Joins()[0];
    const JoinId joinB = clientHost->Joins()[1];
    const NetId netA = sceneA->Get<NetIdentity>(pawnByWorld.at(worldA.Value)).Id;
    const NetId netB = sceneB->Get<NetIdentity>(pawnByWorld.at(worldB.Value)).Id;
    const Entity clientPawnA = clientHost->Replication(joinA).Map().Lookup(netA);
    const Entity clientPawnB = clientHost->Replication(joinB).Map().Lookup(netB);
    REQUIRE_FALSE(clientPawnA.IsNull());
    REQUIRE_FALSE(clientPawnB.IsNull());

    // Each world's remote pawn decoded to its own target within its envelope's quantum — proof the
    // per-key grid was applied, not one shared grid that would mis-scale the far world.
    const f32 gotA = clientHost->World(joinA)->Get<Transform>(clientPawnA).Position.x;
    const f32 gotB = clientHost->World(joinB)->Get<Transform>(clientPawnB).Position.x;
    CHECK(std::abs(gotA - 4.0f) < 0.05f);
    CHECK(std::abs(gotB - 1000.0f) < 1.0f);
}

// ---------------------------------------------------------------------------------------------------
// Adopt-in-place joins, the scene-preserving leave, the make-before-break swap, and stable-anchor
// adoption — a client persistent scene the joins bind into (never a level load), with an anchored
// authoritative entity binding to its live local claimant. Device-free over the loopback.

namespace
{
    const WorldKey AdoptKeyA = WorldKey::FromU64(0x0ADA);
    const WorldKey AdoptKeyB = WorldKey::FromU64(0x0ADB);

    // A server hosting one or two worlds (each its own scene), with a net-unaware spawn rule pawning
    // each seat and an optional anchored authoritative entity carrying replicated Session state.
    struct AdoptServer
    {
        TypeRegistry Types;
        Unique<Scene> SceneA;
        Unique<Scene> SceneB;
        Unique<ServerHost> Host;
        std::unordered_map<u64, std::unordered_map<ConnectionId, Entity>> Pawns;
        vector<NetEvent> Events;
        vector<Entity> Anchored;

        explicit AdoptServer(Transport& transport, bool twoWorlds, bool denyB = false)
        {
            RegisterBuiltinTypes(Types);
            SceneA = Scene::Create(Types);
            Result<Unique<ServerHost>> host = ServerHost::Create(ServerHostInfo{
                .Server = ServerInfo{.TransportOverride = &transport, .Connection = FastConfig},
                .WorldId = WorldInstanceId{.Value = 1},
                .Key = AdoptKeyA,
                .World = *SceneA,
                .Assets = FakeAssets(),
                .LevelId = LevelId,
                .Replication = ReplicationServer::Settings{.SnapshotInterval = 2},
                .Interest = InterestSettings{.Radius = 0.0f},
                .Authorize = [denyB](const JoinRequestInfo& request)
                { return !denyB || !(request.Key == AdoptKeyB); },
            });
            REQUIRE(host.has_value());
            Host = std::move(*host);
            if (twoWorlds)
            {
                SceneB = Scene::Create(Types);
                Host->AddWorld(ServerWorldInfo{
                    .WorldId = WorldInstanceId{.Value = 2},
                    .Key = AdoptKeyB,
                    .World = *SceneB,
                    .LevelId = LevelId,
                    .Replication = ReplicationServer::Settings{.SnapshotInterval = 2},
                    .Interest = InterestSettings{.Radius = 0.0f}});
            }
        }

        // An anchored authoritative entity: a NetAnchor (bound across the wire) plus a replicated
        // Session the adoption applies onto the claimant. Seeded into world A's scene.
        Entity SeedAnchored(u64 lo, u64 hi, i32 score)
        {
            const Entity entity = SceneA->CreateEntity();
            SceneA->Add<NetAnchor>(entity, NetAnchor{.Lo = lo, .Hi = hi});
            SceneA->Add<Session>(entity, Session{.Phase = SessionPhase::Playing, .Score = score});
            SceneA->Add<Authority>(entity, Authority{.Tier = Tier::Server});
            Anchored.push_back(entity);
            return entity;
        }

        void SpawnFor(WorldInstanceId world, Scene& scene)
        {
            for (const ConnectionId id : Host->Server().Connections())
            {
                for (const JoinId join : Host->JoinsFor(id))
                {
                    if (Host->WorldForJoin(id, join) != world)
                    {
                        continue;
                    }
                    const Entity seat = Host->SeatFor(id, join);
                    if (seat.IsNull())
                    {
                        continue;
                    }
                    auto& possesses = scene.Get<Possesses>(seat);
                    if (!possesses.Pawn.IsNull() && scene.IsAlive(possesses.Pawn))
                    {
                        continue;
                    }
                    const Entity pawn = scene.CreateEntity();
                    scene.Add<Transform>(pawn);
                    scene.Add<Intent>(pawn);
                    scene.Add<Mover>(pawn);
                    scene.Add<Authority>(pawn, Authority{.Tier = Tier::Server, .Owner = id});
                    possesses.Pawn = pawn;
                    Pawns[world.Value][id] = pawn;
                }
            }
        }

        void Pump(f64 now, u64 tick)
        {
            SceneA->SetChangeTick(tick);
            if (SceneB)
            {
                SceneB->SetChangeTick(tick);
            }
            SpawnFor(WorldInstanceId{.Value = 1}, *SceneA);
            if (SceneB)
            {
                SpawnFor(WorldInstanceId{.Value = 2}, *SceneB);
            }
            // Re-stamp the anchored entities at this tick so their spawn record carries the state (a
            // component keeps the change tick of its last mutable touch — seeded ones would read tick 0).
            for (const Entity entity : Anchored)
            {
                if (SceneA->IsAlive(entity))
                {
                    (void)SceneA->Get<Session>(entity);
                }
            }
            Host->Pump(now, tick);
            for (const NetEvent& event : Host->Events())
            {
                Events.push_back(event);
            }
        }
    };

    // A client with one persistent scene every join adopts into (never a level load). Tracks the
    // possessions wired and the joins left.
    struct AdoptClient
    {
        TypeRegistry Types;
        Unique<Net::Client> Client;
        Unique<ClientHost> Host;
        Unique<Scene> World;
        vector<JoinId> Left;
        int Possessions = 0;
        Entity LastPossessed = Entity::Null;

        explicit AdoptClient(Transport& transport)
        {
            RegisterBuiltinTypes(Types);
            World = Scene::Create(Types);
            Client = *Net::Client::Connect(
                ClientInfo{.TransportOverride = &transport, .Connection = FastConfig});
            Host = ClientHost::Create(ClientHostInfo{
                .Client = *Client,
                .Assets = FakeAssets(),
                .AutoJoin = false,
                .LoadLevel = [](AssetId) -> Scene* { return nullptr; },
                .ResolvePrefab = [](AssetId) -> Ref<Prefab> { return nullptr; },
                .OnPossession =
                    [this](Scene&, const Entity pawn)
                {
                    ++Possessions;
                    LastPossessed = pawn;
                },
                .OnLeaveWorld = [this](const JoinId join) { Left.push_back(join); },
            });
        }

        // A derived Local prop the joins must never touch (the "byte-untouched" census target).
        Entity SeedDerived(const vec3 position)
        {
            const Entity entity = World->CreateEntity();
            World->Add<Transform>(entity, Transform{.Position = position});
            World->Add<Authority>(entity, Authority{.Tier = Tier::Local});
            return entity;
        }

        // A live local claimant carrying an anchor (a derived entity with server-authoritative state
        // to receive): a NetAnchor plus a derived Transform the adoption must keep.
        Entity SeedClaimant(u64 lo, u64 hi, const vec3 position)
        {
            const Entity entity = World->CreateEntity();
            World->Add<NetAnchor>(entity, NetAnchor{.Lo = lo, .Hi = hi});
            World->Add<Transform>(entity, Transform{.Position = position});
            World->Add<Authority>(entity, Authority{.Tier = Tier::Local});
            return entity;
        }

        void Pump(f64 now) { Host->Pump(now); }

        [[nodiscard]] usize CountAnchor(u64 lo, u64 hi) const
        {
            usize count = 0;
            for (auto [entity, anchor] : World->View<NetAnchor>())
            {
                if (anchor.Lo == lo && anchor.Hi == hi)
                {
                    ++count;
                }
            }
            return count;
        }

        [[nodiscard]] usize CountRemote() const
        {
            usize count = 0;
            for (auto [entity, authority] : World->View<Authority>())
            {
                if (authority.Tier == Tier::Remote)
                {
                    ++count;
                }
            }
            return count;
        }

        [[nodiscard]] usize CountPredicted() const
        {
            usize count = 0;
            for (auto [entity, authority] : World->View<Authority>())
            {
                if (authority.Tier == Tier::Predicted)
                {
                    ++count;
                }
            }
            return count;
        }
    };
}

TEST_CASE(
    "Adopt-in-place: a join binds into a live scene, streaming in without touching derived state")
{
    auto [serverT, clientT] = LoopbackTransport::CreatePair();
    AdoptServer server(*serverT, /*twoWorlds=*/false);
    AdoptClient client(*clientT);

    // A derived Local prop stands in the persistent scene before any join — its state must survive the
    // adopt untouched.
    const Entity prop = client.SeedDerived(vec3(3.0f, 4.0f, 5.0f));
    usize entitiesBefore = 0;
    for (auto [entity, authority] : client.World->View<Authority>())
    {
        (void)entity;
        (void)authority;
        ++entitiesBefore;
    }

    f64 now = 0.0;
    constexpr f32 Delta = 1.0f / 60.0f;
    bool joined = false;
    for (u64 tick = 1; tick <= 40; ++tick)
    {
        now += Delta;
        server.Pump(now, tick);
        client.Pump(now);
        if (!joined && client.Client->State() == ClientState::Connected)
        {
            client.Host->JoinInto(AdoptKeyA, *client.World);
            joined = true;
        }
    }

    REQUIRE(client.Host->IsJoined());
    // The join bound to the *same* persistent scene — no level was loaded.
    CHECK(client.Host->World() == client.World.get());
    // The derived prop is byte-untouched: same tier, same transform, still alive.
    REQUIRE(client.World->IsAlive(prop));
    CHECK(client.World->Get<Authority>(prop).Tier == Tier::Local);
    CHECK(client.World->Get<Transform>(prop).Position == vec3(3.0f, 4.0f, 5.0f));
    // The stream spawned wire-owned entities (the seat + pawn) into the live scene alongside the prop.
    usize entitiesAfter = 0;
    for (auto [entity, authority] : client.World->View<Authority>())
    {
        (void)entity;
        (void)authority;
        ++entitiesAfter;
    }
    CHECK(entitiesAfter > entitiesBefore);
    CHECK(client.CountRemote() >= 1);
}

TEST_CASE("Adopt-in-place: a digest mismatch refuses the join before any stream applies")
{
    auto [serverT, clientT] = LoopbackTransport::CreatePair();
    AdoptServer server(*serverT, /*twoWorlds=*/false);

    // A client whose reconstruction digest disagrees with the server's world digest (zero): the adopt
    // is rejected loudly, no join installed, and nothing streams into the scene.
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    Unique<Scene> scene = Scene::Create(types);
    Unique<Net::Client> conn = *Net::Client::Connect(
        ClientInfo{.TransportOverride = clientT.get(), .Connection = FastConfig});
    Unique<ClientHost> host = ClientHost::Create(ClientHostInfo{
        .Client = *conn,
        .Assets = FakeAssets(),
        .AutoJoin = false,
        .WorldDigest = [](const WorldKey&, const TravelPayload&)
        { return ContentDigest{.Lo = 0xDEAD, .Hi = 0}; },
        .LoadLevel = [](AssetId) -> Scene* { return nullptr; },
        .ResolvePrefab = [](AssetId) -> Ref<Prefab> { return nullptr; },
    });

    const Entity prop = scene->CreateEntity();
    scene->Add<Authority>(prop, Authority{.Tier = Tier::Local});

    f64 now = 0.0;
    constexpr f32 Delta = 1.0f / 60.0f;
    bool asked = false;
    for (u64 tick = 1; tick <= 40; ++tick)
    {
        now += Delta;
        server.Pump(now, tick);
        conn->Pump(now);
        host->Pump(now);
        if (!asked && conn->State() == ClientState::Connected)
        {
            host->JoinInto(AdoptKeyA, *scene);
            asked = true;
        }
    }

    CHECK_FALSE(host->IsJoined());
    // No wire-owned entity ever spawned: the scene holds only the derived prop.
    usize entities = 0;
    for (auto [entity, authority] : scene->View<Authority>())
    {
        (void)entity;
        (void)authority;
        ++entities;
    }
    CHECK(entities == 1);
}

TEST_CASE(
    "Leave removes exactly the wire-owned set, the scene surviving, and the server reaps the seat")
{
    auto [serverT, clientT] = LoopbackTransport::CreatePair();
    AdoptServer server(*serverT, /*twoWorlds=*/false);
    AdoptClient client(*clientT);
    const Entity prop = client.SeedDerived(vec3(1.0f, 0.0f, 0.0f));

    f64 now = 0.0;
    constexpr f32 Delta = 1.0f / 60.0f;
    bool joined = false;
    JoinId join = ControlJoinId;
    ConnectionId connId = ServerConnectionId;
    for (u64 tick = 1; tick <= 60; ++tick)
    {
        now += Delta;
        server.Pump(now, tick);
        client.Pump(now);
        if (!joined && client.Client->State() == ClientState::Connected)
        {
            client.Host->JoinInto(AdoptKeyA, *client.World);
            joined = true;
        }
    }
    REQUIRE(client.Host->IsJoined());
    join = client.Host->CurrentJoinId();
    connId = client.Client->AssignedId();
    REQUIRE(server.Host->SeatFor(connId, join).IsNull() == false);
    const usize remoteBefore = client.CountRemote();
    CHECK(remoteBefore >= 1);

    // Leave the join: its wire-owned entities go, the derived prop stays.
    client.Host->Leave(join);
    for (u64 tick = 61; tick <= 90; ++tick)
    {
        now += Delta;
        server.Pump(now, tick);
        client.Pump(now);
    }

    CHECK(client.Host->Joins().empty());
    CHECK(client.CountRemote() == 0);     // every wire-owned entity destroyed
    REQUIRE(client.World->IsAlive(prop)); // the scene survived
    CHECK(client.World->Get<Transform>(prop).Position == vec3(1.0f, 0.0f, 0.0f));

    // The server tore the seat down, dropped the join, and surfaced a WorldLeft event — the connection
    // staying live.
    CHECK(server.Host->SeatFor(connId, join).IsNull());
    CHECK(server.Host->JoinsFor(connId).empty());
    bool sawLeft = false;
    for (const NetEvent& event : server.Events)
    {
        if (event.Type == NetEventType::WorldLeft && event.Id == connId && event.Join == join)
        {
            sawLeft = true;
        }
    }
    CHECK(sawLeft);
    CHECK(server.Host->Server().Connections().size() == 1);
}

TEST_CASE(
    "The swap: adopt B while A stays live, hand possession over, then leave A — one predicted set")
{
    auto [serverT, clientT] = LoopbackTransport::CreatePair();
    AdoptServer server(*serverT, /*twoWorlds=*/true);
    AdoptClient client(*clientT);

    f64 now = 0.0;
    constexpr f32 Delta = 1.0f / 60.0f;
    bool joinedA = false;
    bool adoptedB = false;
    bool leftA = false;
    JoinId joinA = ControlJoinId;
    JoinId joinB = ControlJoinId;
    Entity pawnA = Entity::Null;
    bool sawGap = false;
    usize maxPredicted = 0;

    for (u64 tick = 1; tick <= 200; ++tick)
    {
        now += Delta;
        server.Pump(now, tick);
        client.Pump(now);

        if (!joinedA && client.Client->State() == ClientState::Connected)
        {
            client.Host->JoinInto(AdoptKeyA, *client.World);
            joinedA = true;
        }
        // Once A is joined and its pawn possessed, adopt B into the same scene (make-before-break).
        if (joinedA && !adoptedB && client.Host->IsJoined() && !client.LastPossessed.IsNull())
        {
            joinA = client.Host->CurrentJoinId();
            pawnA = client.LastPossessed;
            client.Host->JoinInto(AdoptKeyB, *client.World);
            adoptedB = true;
        }
        // Once B is joined *and* its own pawn possessed (a distinct pawn), leave A in the same frame —
        // the possession handoff and the demotion of A's predicted set land together.
        if (adoptedB && !leftA)
        {
            for (const JoinId j : client.Host->Joins())
            {
                if (j != joinA)
                {
                    joinB = j;
                }
            }
            const bool bReady = joinB != ControlJoinId && client.Host->IsJoined(joinB) &&
                                !client.LastPossessed.IsNull() && client.LastPossessed != pawnA;
            if (bReady)
            {
                client.Host->Leave(joinA);
                leftA = true;
            }
        }

        // The make-before-break guarantee: once A landed, the client is never join-less.
        if (joinedA && client.Host->IsJoined() && client.Host->Joins().empty())
        {
            sawGap = true;
        }
        // One predicted set across the boundary: at each tick's end at most one pawn is Predicted.
        maxPredicted = std::max(maxPredicted, client.CountPredicted());
    }

    CHECK_FALSE(sawGap);
    CHECK(leftA);
    REQUIRE(client.Host->Joins().size() == 1);
    CHECK(client.Host->Joins().front() == joinB);
    REQUIRE(client.Left.size() == 1);
    CHECK(client.Left.front() == joinA);
    // Never two predicted pawns at a tick boundary — the demotion rode the same pump as the promotion.
    CHECK(maxPredicted <= 1);
    // Possession handed over to B's pawn (a distinct entity from A's).
    CHECK(client.LastPossessed != pawnA);
    CHECK_FALSE(client.LastPossessed.IsNull());
}

TEST_CASE("The swap snaps back: a denied destination never leaves the live join")
{
    auto [serverT, clientT] = LoopbackTransport::CreatePair();
    AdoptServer server(*serverT, /*twoWorlds=*/true, /*denyB=*/true);
    AdoptClient client(*clientT);

    f64 now = 0.0;
    constexpr f32 Delta = 1.0f / 60.0f;
    bool joinedA = false;
    bool adoptedB = false;
    JoinId joinA = ControlJoinId;
    for (u64 tick = 1; tick <= 160; ++tick)
    {
        now += Delta;
        server.Pump(now, tick);
        client.Pump(now);
        if (!joinedA && client.Client->State() == ClientState::Connected)
        {
            client.Host->JoinInto(AdoptKeyA, *client.World);
            joinedA = true;
        }
        if (joinedA && !adoptedB && client.Host->IsJoined())
        {
            joinA = client.Host->CurrentJoinId();
            client.Host->JoinInto(AdoptKeyB, *client.World);
            adoptedB = true;
        }
        // The consumer only leaves A once B is ready; B is denied, so A is never left.
    }

    // A stays the sole join; nothing was left.
    REQUIRE(client.Host->Joins().size() == 1);
    CHECK(client.Host->Joins().front() == joinA);
    CHECK(client.Left.empty());
    CHECK(client.CountRemote() >= 1); // A's wire-owned set is bit-intact
}

TEST_CASE("Stable-anchor adoption: server state lands on the live claimant, released on leave")
{
    auto [serverT, clientT] = LoopbackTransport::CreatePair();
    AdoptServer server(*serverT, /*twoWorlds=*/false);
    server.SeedAnchored(0x7ULL, 0x0ULL, /*score=*/42);

    AdoptClient client(*clientT);
    const Entity claimant = client.SeedClaimant(0x7ULL, 0x0ULL, vec3(2.0f, 0.0f, 0.0f));

    f64 now = 0.0;
    constexpr f32 Delta = 1.0f / 60.0f;
    bool joined = false;
    JoinId join = ControlJoinId;
    for (u64 tick = 1; tick <= 60; ++tick)
    {
        now += Delta;
        server.Pump(now, tick);
        client.Pump(now);
        if (!joined && client.Client->State() == ClientState::Connected)
        {
            client.Host->JoinInto(AdoptKeyA, *client.World);
            joined = true;
        }
    }

    REQUIRE(client.Host->IsJoined());
    join = client.Host->CurrentJoinId();
    // The server state landed on the *claimant* — no duplicate anchored entity spawned.
    CHECK(client.CountAnchor(0x7ULL, 0x0ULL) == 1);
    REQUIRE(client.World->IsAlive(claimant));
    REQUIRE(client.World->Has<Session>(claimant));
    CHECK(client.World->Get<Session>(claimant).Score == 42);
    // The claimant stayed a derived Local entity; its pre-existing transform is untouched.
    CHECK(client.World->Get<Authority>(claimant).Tier == Tier::Local);
    CHECK(client.World->Get<Transform>(claimant).Position == vec3(2.0f, 0.0f, 0.0f));

    // Leave releases the binding: the stream-added Session goes, the claimant (and its pre-existing
    // components) survive, re-adoptable by the next join.
    client.Host->Leave(join);
    for (u64 tick = 61; tick <= 80; ++tick)
    {
        now += Delta;
        server.Pump(now, tick);
        client.Pump(now);
    }
    REQUIRE(client.World->IsAlive(claimant));
    CHECK_FALSE(client.World->Has<Session>(claimant)); // stream-added type removed
    CHECK(client.World->Has<NetAnchor>(claimant));     // pre-existing kept
    CHECK(client.World->Get<Transform>(claimant).Position == vec3(2.0f, 0.0f, 0.0f));
    CHECK(client.CountAnchor(0x7ULL, 0x0ULL) == 1); // anchor index entry stays
}

TEST_CASE("Stable-anchor adoption: a claimant-less anchored spawn falls back to a wire-owned spawn")
{
    auto [serverT, clientT] = LoopbackTransport::CreatePair();
    AdoptServer server(*serverT, /*twoWorlds=*/false);
    server.SeedAnchored(0x9ULL, 0x0ULL, /*score=*/7);

    // The client scene carries no claimant for anchor 9: the anchored spawn falls back to an ordinary
    // wire-owned entity (a Remote mirror carrying the state) instead of adopting.
    AdoptClient client(*clientT);

    f64 now = 0.0;
    constexpr f32 Delta = 1.0f / 60.0f;
    bool joined = false;
    for (u64 tick = 1; tick <= 60; ++tick)
    {
        now += Delta;
        server.Pump(now, tick);
        client.Pump(now);
        if (!joined && client.Client->State() == ClientState::Connected)
        {
            client.Host->JoinInto(AdoptKeyA, *client.World);
            joined = true;
        }
    }

    REQUIRE(client.Host->IsJoined());
    // The fallback spawned a wire-owned Remote entity carrying the anchored state; the fallback does
    // not stamp NetAnchor, so it never pollutes the claimant index.
    CHECK(client.CountAnchor(0x9ULL, 0x0ULL) == 0);
    bool sawState = false;
    for (auto [entity, session] : client.World->View<Session>())
    {
        if (session.Score == 7 && client.World->Get<Authority>(entity).Tier == Tier::Remote)
        {
            sawState = true;
        }
    }
    CHECK(sawState);
}

// ---- Account identity ------------------------------------------------------------------------------

namespace
{
    // A minimal client presenting an explicit account id at the handshake. Joins are test-driven
    // (AutoJoin off); each reply loads a fresh borrowed scene.
    struct IdentityClient
    {
        TypeRegistry Types;
        Unique<Net::Client> Client;
        Unique<ClientHost> Host;
        vector<Unique<Scene>> Scenes; // scenes handed to the host, kept alive here

        IdentityClient(Transport& transport, const AccountId account)
        {
            RegisterBuiltinTypes(Types);
            Client = *Net::Client::Connect(ClientInfo{
                .Account = account, .TransportOverride = &transport, .Connection = FastConfig});
            Host = ClientHost::Create(ClientHostInfo{
                .Client = *Client,
                .Assets = FakeAssets(),
                .AutoJoin = false,
                .LoadLevel = [this](AssetId) -> Scene*
                {
                    Scenes.push_back(Scene::Create(Types));
                    return Scenes.back().get();
                },
                .ResolvePrefab = [](AssetId) -> Ref<Prefab> { return nullptr; },
            });
        }

        void Pump(const f64 now) { Host->Pump(now); }
    };
}

TEST_CASE("The admitted account reaches Authorize, the seat's SeatAccount, and the host accessors")
{
    auto [serverT, clientT] = LoopbackTransport::CreatePair();

    TypeRegistry serverTypes;
    RegisterBuiltinTypes(serverTypes);
    Unique<Scene> serverScene = Scene::Create(serverTypes);

    AccountId authorizedAccount;
    ConnectionId authorizedConnection = ServerConnectionId;
    Result<Unique<ServerHost>> hostR = ServerHost::Create(ServerHostInfo{
        .Server = ServerInfo{.TransportOverride = serverT.get(), .Connection = FastConfig},
        .World = *serverScene,
        .Assets = FakeAssets(),
        .LevelId = LevelId,
        .Authorize =
            [&](const JoinRequestInfo& request)
        {
            authorizedAccount = request.Account;
            authorizedConnection = request.Connection;
            return true;
        },
    });
    REQUIRE(hostR.has_value());
    Unique<ServerHost> host = std::move(*hostR);

    const AccountId account{.Lo = 0xABCD, .Hi = 0x1234};
    IdentityClient client(*clientT, account);

    f64 now = 0.0;
    constexpr f32 Delta = 1.0f / 60.0f;
    bool requested = false;
    for (u64 tick = 1; tick <= 40 && !client.Host->IsJoined(); ++tick)
    {
        now += Delta;
        serverScene->SetChangeTick(tick);
        host->Pump(now, tick);
        client.Pump(now);
        if (!requested && client.Client->State() == ClientState::Connected)
        {
            client.Host->Join(DefaultWorldKey);
            requested = true;
        }
    }
    REQUIRE(client.Host->IsJoined());

    // Authorize saw the admitted account beside the requesting connection.
    const ConnectionId id = client.Client->AssignedId();
    CHECK(authorizedAccount == account);
    CHECK(authorizedConnection == id);

    // The spawned seat is stamped with the account, server-local.
    const Entity seat = host->SeatFor(id);
    REQUIRE_FALSE(seat.IsNull());
    REQUIRE(serverScene->Has<SeatAccount>(seat));
    CHECK(serverScene->Get<SeatAccount>(seat).Account == account);

    // The host accessors resolve both directions.
    CHECK(host->AccountFor(id) == account);
    CHECK(host->ConnectionFor(account) == id);
    CHECK(host->ConnectionFor(AccountId{.Lo = 0xDEAD}) == ServerConnectionId);
}

TEST_CASE("AdmitAccount refuses or normalizes the presented account at the handshake")
{
    SUBCASE("nullopt refuses the connection with AccountRefused")
    {
        auto [serverT, clientT] = LoopbackTransport::CreatePair();

        TypeRegistry serverTypes;
        RegisterBuiltinTypes(serverTypes);
        Unique<Scene> serverScene = Scene::Create(serverTypes);

        Result<Unique<ServerHost>> hostR = ServerHost::Create(ServerHostInfo{
            .Server =
                ServerInfo{
                    .AdmitAccount = [](ConnectionId, const AccountId&) -> optional<AccountId>
                    { return std::nullopt; },
                    .TransportOverride = serverT.get(),
                    .Connection = FastConfig,
                },
            .World = *serverScene,
            .Assets = FakeAssets(),
            .LevelId = LevelId,
        });
        REQUIRE(hostR.has_value());
        Unique<ServerHost> host = std::move(*hostR);

        IdentityClient client(*clientT, AccountId{.Lo = 1});
        f64 now = 0.0;
        constexpr f32 Delta = 1.0f / 60.0f;
        for (u64 tick = 1; tick <= 20 && client.Client->State() == ClientState::Connecting; ++tick)
        {
            now += Delta;
            host->Pump(now, tick);
            client.Pump(now);
        }

        CHECK(client.Client->State() == ClientState::Denied);
        REQUIRE(client.Client->GetDenyReason().has_value());
        CHECK(*client.Client->GetDenyReason() == DenyReason::AccountRefused);
        CHECK(host->Server().Connections().empty());
    }

    SUBCASE("a normalized account is the one bound to the connection")
    {
        auto [serverT, clientT] = LoopbackTransport::CreatePair();

        TypeRegistry serverTypes;
        RegisterBuiltinTypes(serverTypes);
        Unique<Scene> serverScene = Scene::Create(serverTypes);

        const AccountId canonical{.Lo = 0xCA, .Hi = 0x0};
        Result<Unique<ServerHost>> hostR = ServerHost::Create(ServerHostInfo{
            .Server =
                ServerInfo{
                    .AdmitAccount = [canonical](ConnectionId, const AccountId&)
                        -> optional<AccountId> { return canonical; },
                    .TransportOverride = serverT.get(),
                    .Connection = FastConfig,
                },
            .World = *serverScene,
            .Assets = FakeAssets(),
            .LevelId = LevelId,
        });
        REQUIRE(hostR.has_value());
        Unique<ServerHost> host = std::move(*hostR);

        IdentityClient client(*clientT, AccountId{.Lo = 0xFEED});
        f64 now = 0.0;
        constexpr f32 Delta = 1.0f / 60.0f;
        bool requested = false;
        for (u64 tick = 1; tick <= 40 && !client.Host->IsJoined(); ++tick)
        {
            now += Delta;
            serverScene->SetChangeTick(tick);
            host->Pump(now, tick);
            client.Pump(now);
            if (!requested && client.Client->State() == ClientState::Connected)
            {
                client.Host->Join(DefaultWorldKey);
                requested = true;
            }
        }
        REQUIRE(client.Host->IsJoined());

        const ConnectionId id = client.Client->AssignedId();
        CHECK(host->AccountFor(id) == canonical);
        const Entity seat = host->SeatFor(id);
        REQUIRE_FALSE(seat.IsNull());
        CHECK(serverScene->Get<SeatAccount>(seat).Account == canonical);
    }
}

TEST_CASE("A duplicate live account is refused; the first connection is undisturbed")
{
    const auto hub = CreateRef<Hub>();
    const u32 serverEndpoint = hub->Register();
    auto serverT = CreateUnique<HubTransport>(hub, serverEndpoint, serverEndpoint);

    TypeRegistry serverTypes;
    RegisterBuiltinTypes(serverTypes);
    Unique<Scene> serverScene = Scene::Create(serverTypes);

    Result<Unique<ServerHost>> hostR = ServerHost::Create(ServerHostInfo{
        .Server = ServerInfo{.TransportOverride = serverT.get(), .Connection = FastConfig},
        .World = *serverScene,
        .Assets = FakeAssets(),
        .LevelId = LevelId,
    });
    REQUIRE(hostR.has_value());
    Unique<ServerHost> host = std::move(*hostR);

    const AccountId account{.Lo = 0x11, .Hi = 0x22};

    // The first presenter binds the account and joins.
    auto firstT = CreateUnique<HubTransport>(hub, hub->Register(), serverEndpoint);
    IdentityClient first(*firstT, account);
    f64 now = 0.0;
    constexpr f32 Delta = 1.0f / 60.0f;
    u64 tick = 0;
    bool requested = false;
    while (!first.Host->IsJoined() && tick < 40)
    {
        ++tick;
        now += Delta;
        serverScene->SetChangeTick(tick);
        host->Pump(now, tick);
        first.Pump(now);
        if (!requested && first.Client->State() == ClientState::Connected)
        {
            first.Host->Join(DefaultWorldKey);
            requested = true;
        }
    }
    REQUIRE(first.Host->IsJoined());
    const ConnectionId firstId = first.Client->AssignedId();

    // A second presenter of the same account is refused at the door with the retryable reason.
    auto duplicateT = CreateUnique<HubTransport>(hub, hub->Register(), serverEndpoint);
    IdentityClient duplicate(*duplicateT, account);
    while (duplicate.Client->State() == ClientState::Connecting && tick < 80)
    {
        ++tick;
        now += Delta;
        serverScene->SetChangeTick(tick);
        host->Pump(now, tick);
        first.Pump(now);
        duplicate.Pump(now);
    }
    CHECK(duplicate.Client->State() == ClientState::Denied);
    REQUIRE(duplicate.Client->GetDenyReason().has_value());
    CHECK(*duplicate.Client->GetDenyReason() == DenyReason::AccountAlreadyConnected);

    // The existing binding is undisturbed: the first connection stays live, joined, and bound.
    CHECK(first.Client->State() == ClientState::Connected);
    CHECK(first.Host->IsJoined());
    REQUIRE(host->Server().Connections().size() == 1);
    CHECK(host->ConnectionFor(account) == firstId);

    // A distinct account is admitted alongside — the refusal was per-account, not a lockout.
    auto otherT = CreateUnique<HubTransport>(hub, hub->Register(), serverEndpoint);
    IdentityClient other(*otherT, AccountId{.Lo = 0x33});
    while (other.Client->State() == ClientState::Connecting && tick < 120)
    {
        ++tick;
        now += Delta;
        serverScene->SetChangeTick(tick);
        host->Pump(now, tick);
        first.Pump(now);
        other.Pump(now);
    }
    CHECK(other.Client->State() == ClientState::Connected);
    CHECK(host->Server().Connections().size() == 2);
}

TEST_CASE("Reconnect after a disconnect re-admits the account onto a fresh connection")
{
    const auto hub = CreateRef<Hub>();
    const u32 serverEndpoint = hub->Register();
    auto serverT = CreateUnique<HubTransport>(hub, serverEndpoint, serverEndpoint);

    TypeRegistry serverTypes;
    RegisterBuiltinTypes(serverTypes);
    Unique<Scene> serverScene = Scene::Create(serverTypes);

    Result<Unique<ServerHost>> hostR = ServerHost::Create(ServerHostInfo{
        .Server = ServerInfo{.TransportOverride = serverT.get(), .Connection = FastConfig},
        .World = *serverScene,
        .Assets = FakeAssets(),
        .LevelId = LevelId,
    });
    REQUIRE(hostR.has_value());
    Unique<ServerHost> host = std::move(*hostR);

    const AccountId account{.Lo = 0xAA, .Hi = 0xBB};

    auto firstT = CreateUnique<HubTransport>(hub, hub->Register(), serverEndpoint);
    IdentityClient first(*firstT, account);
    f64 now = 0.0;
    constexpr f32 Delta = 1.0f / 60.0f;
    u64 tick = 0;
    bool requested = false;
    while (!first.Host->IsJoined() && tick < 40)
    {
        ++tick;
        now += Delta;
        serverScene->SetChangeTick(tick);
        host->Pump(now, tick);
        first.Pump(now);
        if (!requested && first.Client->State() == ClientState::Connected)
        {
            first.Host->Join(DefaultWorldKey);
            requested = true;
        }
    }
    REQUIRE(first.Host->IsJoined());
    const ConnectionId firstId = first.Client->AssignedId();
    CHECK(host->ConnectionFor(account) == firstId);

    // A graceful leave frees the binding immediately (no zombie window).
    first.Client->Disconnect();
    while (!host->Server().Connections().empty() && tick < 80)
    {
        ++tick;
        now += Delta;
        serverScene->SetChangeTick(tick);
        host->Pump(now, tick);
        first.Pump(now);
    }
    REQUIRE(host->Server().Connections().empty());
    CHECK(host->ConnectionFor(account) == ServerConnectionId);

    // The same account reconnects onto a fresh ConnectionId; the reverse lookup re-points.
    auto secondT = CreateUnique<HubTransport>(hub, hub->Register(), serverEndpoint);
    IdentityClient second(*secondT, account);
    bool rejoined = false;
    while (!second.Host->IsJoined() && tick < 140)
    {
        ++tick;
        now += Delta;
        serverScene->SetChangeTick(tick);
        host->Pump(now, tick);
        second.Pump(now);
        if (!rejoined && second.Client->State() == ClientState::Connected)
        {
            second.Host->Join(DefaultWorldKey);
            rejoined = true;
        }
    }
    REQUIRE(second.Host->IsJoined());
    const ConnectionId secondId = second.Client->AssignedId();
    CHECK(secondId != firstId);
    CHECK(host->AccountFor(secondId) == account);
    CHECK(host->ConnectionFor(account) == secondId);
}

TEST_CASE("A reconnect inside the zombie window is refused, then admitted once the timeout clears")
{
    const auto hub = CreateRef<Hub>();
    const u32 serverEndpoint = hub->Register();
    auto serverT = CreateUnique<HubTransport>(hub, serverEndpoint, serverEndpoint);

    TypeRegistry serverTypes;
    RegisterBuiltinTypes(serverTypes);
    Unique<Scene> serverScene = Scene::Create(serverTypes);

    Result<Unique<ServerHost>> hostR = ServerHost::Create(ServerHostInfo{
        .Server = ServerInfo{.TransportOverride = serverT.get(), .Connection = FastConfig},
        .World = *serverScene,
        .Assets = FakeAssets(),
        .LevelId = LevelId,
    });
    REQUIRE(hostR.has_value());
    Unique<ServerHost> host = std::move(*hostR);

    const AccountId account{.Lo = 0xC0FFEE};

    // Bind the account, then go silent: the client is never pumped again (a crash / silent drop),
    // so the server holds the stale binding live until its dead-connection timeout fires.
    auto firstT = CreateUnique<HubTransport>(hub, hub->Register(), serverEndpoint);
    IdentityClient first(*firstT, account);
    f64 now = 0.0;
    constexpr f32 Delta = 1.0f / 60.0f;
    u64 tick = 0;
    bool requested = false;
    while (!first.Host->IsJoined() && tick < 40)
    {
        ++tick;
        now += Delta;
        serverScene->SetChangeTick(tick);
        host->Pump(now, tick);
        first.Pump(now);
        if (!requested && first.Client->State() == ClientState::Connected)
        {
            first.Host->Join(DefaultWorldKey);
            requested = true;
        }
    }
    REQUIRE(first.Host->IsJoined());
    const f64 silentSince = now;

    // Inside the zombie window (well before FastConfig.TimeoutInterval) the reconnect is refused
    // with the documented-retryable reason; the stale binding still holds the account.
    auto retryT = CreateUnique<HubTransport>(hub, hub->Register(), serverEndpoint);
    IdentityClient retry(*retryT, account);
    while (retry.Client->State() == ClientState::Connecting && now < silentSince + 1.0)
    {
        ++tick;
        now += Delta;
        serverScene->SetChangeTick(tick);
        host->Pump(now, tick);
        retry.Pump(now);
    }
    CHECK(retry.Client->State() == ClientState::Denied);
    REQUIRE(retry.Client->GetDenyReason().has_value());
    CHECK(*retry.Client->GetDenyReason() == DenyReason::AccountAlreadyConnected);
    CHECK(host->Server().Connections().size() == 1);

    // Advance past the timeout: the server reaps the zombie and frees the binding.
    now = silentSince + FastConfig.TimeoutInterval + 1.0;
    ++tick;
    serverScene->SetChangeTick(tick);
    host->Pump(now, tick);
    REQUIRE(host->Server().Connections().empty());
    CHECK(host->ConnectionFor(account) == ServerConnectionId);

    // The retry-with-backoff succeeds once the window clears.
    auto secondT = CreateUnique<HubTransport>(hub, hub->Register(), serverEndpoint);
    IdentityClient second(*secondT, account);
    bool rejoined = false;
    for (u64 step = 0; step < 60 && !second.Host->IsJoined(); ++step)
    {
        ++tick;
        now += Delta;
        serverScene->SetChangeTick(tick);
        host->Pump(now, tick);
        second.Pump(now);
        if (!rejoined && second.Client->State() == ClientState::Connected)
        {
            second.Host->Join(DefaultWorldKey);
            rejoined = true;
        }
    }
    REQUIRE(second.Host->IsJoined());
    CHECK(host->ConnectionFor(account) == second.Client->AssignedId());
}

TEST_CASE(
    "MembersOf reports accounts across joined worlds, the local account beside connected ones")
{
    auto [serverT, clientT] = LoopbackTransport::CreatePair();

    TypeRegistry serverTypes;
    RegisterBuiltinTypes(serverTypes);
    Unique<Scene> sceneA = Scene::Create(serverTypes);
    Unique<Scene> sceneB = Scene::Create(serverTypes);
    const WorldInstanceId worldA{.Value = 1};
    const WorldInstanceId worldB{.Value = 2};
    const WorldKey keyA = WorldKey::FromU64(0xA);
    const WorldKey keyB = WorldKey::FromU64(0xB);

    // The host borrows the directory (the Application-shared path), so the test reads MembersOf on
    // the same instance the host reports joins into.
    Unique<WorldDirectory> directory = WorldDirectory::Create(WorldDirectoryInfo{});
    Result<Unique<ServerHost>> hostR = ServerHost::Create(ServerHostInfo{
        .Server = ServerInfo{.TransportOverride = serverT.get(), .Connection = FastConfig},
        .WorldId = worldA,
        .Key = keyA,
        .World = *sceneA,
        .Assets = FakeAssets(),
        .LevelId = LevelId,
        .Directory = directory.get(),
    });
    REQUIRE(hostR.has_value());
    Unique<ServerHost> host = std::move(*hostR);
    host->AddWorld(ServerWorldInfo{
        .WorldId = worldB,
        .Key = keyB,
        .World = *sceneB,
        .LevelId = AssetId{0x00000000000000B2ULL},
    });

    const AccountId remote{.Lo = 0x1111};
    IdentityClient client(*clientT, remote);

    f64 now = 0.0;
    constexpr f32 Delta = 1.0f / 60.0f;
    u64 tick = 0;
    bool requested = false;
    while ((!client.Host->IsJoined() || client.Host->Joins().size() < 2) && tick < 80)
    {
        ++tick;
        now += Delta;
        sceneA->SetChangeTick(tick);
        sceneB->SetChangeTick(tick);
        host->Pump(now, tick);
        client.Pump(now);
        if (!requested && client.Client->State() == ClientState::Connected)
        {
            client.Host->Join(keyA);
            client.Host->Join(keyB);
            requested = true;
        }
    }
    REQUIRE(client.Host->Joins().size() == 2);

    // The one account is a member of both joined worlds' keys.
    vector<AccountId> membersA = directory->MembersOf(keyA);
    vector<AccountId> membersB = directory->MembersOf(keyB);
    REQUIRE(membersA.size() == 1);
    CHECK(membersA.front() == remote);
    REQUIRE(membersB.size() == 1);
    CHECK(membersB.front() == remote);

    // The local (connection-less) account registers per its presence like any connection's: a
    // pinned presented world reports it beside the connected account.
    const AccountId local{.Lo = 0x2222};
    directory->Pin(worldA, local);
    membersA = directory->MembersOf(keyA);
    REQUIRE(membersA.size() == 2);
    CHECK(std::ranges::find(membersA, remote) != membersA.end());
    CHECK(std::ranges::find(membersA, local) != membersA.end());
    CHECK(directory->MembersOf(keyB).size() == 1);

    // Dropping each presence drops its membership: unpin removes the local account, and the
    // client's leave removes the connected one from the left world only.
    directory->Unpin(worldA, now, local);
    const JoinId joinA = client.Host->Joins().front();
    client.Host->Leave(joinA);
    for (u64 step = 0; step < 20; ++step)
    {
        ++tick;
        now += Delta;
        sceneA->SetChangeTick(tick);
        sceneB->SetChangeTick(tick);
        host->Pump(now, tick);
        client.Pump(now);
    }
    CHECK(directory->MembersOf(keyA).empty());
    REQUIRE(directory->MembersOf(keyB).size() == 1);
    CHECK(directory->MembersOf(keyB).front() == remote);
}

TEST_CASE("An unconfigured client account mints a valid, process-unique id")
{
    const auto hub = CreateRef<Hub>();
    const u32 serverEndpoint = hub->Register();
    auto serverT = CreateUnique<HubTransport>(hub, serverEndpoint, serverEndpoint);

    TypeRegistry serverTypes;
    RegisterBuiltinTypes(serverTypes);
    Unique<Scene> serverScene = Scene::Create(serverTypes);

    Result<Unique<ServerHost>> hostR = ServerHost::Create(ServerHostInfo{
        .Server = ServerInfo{.TransportOverride = serverT.get(), .Connection = FastConfig},
        .World = *serverScene,
        .Assets = FakeAssets(),
        .LevelId = LevelId,
    });
    REQUIRE(hostR.has_value());
    Unique<ServerHost> host = std::move(*hostR);

    // Neither client configures an account (the ephemeral zero-config posture): each presents a
    // minted valid id, and the two ids are distinct.
    auto aT = CreateUnique<HubTransport>(hub, hub->Register(), serverEndpoint);
    auto bT = CreateUnique<HubTransport>(hub, hub->Register(), serverEndpoint);
    IdentityClient a(*aT, AccountId{});
    IdentityClient b(*bT, AccountId{});

    f64 now = 0.0;
    constexpr f32 Delta = 1.0f / 60.0f;
    for (u64 tick = 1; tick <= 40; ++tick)
    {
        now += Delta;
        serverScene->SetChangeTick(tick);
        host->Pump(now, tick);
        a.Pump(now);
        b.Pump(now);
    }
    REQUIRE(a.Client->State() == ClientState::Connected);
    REQUIRE(b.Client->State() == ClientState::Connected);

    const AccountId accountA = host->AccountFor(a.Client->AssignedId());
    const AccountId accountB = host->AccountFor(b.Client->AssignedId());
    CHECK(accountA.IsValid());
    CHECK(accountB.IsValid());
    CHECK_FALSE(accountA == accountB);
    // The client knows the id it presented (the minted one), matching the server's binding.
    CHECK(a.Client->GetAccount() == accountA);
    CHECK(b.Client->GetAccount() == accountB);
}
