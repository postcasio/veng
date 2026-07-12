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
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Movement.h>
#include <Veng/Scene/RemoteInterpolationSystem.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SystemRegistry.h>
#include <Veng/World.h>
#include <Veng/WorldRunner.h>

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
        std::unordered_map<ConnectionId, InputJitterBuffer> Jitter;
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
                FeedSeatInputs(*Host, Jitter, *World, tick);
            }
            else
            {
                FeedSeatInputs(*Host, Jitter, *World);
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

        explicit ClientWorld(Transport& transport)
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

            if (Client->State() == ClientState::Connected)
            {
                (void)Client->Server().Send(Channel::UnreliableSequenced, Send.Encode(0, Types));
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
    const InputJitterBuffer& buffer = server.Jitter.at(id);
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
    REQUIRE(server.Jitter.contains(id));

    // Over a steady window every scheduled consume finds its tick's input already buffered, so the
    // underrun (coast) count does not move — the input-timing win the ahead-of-server model buys.
    const u64 underrunBefore = server.Jitter.at(id).UnderrunCount();
    const u64 consumeBefore = server.Jitter.at(id).ConsumeCount();
    for (u64 tick = 61; tick <= 200; ++tick)
    {
        step(tick);
    }
    CHECK(server.Jitter.at(id).ConsumeCount() - consumeBefore == 140); // one per server tick
    CHECK(server.Jitter.at(id).UnderrunCount() - underrunBefore == 0); // none underran

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
