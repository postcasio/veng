// Client-side reconciliation: the compare/restore/replay/smooth that converges prediction to
// authoritative truth. The unit slices pin the field-aware compare (spatial leaves within epsilon,
// discrete state exact), the render-residual decay + snap threshold, and the Reconcile arms in
// isolation (match trims, mismatch restores + replays, history underflow hard-snaps). The two-world
// slices are the planset's convergence gate: with the server-scheduled ahead-of-client tick model
// and a seeded lossy/reordering/duplicating link as the adversity — the latency knob is Plan 05's
// consolidated job — predicted state converges byte-equal to the authoritative state after
// quiescence, and a forced server displacement corrects with one smoothed correction. Deterministic
// (fixed tick, injected time, seeded faults), device-free.

#include <doctest/doctest.h>

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>

#include <Veng/Asset/Prefab.h>
#include <Veng/Net/Client.h>
#include <Veng/Net/FaultInjectionTransport.h>
#include <Veng/Net/Host.h>
#include <Veng/Net/InputFeed.h>
#include <Veng/Net/LoopbackTransport.h>
#include <Veng/Net/Reconciliation.h>
#include <Veng/Net/Replication.h>
#include <Veng/Net/Server.h>
#include <Veng/Net/WorldEnvelope.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>

#include "support/TestComponents.h"
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
    constexpr ActionId MoveAction{0xA1};
    constexpr AssetId LevelId{0x00000000000000AAULL};
    constexpr AssetId PawnPrefabId{0x00000000000000BBULL};

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

    Intent ControlMap(const PlayerInput& input)
    {
        const vec2 move = input.GetValue(MoveAction);
        return Intent{.Move = vec3(move.x, 0.0f, move.y)};
    }

    vector<u8> SerializeTransform(const TypeRegistry& types, const Transform& transform)
    {
        vector<u8> out;
        WriteFields(out, &transform, types.Info(TypeIdOf<Transform>()), types);
        return out;
    }

    // ---- Unit: the field-aware compare -----------------------------------------------------------

    TEST_CASE("ValuesMatch: spatial leaves compare within epsilon, discrete state exactly")
    {
        TypeRegistry types;
        RegisterBuiltinTypes(types);
        types.Register<VengTest::TestScore>();
        const ReconcileTolerances tol; // Position 0.01, Rotation 1e-4

        const TypeInfo& transformInfo = types.Info(TypeIdOf<Transform>());
        Transform a{.Position = vec3(1.0f, 2.0f, 3.0f)};
        Transform b = a;
        CHECK(ValuesMatch(&a, &b, transformInfo, types, tol));

        // Sub-epsilon position drift is carried (matches); a gross displacement does not.
        b.Position.x += 0.005f;
        CHECK(ValuesMatch(&a, &b, transformInfo, types, tol));
        b.Position.x = a.Position.x + 0.5f;
        CHECK_FALSE(ValuesMatch(&a, &b, transformInfo, types, tol));

        // A tiny rotation difference is within the quaternion tolerance; a large one is not.
        Transform ra;
        Transform rb;
        rb.Rotation = glm::normalize(quat(1.0f, 0.0005f, 0.0f, 0.0f));
        CHECK(ValuesMatch(&ra, &rb, transformInfo, types, tol));
        rb.Rotation = glm::angleAxis(1.0f, vec3(0.0f, 1.0f, 0.0f));
        CHECK_FALSE(ValuesMatch(&ra, &rb, transformInfo, types, tol));

        // Discrete gameplay state must match exactly — no epsilon on a score.
        const TypeInfo& scoreInfo = types.Info(TypeIdOf<VengTest::TestScore>());
        VengTest::TestScore s1{.Value = 5};
        VengTest::TestScore s2 = s1;
        CHECK(ValuesMatch(&s1, &s2, scoreInfo, types, tol));
        s2.Value = 6;
        CHECK_FALSE(ValuesMatch(&s1, &s2, scoreInfo, types, tol));

        // A non-spatial float scalar is exact too, not epsilon (only Transform's spatial leaves
        // carry a tolerance).
        const TypeInfo& moverInfo = types.Info(TypeIdOf<Mover>());
        Mover m1{.MoveSpeed = 1.0f};
        Mover m2 = m1;
        CHECK(ValuesMatch(&m1, &m2, moverInfo, types, tol));
        m2.MoveSpeed = 1.0f + 1.0e-5f;
        CHECK_FALSE(ValuesMatch(&m1, &m2, moverInfo, types, tol));
    }

    // ---- Unit: the render-residual decay + snap threshold ----------------------------------------

    TEST_CASE("DecayPredictionError eases a residual monotonically to zero")
    {
        PredictionError error{.Position = vec3(1.0f, 0.0f, 0.0f),
                              .Rotation = glm::angleAxis(0.5f, vec3(0.0f, 1.0f, 0.0f))};
        f32 previous = glm::length(error.Position);
        for (int i = 0; i < 60; ++i)
        {
            error = DecayPredictionError(error, 1.0f / 60.0f, 20.0f);
            const f32 length = glm::length(error.Position);
            CHECK(length <= previous + 1.0e-6f); // never grows — no oscillation
            previous = length;
        }
        // ~1s at speed 20 has all but vanished.
        CHECK(glm::length(error.Position) < 1.0e-3f);
        CHECK(IsPredictionErrorNegligible(error));
        CHECK_FALSE(
            IsPredictionErrorNegligible(PredictionError{.Position = vec3(0.1f, 0.0f, 0.0f)}));
    }

    // ---- Unit: the Reconcile arms in isolation ---------------------------------------------------

    struct SoloWorld
    {
        TypeRegistry Types;
        Unique<Scene> World;
        PredictionHistory History;
        MovementSystem Movement;
        Entity Pawn = Entity::Null;

        alignas(16) unsigned char AssetsBytes[64]{};
        alignas(16) unsigned char InputBytes[64]{};
        alignas(16) unsigned char TasksBytes[64]{};

        SoloWorld()
        {
            RegisterBuiltinTypes(Types);
            World = Scene::Create(Types);
            Pawn = World->CreateEntity();
            World->Add<Transform>(Pawn);
            World->Add<Intent>(Pawn);
            World->Add<Mover>(Pawn, Mover{.MoveSpeed = 4.0f, .TurnSpeed = 2.0f});
            World->Add<Authority>(Pawn, Authority{.Tier = Tier::Predicted});
            History.Track(Pawn);
        }

        SystemContext Context(const bool replay)
        {
            return SystemContext{
                .Assets = *reinterpret_cast<AssetManager*>(AssetsBytes),
                .Input = *reinterpret_cast<Input*>(InputBytes),
                .Tasks = *reinterpret_cast<TaskSystem*>(TasksBytes),
                .Role = NetRole::Client,
                .IsReplay = replay,
            };
        }

        // One live client tick: derive Intent, integrate, record.
        void PredictTick(const u64 tick, const vec2 move)
        {
            constexpr f32 Delta = 1.0f / 60.0f;
            PlayerInput input;
            input.State = MoveState(move);
            World->Get<Intent>(Pawn) = ControlMap(input);
            Movement.OnUpdate(*World, Delta, Context(false));
            History.Record(tick, input, *World);
        }

        // The replay driver Reconcile calls per rolled-back tick.
        ReplayTick Replay()
        {
            return [this](Scene& scene, const u64, const PlayerInput& input)
            {
                constexpr f32 Delta = 1.0f / 60.0f;
                scene.Get<Intent>(Pawn) = ControlMap(input);
                Movement.OnUpdate(scene, Delta, Context(true));
            };
        }

        PredictedRecord AuthoritativeTransform(const Transform& transform) const
        {
            PredictedRecord record;
            record.Entity = Pawn;
            record.Components.push_back(PredictedRecord::Component{
                .Type = TypeIdOf<Transform>(), .Bytes = SerializeTransform(Types, transform)});
            return record;
        }
    };

    TEST_CASE("Reconcile: a matching prediction stands and history through C is trimmed")
    {
        SoloWorld w;
        for (u64 tick = 1; tick <= 6; ++tick)
        {
            w.PredictTick(tick, vec2(1.0f, 0.0f));
        }
        REQUIRE(w.History.Contains(4));

        // The authoritative state at tick 4 equals the recorded prediction — a match.
        const Transform predictedAt4 =
            w.World->Get<Transform>(w.Pawn); // pose now; rebuild @4 below
        // Reconstruct the recorded pose at tick 4 by restoring, reading, then re-predicting is
        // unnecessary: the capture at 4 is what the server would confirm, so build the record from it.
        Transform authoritative;
        {
            // Restore to tick 4 to read the predicted pose, then restore forward is not needed —
            // Reconcile compares against the capture directly, so hand it that exact pose.
            REQUIRE(w.History.Restore(4, *w.World));
            authoritative = w.World->Get<Transform>(w.Pawn);
            // Put the live pose back to "now" by replaying 5,6 — keep the world consistent.
            const auto tape = w.History.InputsAfter(4);
            for (const StoredInput& in : tape)
            {
                w.World->Get<Intent>(w.Pawn) = ControlMap(in.Input);
                w.Movement.OnUpdate(*w.World, 1.0f / 60.0f, w.Context(true));
            }
        }
        (void)predictedAt4;

        const usize sizeBefore = w.History.Size();
        const std::vector<PredictedRecord> records{w.AuthoritativeTransform(authoritative)};
        const ReconcileResult result =
            Reconcile(*w.World, w.History, records, 4, w.Replay(), ReconcileTolerances{});

        CHECK(result.Compared);
        CHECK_FALSE(result.Corrected);
        CHECK(result.ReplayedTicks == 0);
        // History through 4 is dropped (server confirmed it); the later ticks remain.
        CHECK_FALSE(w.History.Contains(4));
        CHECK(w.History.Contains(5));
        CHECK(w.History.Size() < sizeBefore);
    }

    TEST_CASE("Reconcile: a mispredict restores to authoritative and replays forward")
    {
        SoloWorld w;
        for (u64 tick = 1; tick <= 6; ++tick)
        {
            w.PredictTick(tick, vec2(1.0f, 0.0f));
        }
        REQUIRE(w.History.Contains(3));

        // The server disagrees at tick 3: the pawn is 2 units further along +x than predicted.
        REQUIRE(w.History.Restore(3, *w.World));
        Transform authoritative = w.World->Get<Transform>(w.Pawn);
        authoritative.Position.x += 2.0f;
        // Leave the live pose at "now" for the pre-correction capture (replay 4,5,6 back).
        {
            const auto tape = w.History.InputsAfter(3);
            for (const StoredInput& in : tape)
            {
                w.World->Get<Intent>(w.Pawn) = ControlMap(in.Input);
                w.Movement.OnUpdate(*w.World, 1.0f / 60.0f, w.Context(true));
            }
        }

        const std::vector<PredictedRecord> records{w.AuthoritativeTransform(authoritative)};
        const ReconcileResult result =
            Reconcile(*w.World, w.History, records, 3, w.Replay(), ReconcileTolerances{});

        CHECK(result.Compared);
        CHECK(result.Corrected);
        CHECK_FALSE(result.Snapped);
        CHECK(result.ReplayedTicks == 3); // ticks 4, 5, 6 replayed

        // The corrected pose is the authoritative tick-3 pose replayed forward three ticks of +x —
        // exactly 2 units past where the uncorrected prediction sat.
        const f32 speed = w.World->Get<Mover>(w.Pawn).MoveSpeed;
        const f32 expected = authoritative.Position.x + speed * (1.0f / 60.0f) * 3.0f;
        CHECK(w.World->Get<Transform>(w.Pawn).Position.x == doctest::Approx(expected));

        // A small correction eases: the pawn carries a decaying render residual, not a snap.
        REQUIRE(w.World->Has<PredictionError>(w.Pawn));
        CHECK(glm::length(w.World->Get<PredictionError>(w.Pawn).Position) > 0.0f);
    }

    TEST_CASE("Reconcile: a confirmation older than the ring holds the prediction, never crashing")
    {
        SoloWorld w;
        for (u64 tick = 5; tick <= 8; ++tick)
        {
            w.PredictTick(tick, vec2(1.0f, 0.0f));
        }
        REQUIRE(w.History.OldestTick() == 5);
        const vec3 poseBefore = w.World->Get<Transform>(w.Pawn).Position;
        const usize sizeBefore = w.History.Size();

        // The confirmation is for tick 2 — older than the retained ring (the warm-up degenerate, or an
        // extreme spike). There is no capture at 2 to compare or roll back to: the prediction stands
        // and the history is left to grow, so reconciliation resumes once the window covers the
        // confirmed tick. Never a crash, never a history-clearing snap cycle.
        Transform authoritative;
        authoritative.Position = vec3(9.0f, 0.0f, 0.0f);
        const std::vector<PredictedRecord> records{w.AuthoritativeTransform(authoritative)};
        const ReconcileResult result =
            Reconcile(*w.World, w.History, records, 2, w.Replay(), ReconcileTolerances{});

        CHECK(result.Compared);
        CHECK_FALSE(result.Corrected);
        CHECK(result.ReplayedTicks == 0);
        // The prediction and its history are untouched — held until the window catches up.
        CHECK(w.History.Size() == sizeBefore);
        CHECK(w.World->Get<Transform>(w.Pawn).Position.x == doctest::Approx(poseBefore.x));
        CHECK_FALSE(w.World->Has<PredictionError>(w.Pawn));
    }

    // ---- Two-world convergence gate --------------------------------------------------------------

    const ConnectionConfig FastConfig{
        .ResendInterval = 0.02, .KeepaliveInterval = 0.05, .TimeoutInterval = 5.0};

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

    struct FakeContext
    {
        alignas(16) unsigned char AssetsBytes[64]{};
        alignas(16) unsigned char InputBytes[64]{};
        alignas(16) unsigned char TasksBytes[64]{};
        NetRole Role = NetRole::Server;
        bool Replay = false;

        SystemContext Make()
        {
            return SystemContext{
                .Assets = *reinterpret_cast<AssetManager*>(AssetsBytes),
                .Input = *reinterpret_cast<Input*>(InputBytes),
                .Tasks = *reinterpret_cast<TaskSystem*>(TasksBytes),
                .Role = Role,
                .IsReplay = Replay,
            };
        }
    };

    // The server half, scheduled-consume so each snapshot confirms its consumed-input tick.
    struct ServerWorld
    {
        TypeRegistry Types;
        Unique<Scene> World;
        Unique<ServerHost> Host;
        Ref<Prefab> PawnPrefab;
        std::unordered_map<u64, InputJitterBuffer> Jitter;
        std::unordered_map<ConnectionId, Entity> Pawns;
        MovementSystem Movement;

        explicit ServerWorld(Transport& transport,
                             ReplicationServer::Settings settings = {.SnapshotInterval = 2},
                             f32 interestRadius = 0.0f)
        {
            RegisterBuiltinTypes(Types);
            World = Scene::Create(Types);
            PawnPrefab = MakePawnPrefab(Types);
            Result<Unique<ServerHost>> host = ServerHost::Create(ServerHostInfo{
                .Server = ServerInfo{.TransportOverride = &transport, .Connection = FastConfig},
                .World = *World,
                .Assets = FakeAssets(),
                .LevelId = LevelId,
                .Replication = settings,
                .Interest = InterestSettings{.Radius = interestRadius, .MinDwellSnapshots = 2},
            });
            REQUIRE(host.has_value());
            Host = std::move(*host);
        }

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
                AssignServerNetIds(*World, Host->Allocator());
                Host->Replication().SetEntityPrefab(World->Get<NetIdentity>(pawn).Id, PawnPrefabId);
                Pawns[id] = pawn;
            }
        }

        void SimStep(u64 tick, f32 delta)
        {
            World->SetChangeTick(tick);
            RunSpawnRule();
            FeedSeatInputs(*Host, Jitter, WorldInstanceId{}, *World,
                           tick); // scheduled: confirms LastConsumedInputTick

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

    // The client half, with the ClientHost's rollback replay driver wired to control + movement.
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

        explicit ClientWorld(Transport& transport)
        {
            RegisterBuiltinTypes(Types);
            PawnPrefab = MakePawnPrefab(Types);
            Client = *Net::Client::Connect(
                ClientInfo{.TransportOverride = &transport, .Connection = FastConfig});
            Send = InputSendBuffer(InputSendBuffer::Settings{.Redundancy = 4});
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
                    if (!LocalSeat.IsNull() && world.IsAlive(LocalSeat) &&
                        world.Has<Possesses>(LocalSeat))
                    {
                        world.Get<Possesses>(LocalSeat).Pawn = pawn;
                    }
                },
                .Replay =
                    [this](Scene& world, const u64, const PlayerInput& input)
                {
                    constexpr f32 Delta = 1.0f / 60.0f;
                    if (!LocalSeat.IsNull() && world.IsAlive(LocalSeat) &&
                        world.Has<PlayerInput>(LocalSeat))
                    {
                        world.Get<PlayerInput>(LocalSeat) = input;
                    }
                    if (!OwnPawn.IsNull() && world.IsAlive(OwnPawn) && world.Has<Intent>(OwnPawn))
                    {
                        world.Get<Intent>(OwnPawn) = ControlMap(input);
                    }
                    FakeContext ctx;
                    ctx.Role = NetRole::Client;
                    ctx.Replay = true;
                    Movement.OnUpdate(world, Delta, ctx.Make());
                },
            });
        }

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
            if (!OwnPawn.IsNull() && world->IsAlive(OwnPawn) && world->Has<Intent>(OwnPawn))
            {
                world->Get<Intent>(OwnPawn) = ControlMap(world->Get<PlayerInput>(LocalSeat));
            }
            FakeContext ctx;
            ctx.Role = NetRole::Client;
            Movement.OnUpdate(*world, delta, ctx.Make());
            Host->RecordPrediction(clientTick);
            StampLocalSeatInput(Send, *world, clientTick);
        }

        void Frame(f64 now, u64 clientTick, f32 delta, const optional<ActionState>& scripted)
        {
            Host->Pump(now); // applies snapshots + reconciles the predicted set
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

    // Steps both worlds one iteration with the client leading the server by Lead ticks (the
    // ahead-of-server tick model that makes the input for server tick T arrive by T).
    void StepAhead(ServerWorld& server, ClientWorld& client, u64 serverTick, u64 lead, f64& now,
                   const optional<ActionState>& scripted, ConnectionId& id)
    {
        constexpr f32 Delta = 1.0f / 60.0f;
        now += Delta;
        client.Frame(now, serverTick + lead, Delta, scripted);
        server.SimStep(serverTick, Delta);
        server.NetPump(now, serverTick);
        if (!server.Host->Server().Connections().empty())
        {
            id = server.Host->Server().Connections().front();
        }
    }
}

TEST_CASE("Convergence: prediction under seeded loss converges byte-equal after quiescence")
{
    auto [serverT, clientT] = LoopbackTransport::CreatePair();
    // A lossy/reordering/duplicating link on both directions: input packets drop (forcing
    // server-side consume underruns that diverge from the client's prediction) and snapshots drop
    // and reorder. Reconciliation must still converge the predicted pawn onto the authoritative one.
    const FaultInjectionConfig faults{
        .DropRate = 0.2f, .DuplicateRate = 0.1f, .ReorderRate = 0.15f, .Seed = 4242};
    FaultInjectionTransport serverLink(*serverT, faults);
    FaultInjectionTransport clientLink(*clientT, faults);

    ServerWorld server(serverLink);
    ClientWorld client(clientLink);

    constexpr u64 Lead = 4;
    const ActionState move = MoveState(vec2(1.0f, 0.0f));
    const ActionState stop = MoveState(vec2(0.0f, 0.0f));

    f64 now = 0.0;
    ConnectionId id = ServerConnectionId;
    // Drive a sustained hold, then a long quiescent tail so the lossy stream and the reconciler
    // settle onto the halted authoritative pose.
    for (u64 tick = 1; tick <= 500; ++tick)
    {
        StepAhead(server, client, tick, Lead, now, tick <= 300 ? move : stop, id);
    }

    REQUIRE(client.Host->IsJoined());
    REQUIRE(id != ServerConnectionId);
    const Entity serverPawn = server.PawnFor(id);
    REQUIRE_FALSE(serverPawn.IsNull());
    const Entity clientPawn = client.OwnPawn;
    REQUIRE_FALSE(clientPawn.IsNull());

    // The pawn actually moved (the run exercised real prediction + correction, not a rest state).
    CHECK(server.World->Get<Transform>(serverPawn).Position.x > 1.0f);

    // The convergence invariant: after quiescence the predicted pose equals the authoritative pose
    // byte-for-byte — restore + replay converged despite the loss burst.
    const vector<u8> serverBytes =
        SerializeTransform(server.Types, server.World->Get<Transform>(serverPawn));
    const vector<u8> clientBytes =
        SerializeTransform(client.Types, client.Host->World()->Get<Transform>(clientPawn));
    CHECK(clientBytes == serverBytes);

    // The residual has fully eased out (no lingering render offset once converged).
    CHECK_FALSE(client.Host->World()->Has<PredictionError>(clientPawn));

    // History did not grow without bound: the on-match TrimThrough kept the ring shallow.
    CHECK(client.Host->History().Size() <= Lead + 4);
}

TEST_CASE("Convergence: a forced server displacement corrects with one smoothed correction")
{
    auto [serverT, clientT] = LoopbackTransport::CreatePair();
    ServerWorld server(*serverT);
    ClientWorld client(*clientT);

    constexpr u64 Lead = 4;
    const ActionState stop = MoveState(vec2(0.0f, 0.0f));

    f64 now = 0.0;
    ConnectionId id = ServerConnectionId;
    // Join and settle at rest — no input, so client prediction and server agree.
    for (u64 tick = 1; tick <= 60; ++tick)
    {
        StepAhead(server, client, tick, Lead, now, stop, id);
    }
    REQUIRE(client.Host->IsJoined());
    REQUIRE(id != ServerConnectionId);
    const Entity serverPawn = server.PawnFor(id);
    const Entity clientPawn = client.OwnPawn;
    REQUIRE_FALSE(serverPawn.IsNull());
    REQUIRE_FALSE(clientPawn.IsNull());

    // Forcibly displace the server pawn a modest (below snap-threshold) distance — a server-side
    // interaction the client never predicted (a shove, a collision).
    server.World->SetChangeTick(60);
    server.World->Get<Transform>(serverPawn).Position.z += 0.5f;

    // A few ticks later the client has a render residual (the correction is being smoothed, not
    // snapped) — the pose is being eased, not teleported.
    bool sawSmoothing = false;
    for (u64 tick = 61; tick <= 90; ++tick)
    {
        StepAhead(server, client, tick, Lead, now, stop, id);
        if (client.Host->World()->Has<PredictionError>(clientPawn))
        {
            sawSmoothing = true;
        }
    }
    CHECK(sawSmoothing);

    // Let it settle, then assert byte-equal convergence and that the residual eased fully out.
    for (u64 tick = 91; tick <= 200; ++tick)
    {
        StepAhead(server, client, tick, Lead, now, stop, id);
    }
    const vector<u8> serverBytes =
        SerializeTransform(server.Types, server.World->Get<Transform>(serverPawn));
    const vector<u8> clientBytes =
        SerializeTransform(client.Types, client.Host->World()->Get<Transform>(clientPawn));
    CHECK(clientBytes == serverBytes);
    CHECK_FALSE(client.Host->World()->Has<PredictionError>(clientPawn));
}

TEST_CASE(
    "Convergence: all mechanisms under combined loss + latency converge byte-equal (lossless wire)")
{
    auto [serverT, clientT] = LoopbackTransport::CreatePair();

    // The full adversity: loss, reorder, duplication, and a latency + jitter delay queue, both ways.
    // The wire stays lossless (quantization off), so the predicted pose must converge byte-for-byte.
    const FaultInjectionConfig faults{.DropRate = 0.1f,
                                      .DuplicateRate = 0.05f,
                                      .ReorderRate = 0.1f,
                                      .LatencyMs = 30.0f,
                                      .JitterMs = 8.0f,
                                      .Seed = 99};
    SimulatedTransport serverLink(*serverT, faults);
    SimulatedTransport clientLink(*clientT, faults);

    // Every mechanism on: prediction + rollback (the client harness), delta compression (always),
    // and interest management (radius large enough the own pawn stays relevant while the filter runs).
    ServerWorld server(serverLink, {.SnapshotInterval = 2, .KeyframeInterval = 8},
                       /*interestRadius=*/1000.0f);
    ClientWorld client(clientLink);

    constexpr u64 Lead = 4;
    const ActionState move = MoveState(vec2(1.0f, 0.0f));
    const ActionState stop = MoveState(vec2(0.0f, 0.0f));

    f64 now = 0.0;
    constexpr f32 Delta = 1.0f / 60.0f;
    ConnectionId id = ServerConnectionId;
    for (u64 tick = 1; tick <= 700; ++tick)
    {
        now += Delta;
        serverLink.SetTime(now);
        clientLink.SetTime(now);
        client.Frame(now, tick + Lead, Delta, tick <= 450 ? move : stop);
        server.SimStep(tick, Delta);
        server.NetPump(now, tick);
        if (!server.Host->Server().Connections().empty())
        {
            id = server.Host->Server().Connections().front();
        }
    }

    REQUIRE(client.Host->IsJoined());
    REQUIRE(id != ServerConnectionId);
    const Entity serverPawn = server.PawnFor(id);
    const Entity clientPawn = client.OwnPawn;
    REQUIRE_FALSE(serverPawn.IsNull());
    REQUIRE_FALSE(clientPawn.IsNull());
    CHECK(server.World->Get<Transform>(serverPawn).Position.x > 1.0f);

    const vector<u8> serverBytes =
        SerializeTransform(server.Types, server.World->Get<Transform>(serverPawn));
    const vector<u8> clientBytes =
        SerializeTransform(client.Types, client.Host->World()->Get<Transform>(clientPawn));
    CHECK(clientBytes == serverBytes);
    CHECK_FALSE(client.Host->World()->Has<PredictionError>(clientPawn));

    // Bounded memory: the on-match trim kept the history ring shallow through the whole run.
    CHECK(client.Host->History().Size() <= Lead + 6);
}

TEST_CASE("Convergence: prediction with quantization + interest converges within the quantum")
{
    auto [serverT, clientT] = LoopbackTransport::CreatePair();
    const FaultInjectionConfig faults{.DropRate = 0.08f,
                                      .DuplicateRate = 0.05f,
                                      .ReorderRate = 0.1f,
                                      .LatencyMs = 25.0f,
                                      .JitterMs = 6.0f,
                                      .Seed = 271};
    SimulatedTransport serverLink(*serverT, faults);
    SimulatedTransport clientLink(*clientT, faults);

    // Quantization on (default 1 mm grid) with interest and prediction: the reconciliation epsilon
    // (1 cm) exceeds the quantum, so quantization noise never reads as a misprediction, and the
    // displayed pose converges to within the quantum of the authoritative one after quiescence.
    ServerWorld server(serverLink,
                       {.SnapshotInterval = 2, .QuantizeSpatial = true, .KeyframeInterval = 8},
                       /*interestRadius=*/1000.0f);
    ClientWorld client(clientLink); // default quantization matches the server's

    constexpr u64 Lead = 4;
    const ActionState move = MoveState(vec2(1.0f, 0.0f));
    const ActionState stop = MoveState(vec2(0.0f, 0.0f));

    f64 now = 0.0;
    constexpr f32 Delta = 1.0f / 60.0f;
    ConnectionId id = ServerConnectionId;
    for (u64 tick = 1; tick <= 700; ++tick)
    {
        now += Delta;
        serverLink.SetTime(now);
        clientLink.SetTime(now);
        client.Frame(now, tick + Lead, Delta, tick <= 450 ? move : stop);
        server.SimStep(tick, Delta);
        server.NetPump(now, tick);
        if (!server.Host->Server().Connections().empty())
        {
            id = server.Host->Server().Connections().front();
        }
    }

    REQUIRE(client.Host->IsJoined());
    REQUIRE(id != ServerConnectionId);
    const Entity serverPawn = server.PawnFor(id);
    const Entity clientPawn = client.OwnPawn;
    REQUIRE_FALSE(serverPawn.IsNull());
    REQUIRE_FALSE(clientPawn.IsNull());
    CHECK(server.World->Get<Transform>(serverPawn).Position.x > 1.0f);

    const vec3 serverPos = server.World->Get<Transform>(serverPawn).Position;
    const vec3 clientPos = client.Host->World()->Get<Transform>(clientPawn).Position;
    CHECK(glm::length(clientPos - serverPos) < 0.01f); // within the 1 cm reconcile epsilon
    CHECK_FALSE(client.Host->World()->Has<PredictionError>(clientPawn)); // residual eased out
    CHECK(client.Host->History().Size() <= Lead + 6);
}
