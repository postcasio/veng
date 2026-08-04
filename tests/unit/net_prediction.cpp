// Client-side prediction over the two-world loopback: the locally-controlled pawn is promoted to
// Tier::Predicted on possession and re-runs the real Sim systems (control + movement) each client
// tick, so it responds on the tick its input is sampled. These scenarios pin the promotion/demotion
// set math (default policy, a custom policy, depossession), the zero-tick local response (the pawn
// moves with no server round trip), and the interpolation skip (a predicted pawn is simulated,
// never buffered). Convergence under reconciliation — restore, replay, and byte-equal agreement with
// the server — is the net_reconciliation.cpp gate. Deterministic (fixed tick, injected time),
// two-world.

#include <doctest/doctest.h>

#include <glm/geometric.hpp>

#include <Veng/Asset/Prefab.h>
#include <Veng/Net/Client.h>
#include <Veng/Net/Host.h>
#include <Veng/Net/InputFeed.h>
#include <Veng/Net/LoopbackTransport.h>
#include <Veng/Net/Replication.h>
#include <Veng/Net/Server.h>
#include <Veng/Net/WorldEnvelope.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Movement.h>
#include <Veng/Scene/RemoteInterpolationSystem.h>
#include <Veng/Scene/Scene.h>

#include <unordered_map>
#include <utility>

using namespace Veng;
using namespace Veng::Net;

namespace
{
    // The one game action: a 2D move axis the control mapping turns into an Intent.
    constexpr ActionId MoveAction{0xA1};
    constexpr AssetId LevelId{0x00000000000000AAULL};
    // The pawn prefab id the server associates with each spawn so the client instantiates the full
    // pawn (Intent/Mover are not replicated — only a prefab spawn carries them to the client).
    constexpr AssetId PawnPrefabId{0x00000000000000BBULL};

    // A dependency-free prefab and the seat spawn never dereference the manager (the net_two_world
    // precedent), so a never-dereferenced reference is safe.
    AssetManager& FakeAssets()
    {
        alignas(16) static unsigned char bytes[64]{};
        return *reinterpret_cast<AssetManager*>(bytes);
    }

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
                .Audio = *reinterpret_cast<Audio::AudioEngine*>(TasksBytes),
                .Role = Role,
            };
        }
    };

    const ConnectionConfig FastConfig{
        .ResendInterval = 0.02, .KeepaliveInterval = 0.05, .TimeoutInterval = 5.0};

    // A netpawn prefab: a Server-tier (Transform, Intent, Mover) entity. Associated per spawn so the
    // client instantiates the whole pawn — the movement pipeline has its inputs on both ends, exactly
    // as a prefab-associated spawn gives a real game's client pawn.
    Ref<Prefab> MakePawnPrefab(const TypeRegistry& registry)
    {
        const auto record = [&](const TypeId id, const void* value)
        {
            vector<u8> out;
            WriteFields(out, value, registry.Info(id), registry);
            return out;
        };

        const Transform transform{};
        const Intent intent{};
        const Mover mover{.MoveSpeed = 4.0f, .TurnSpeed = 2.0f};
        const Authority authority{.Tier = Tier::Server};

        Prefab::PrefabEntity entity;
        entity.Components.push_back(Prefab::Component{
            .Type = TypeIdOf<Transform>(), .Record = record(TypeIdOf<Transform>(), &transform)});
        entity.Components.push_back(Prefab::Component{
            .Type = TypeIdOf<Intent>(), .Record = record(TypeIdOf<Intent>(), &intent)});
        entity.Components.push_back(Prefab::Component{.Type = TypeIdOf<Mover>(),
                                                      .Record = record(TypeIdOf<Mover>(), &mover)});
        entity.Components.push_back(Prefab::Component{
            .Type = TypeIdOf<Authority>(), .Record = record(TypeIdOf<Authority>(), &authority)});

        vector<Prefab::PrefabEntity> entities;
        entities.push_back(std::move(entity));
        return Prefab::Create(std::move(entities), {});
    }

    // The server half, minus the transport: a scene, a ServerHost, the per-connection jitter buffers,
    // and the game-mode spawn rule + control/movement sim — the ServerHost/InputFeed path Application
    // drives, stepped by hand. Each seat's pawn is spawned from the shared prefab and prefab-associated
    // so the client instantiates it.
    struct ServerWorld
    {
        TypeRegistry Types;
        Unique<Scene> World;
        Unique<ServerHost> Host;
        Ref<Prefab> PawnPrefab;
        std::unordered_map<u64, InputJitterBuffer> Jitter;
        std::unordered_map<ConnectionId, Entity> Pawns;
        MovementSystem Movement;
        // When false the spawn rule stops pawning unpawned seats — the depossession case holds a seat
        // pawnless rather than re-pawning it the next tick.
        bool SpawnEnabled = true;

        explicit ServerWorld(Transport& transport)
        {
            RegisterBuiltinTypes(Types);
            World = Scene::Create(Types);
            PawnPrefab = MakePawnPrefab(Types);
            Result<Unique<ServerHost>> host = ServerHost::Create(ServerHostInfo{
                .Server = ServerInfo{.TransportOverride = &transport, .Connection = FastConfig},
                .World = *World,
                .Assets = FakeAssets(),
                .LevelId = LevelId,
                .Replication = ReplicationServer::Settings{.SnapshotInterval = 2},
            });
            REQUIRE(host.has_value());
            Host = std::move(*host);
        }

        void RunSpawnRule()
        {
            if (!SpawnEnabled)
            {
                return;
            }
            for (const ConnectionId id : Host->Server().Connections())
            {
                const Entity seat = Host->SeatFor(id);
                if (seat.IsNull())
                {
                    continue;
                }
                auto& possesses = World->Get<Possesses>(seat);
                if (!possesses.Pawn.IsNull() && World->IsAlive(possesses.Pawn))
                {
                    continue;
                }
                const Prefab::SpawnResult spawned = PawnPrefab->SpawnInto(*World, FakeAssets());
                if (spawned.Roots.empty())
                {
                    continue;
                }
                const Entity pawn = spawned.Roots.front();
                World->Get<Authority>(pawn).Owner = id;
                possesses.Pawn = pawn;

                // Assign the pawn its wire id now and associate the prefab so the spawn replicates as
                // an instantiation (Intent/Mover included), not as per-component state.
                AssignServerNetIds(*World, Host->Allocator());
                const NetId netId = World->Get<NetIdentity>(pawn).Id;
                Host->Replication().SetEntityPrefab(netId, PawnPrefabId);
                Pawns[id] = pawn;
            }
        }

        void SimStep(u64 tick, f32 delta)
        {
            World->SetChangeTick(tick);
            RunSpawnRule();
            FeedSeatInputs(*Host, Jitter, WorldInstanceId{}, *World);

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

        void NetPump(f64 now, u64 tick)
        {
            Host->Pump(now, tick);
            IngestConnectionInputs(*Host, Jitter, InputJitterBuffer::Settings{}, Types);
        }

        [[nodiscard]] Entity PawnFor(ConnectionId id) const
        {
            const auto it = Pawns.find(id);
            return it != Pawns.end() ? it->second : Entity::Null;
        }
    };

    // The client half: a connection, a ClientHost, a Local-tier camera + input seat, and the
    // client-side predicted sim (control + movement over the Predicted set) plus the View-phase
    // interpolation. The pawn arrives as a Remote mirror, is promoted to Predicted on possession, and
    // is driven here each client tick.
    struct ClientWorld
    {
        TypeRegistry Types;
        Ref<Prefab> PawnPrefab;
        Unique<Net::Client> Client;
        Unique<ClientHost> Host;
        InputSendBuffer Send;
        MovementSystem Movement;
        RemoteInterpolationSystem Interp;
        Unique<Scene> ClientScene;
        Entity LocalSeat = Entity::Null;
        Entity LocalCamera = Entity::Null;
        Entity OwnPawn = Entity::Null;

        explicit ClientWorld(Transport& transport, const PredictionPolicy& policy = {})
        {
            RegisterBuiltinTypes(Types);
            PawnPrefab = MakePawnPrefab(Types);
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
                    ClientScene->Add<Possesses>(LocalSeat);
                    ClientScene->Add<Authority>(LocalSeat, Authority{.Tier = Tier::Local});
                    return ClientScene.get();
                },
                .ResolvePrefab = [this](AssetId id) -> Ref<Prefab>
                { return id == PawnPrefabId ? PawnPrefab : nullptr; },
                .OnPossession =
                    [this](Scene& world, const Entity pawn)
                {
                    OwnPawn = pawn;
                    // Point the local seat's Possesses at the predicted pawn so control drives it, and
                    // aim the follow camera (the consumer wiring hello-triangle performs).
                    if (!LocalSeat.IsNull() && world.IsAlive(LocalSeat) &&
                        world.Has<Possesses>(LocalSeat))
                    {
                        world.Get<Possesses>(LocalSeat).Pawn = pawn;
                    }
                    if (!LocalCamera.IsNull() && world.IsAlive(LocalCamera))
                    {
                        world.Get<CameraFollow>(LocalCamera).Target = pawn;
                    }
                },
                .Prediction = policy,
            });
        }

        // One client Sim tick: set the scripted input, derive Intent onto the predicted pawn, integrate
        // movement (Role::Client, so HasAuthority answers true for the Predicted set), record the tick,
        // and stamp the send window.
        void PredictStep(u64 clientTick, f32 delta, const optional<ActionState>& scripted)
        {
            Scene* world = Host->World();
            if (world == nullptr)
            {
                return;
            }

            if (scripted.has_value() && !LocalSeat.IsNull() && world->IsAlive(LocalSeat))
            {
                world->Get<PlayerInput>(LocalSeat).State = *scripted;
            }
            if (!LocalSeat.IsNull() && world->IsAlive(LocalSeat))
            {
                const Possesses& possesses = world->Get<Possesses>(LocalSeat);
                if (!possesses.Pawn.IsNull() && world->IsAlive(possesses.Pawn) &&
                    world->Has<Intent>(possesses.Pawn))
                {
                    world->Get<Intent>(possesses.Pawn) =
                        ControlMap(world->Get<PlayerInput>(LocalSeat));
                }
            }

            FakeContext ctx;
            ctx.Role = NetRole::Client;
            Movement.OnUpdate(*world, delta, ctx.Make());
            Host->RecordPrediction(clientTick);
            StampLocalSeatInput(Send, *world, clientTick);
        }

        // The full client frame: apply the stream + promote (Pump), predict (Sim), interpolate remotes
        // (View, which skips the predicted set), then send this tick's input window.
        void Frame(f64 now, u64 clientTick, f32 delta, const optional<ActionState>& scripted)
        {
            Host->Pump(now);
            PredictStep(clientTick, delta, scripted);

            if (Scene* world = Host->World())
            {
                FakeContext ctx;
                ctx.Role = NetRole::Client;
                Interp.OnUpdate(*world, delta, ctx.Make());
            }

            if (Client->State() == ClientState::Connected && Host->CurrentJoinId() != ControlJoinId)
            {
                (void)Client->Server().Send(
                    Channel::UnreliableSequenced,
                    EncodeWorldEnvelope(Host->CurrentJoinId(), Send.Encode(0, Types)));
            }
        }
    };

    // Runs a joined session for @p ticks, driving both worlds tick-by-tick with @p scripted input.
    void RunJoined(ServerWorld& server, ClientWorld& client, u64 ticks,
                   const optional<ActionState>& scripted, f64& now, ConnectionId& id)
    {
        constexpr f32 Delta = 1.0f / 60.0f;
        for (u64 tick = 1; tick <= ticks; ++tick)
        {
            now += Delta;
            server.SimStep(tick, Delta);
            server.NetPump(now, tick);
            client.Frame(now, tick, Delta, scripted);
            if (!server.Host->Server().Connections().empty())
            {
                id = server.Host->Server().Connections().front();
            }
        }
    }
}

TEST_CASE("Possession promotes the pawn to Predicted and tracks it in the history")
{
    auto [serverT, clientT] = LoopbackTransport::CreatePair();
    ServerWorld server(*serverT);
    ClientWorld client(*clientT);

    f64 now = 0.0;
    ConnectionId id = ServerConnectionId;
    RunJoined(server, client, 50, MoveState(vec2(0.0f, 0.0f)), now, id);

    REQUIRE(client.Host->IsJoined());
    REQUIRE_FALSE(client.OwnPawn.IsNull());

    Scene& world = *client.Host->World();
    const Entity pawn = client.OwnPawn;

    // The possessed pawn is the predicted set: exactly one tracked entity, promoted from Remote.
    CHECK(world.Get<Authority>(pawn).Tier == Tier::Predicted);
    const std::span<const Entity> tracked = client.Host->History().Tracked();
    REQUIRE(tracked.size() == 1);
    CHECK(tracked.front() == pawn);

    // The history recorded the predicted ticks (input + captured state).
    CHECK(client.Host->History().Size() > 0);
}

TEST_CASE("A narrowing prediction policy leaves the pawn a Remote mirror")
{
    auto [serverT, clientT] = LoopbackTransport::CreatePair();
    ServerWorld server(*serverT);
    // A policy that predicts nothing — the client displays its own pawn interpolated, unpredicted.
    ClientWorld client(*clientT, [](const Scene&, Entity) { return vector<Entity>{}; });

    f64 now = 0.0;
    ConnectionId id = ServerConnectionId;
    RunJoined(server, client, 50, MoveState(vec2(0.0f, 0.0f)), now, id);

    REQUIRE(client.Host->IsJoined());
    REQUIRE_FALSE(client.OwnPawn.IsNull());

    Scene& world = *client.Host->World();
    CHECK(client.Host->History().Tracked().empty());
    CHECK(world.Get<Authority>(client.OwnPawn).Tier == Tier::Remote);
}

TEST_CASE("The predicted pawn responds on the tick its input is sampled, with no server round trip")
{
    auto [serverT, clientT] = LoopbackTransport::CreatePair();
    ServerWorld server(*serverT);
    ClientWorld client(*clientT);

    // Join, spawn, possess, promote — no input yet, so the pawn rests at the origin.
    f64 now = 0.0;
    ConnectionId id = ServerConnectionId;
    RunJoined(server, client, 50, MoveState(vec2(0.0f, 0.0f)), now, id);

    REQUIRE(client.Host->IsJoined());
    REQUIRE_FALSE(client.OwnPawn.IsNull());
    const Entity serverPawn = server.PawnFor(id);
    REQUIRE_FALSE(serverPawn.IsNull());

    Scene& world = *client.Host->World();
    const Entity clientPawn = client.OwnPawn;
    const f32 clientBefore = world.Get<Transform>(clientPawn).Position.x;
    const f32 serverBefore = server.World->Get<Transform>(serverPawn).Position.x;

    // Drive +x on the client alone — the server never ticks, so no snapshot arrives to move or
    // correct the pawn. Pure local prediction: the pawn advances on the tick its input lands.
    const ActionState move = MoveState(vec2(1.0f, 0.0f));
    constexpr f32 Delta = 1.0f / 60.0f;
    for (u64 tick = 51; tick <= 60; ++tick)
    {
        now += Delta;
        client.PredictStep(tick, Delta, move);
        // Interpolation runs every client frame and must not touch the predicted pawn.
        FakeContext ctx;
        ctx.Role = NetRole::Client;
        client.Interp.OnUpdate(world, Delta, ctx.Make());
    }

    // The client pawn moved locally (zero-tick response), while the un-ticked server pawn did not —
    // the response owes nothing to the round trip.
    CHECK(world.Get<Transform>(clientPawn).Position.x > clientBefore + 0.2f);
    CHECK(server.World->Get<Transform>(serverPawn).Position.x == doctest::Approx(serverBefore));

    // The interpolation system left the predicted pose alone (it is simulated, not buffered).
    CHECK(world.Get<Authority>(clientPawn).Tier == Tier::Predicted);
}

TEST_CASE("Depossession demotes the predicted pawn back to a Remote mirror and untracks it")
{
    auto [serverT, clientT] = LoopbackTransport::CreatePair();
    ServerWorld server(*serverT);
    ClientWorld client(*clientT);

    f64 now = 0.0;
    ConnectionId id = ServerConnectionId;
    RunJoined(server, client, 50, MoveState(vec2(0.0f, 0.0f)), now, id);

    REQUIRE(client.Host->IsJoined());
    const Entity clientPawn = client.OwnPawn;
    REQUIRE_FALSE(clientPawn.IsNull());
    REQUIRE(client.Host->History().Tracked().size() == 1);

    // Depossess server-side: the seat releases its pawn. The change replicates, and the client's
    // WireSeat sees the possessed pawn go null → the predicted set is demoted and untracked.
    const Entity serverSeat = server.Host->SeatFor(id);
    REQUIRE_FALSE(serverSeat.IsNull());

    constexpr f32 Delta = 1.0f / 60.0f;
    for (u64 tick = 51; tick <= 90; ++tick)
    {
        now += Delta;
        // Stop pawning, then clear the seat's possession so it stays pawnless — the depossession the
        // client observes as its possessed pawn going null.
        if (tick == 51)
        {
            server.SpawnEnabled = false;
            server.Pawns.erase(id);
        }
        server.SimStep(tick, Delta);
        if (tick == 51)
        {
            server.World->Get<Possesses>(serverSeat).Pawn = Entity::Null;
        }
        server.NetPump(now, tick);
        client.Frame(now, tick, Delta, MoveState(vec2(0.0f, 0.0f)));
    }

    Scene& world = *client.Host->World();
    CHECK(client.Host->PossessedPawn().IsNull());
    CHECK(client.Host->History().Tracked().empty());
    // The formerly-predicted pawn, still present, is back to an interpolated Remote mirror.
    if (world.IsAlive(clientPawn))
    {
        CHECK(world.Get<Authority>(clientPawn).Tier == Tier::Remote);
    }
}
