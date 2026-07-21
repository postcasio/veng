// Plan 04's server→client state flow: spawn/despawn + latest-wins snapshots + remote interpolation,
// exercised device-free with two in-process scenes. Most cases drive ReplicationServer::Generate →
// ReplicationClient directly (pure codec round-trip, no transport); one integration case runs the
// whole flow over a real Server/Client + LoopbackTransport, proving the reliable app-message inbox
// and the interpolation system produce a delayed, bounded-error track of the server's motion. The
// pure interpolation-window math is tested analytically over scripted samples.

#include <doctest/doctest.h>

#include <Veng/Asset/Prefab.h>
#include <Veng/Net/Client.h>
#include <Veng/Net/LoopbackTransport.h>
#include <Veng/Net/Replication.h>
#include <Veng/Net/Server.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>

#include "support/TestComponents.h"
#include <Veng/Scene/Components.h>
#include <Veng/Scene/RemoteInterpolationSystem.h>
#include <Veng/Scene/Scene.h>

using namespace Veng;
using namespace Veng::Net;

namespace
{
    // A SystemContext whose service references the interpolation system never dereferences (it reads
    // only the scene and delta). Mirrors the game_mode.cpp device-free pattern.
    struct FakeContext
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

    // The prefab-arm spawn calls SpawnInto(scene, assets); a dependency-free prefab never touches the
    // manager, so a never-dereferenced reference is safe (the game_mode.cpp precedent).
    AssetManager& FakeAssets()
    {
        alignas(16) static unsigned char bytes[64]{};
        return *reinterpret_cast<AssetManager*>(bytes);
    }

    vector<u8> ComponentRecord(const TypeRegistry& registry, TypeId id, const void* value)
    {
        vector<u8> out;
        WriteFields(out, value, registry.Info(id), registry);
        return out;
    }

    // Feeds every message ReplicationServer produces this tick into the client (bypassing the
    // transport — the codec round-trip only).
    void PumpReplication(ReplicationServer& server, ReplicationClient& client, ConnectionId id,
                         const Scene& serverScene, Scene& clientScene, u64 tick)
    {
        for (const ReplicationMessage& message : server.Generate(id, serverScene, tick))
        {
            if (message.Channel == Channel::ReliableOrdered)
            {
                client.ApplyReliable(message.Bytes, clientScene, FakeAssets());
            }
            else
            {
                client.ApplySnapshot(message.Bytes, clientScene);
            }
        }
    }

    ReplicationClient MakeClient()
    {
        return ReplicationClient([](AssetId) -> Ref<Prefab> { return nullptr; });
    }
}

TEST_CASE("SampleRemoteInterpolation brackets, blends, and holds at the ends")
{
    const vector<RemoteSample> samples = {
        RemoteSample{.ServerTick = 0, .Position = vec3(0.0f)},
        RemoteSample{.ServerTick = 2, .Position = vec3(2.0f, 0.0f, 0.0f)},
        RemoteSample{.ServerTick = 4, .Position = vec3(4.0f, 0.0f, 0.0f)},
    };

    // Exact-tick and midpoint blends.
    CHECK(SampleRemoteInterpolation(samples, 2.0)->Position.x == doctest::Approx(2.0f));
    CHECK(SampleRemoteInterpolation(samples, 1.0)->Position.x == doctest::Approx(1.0f));
    CHECK(SampleRemoteInterpolation(samples, 3.0)->Position.x == doctest::Approx(3.0f));

    // Hold before the oldest and after the newest — no extrapolation.
    CHECK(SampleRemoteInterpolation(samples, -5.0)->Position.x == doctest::Approx(0.0f));
    CHECK(SampleRemoteInterpolation(samples, 9.0)->Position.x == doctest::Approx(4.0f));

    // An empty buffer yields nothing.
    CHECK_FALSE(SampleRemoteInterpolation(std::span<const RemoteSample>{}, 1.0).has_value());
}

TEST_CASE("The RemoteInterpolationSystem renders the delay-lagged, blended pose")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    Unique<Scene> scene = Scene::Create(types);

    const Entity remote = scene->CreateEntity();
    scene->Add<Transform>(remote);
    auto& interp = scene->Add<RemoteInterpolation>(remote);
    for (u64 t = 0; t <= 8; t += 2)
    {
        interp.Samples.push_back(
            RemoteSample{.ServerTick = t, .Position = vec3(static_cast<f32>(t), 0.0f, 0.0f)});
    }

    RemoteInterpolationSystem system;
    system.SetSettings(RemoteInterpolationSystem::Settings{
        .SnapshotInterval = 2, .InterpolationDelayIntervals = 2, .SimTickRate = 60.0});

    FakeContext ctx;

    // First update seeds the playback clock at newest(8) − delay(4) = tick 4 → sample@4.
    system.OnUpdate(*scene, 0.0f, ctx.Make());
    CHECK(scene->Get<Transform>(remote).Position.x == doctest::Approx(4.0f));

    // A one-tick frame (delta·rate = 1) advances playback to 5 → blended halfway between 4 and 6.
    system.OnUpdate(*scene, 1.0f / 60.0f, ctx.Make());
    CHECK(scene->Get<Transform>(remote).Position.x == doctest::Approx(5.0f));
}

TEST_CASE("A prefab-less spawn materializes the entity with full state, marked Tier::Remote")
{
    TypeRegistry serverTypes;
    RegisterBuiltinTypes(serverTypes);
    serverTypes.Register<VengTest::TestScore>();
    Unique<Scene> server = Scene::Create(serverTypes);

    const Entity pawn = server->CreateEntity();
    server->Add<Transform>(pawn, Transform{.Position = vec3(1.0f, 2.0f, 3.0f)});
    server->Add<VengTest::TestScore>(pawn, VengTest::TestScore{.Value = 5});

    NetIdAllocator allocator;
    AssignServerNetIds(*server, allocator);
    const NetId pawnId = server->Get<NetIdentity>(pawn).Id;

    TypeRegistry clientTypes;
    RegisterBuiltinTypes(clientTypes);
    clientTypes.Register<VengTest::TestScore>();
    Unique<Scene> client = Scene::Create(clientTypes);

    ReplicationServer replServer;
    ReplicationClient replClient = MakeClient();
    replServer.AddConnection(1);

    // A spawn rides on tick 2 (a snapshot-interval tick) alongside the first snapshot.
    PumpReplication(replServer, replClient, 1, *server, *client, 2);

    const Entity clientPawn = replClient.Map().Lookup(pawnId);
    REQUIRE_FALSE(clientPawn.IsNull());
    REQUIRE(client->IsAlive(clientPawn));

    // The spawn's full state applies to the live components (Transform live, discrete immediate).
    const auto* transform = client->TryGet<Transform>(clientPawn);
    REQUIRE(transform != nullptr);
    CHECK(transform->Position.x == doctest::Approx(1.0f));
    CHECK(transform->Position.z == doctest::Approx(3.0f));
    const auto* score = client->TryGet<VengTest::TestScore>(clientPawn);
    REQUIRE(score != nullptr);
    CHECK(score->Value == 5);

    // Marked a remote mirror.
    const auto* authority = client->TryGet<Authority>(clientPawn);
    REQUIRE(authority != nullptr);
    CHECK(authority->Tier == Tier::Remote);
}

TEST_CASE("A spawn rides a prefab AssetId through the ordinary prefab path")
{
    TypeRegistry serverTypes;
    RegisterBuiltinTypes(serverTypes);
    Unique<Scene> server = Scene::Create(serverTypes);

    const Entity pawn = server->CreateEntity();
    server->Add<Transform>(pawn, Transform{.Position = vec3(7.0f, 0.0f, 0.0f)});

    NetIdAllocator allocator;
    AssignServerNetIds(*server, allocator);
    const NetId pawnId = server->Get<NetIdentity>(pawn).Id;

    const AssetId prefabId{0x00000000ABCD0001ULL};

    // A dependency-free in-memory prefab: one entity with a Transform (a distinct default so the
    // spawn's server state visibly overwrites it).
    TypeRegistry clientTypes;
    RegisterBuiltinTypes(clientTypes);
    const Transform prefabDefault{.Position = vec3(-1.0f, -1.0f, -1.0f)};
    Prefab::PrefabEntity entity;
    entity.Components.push_back(Prefab::Component{
        .Type = TypeIdOf<Transform>(),
        .Record = ComponentRecord(clientTypes, TypeIdOf<Transform>(), &prefabDefault)});
    vector<Prefab::PrefabEntity> entities;
    entities.push_back(std::move(entity));
    const Ref<Prefab> prefab = Prefab::Create(std::move(entities), {});

    Unique<Scene> client = Scene::Create(clientTypes);

    ReplicationServer replServer;
    ReplicationClient replClient([&](AssetId id) -> Ref<Prefab>
                                 { return id == prefabId ? prefab : nullptr; });
    replServer.AddConnection(1);
    replServer.SetEntityPrefab(pawnId, prefabId);

    PumpReplication(replServer, replClient, 1, *server, *client, 2);

    const Entity clientPawn = replClient.Map().Lookup(pawnId);
    REQUIRE_FALSE(clientPawn.IsNull());
    // The prefab was instantiated (the entity exists) and the server state overwrote its default.
    const auto* transform = client->TryGet<Transform>(clientPawn);
    REQUIRE(transform != nullptr);
    CHECK(transform->Position.x == doctest::Approx(7.0f));
    CHECK(client->Get<Authority>(clientPawn).Tier == Tier::Remote);
}

TEST_CASE("Snapshots buffer Transform samples while non-spatial state applies immediately")
{
    TypeRegistry serverTypes;
    RegisterBuiltinTypes(serverTypes);
    serverTypes.Register<VengTest::TestScore>();
    Unique<Scene> server = Scene::Create(serverTypes);

    const Entity pawn = server->CreateEntity();
    server->Add<Transform>(pawn, Transform{.Position = vec3(0.0f)});
    server->Add<VengTest::TestScore>(pawn, VengTest::TestScore{.Value = 1});

    NetIdAllocator allocator;
    AssignServerNetIds(*server, allocator);
    const NetId pawnId = server->Get<NetIdentity>(pawn).Id;

    TypeRegistry clientTypes;
    RegisterBuiltinTypes(clientTypes);
    clientTypes.Register<VengTest::TestScore>();
    Unique<Scene> client = Scene::Create(clientTypes);

    ReplicationServer replServer;
    ReplicationClient replClient = MakeClient();
    replServer.AddConnection(1);

    // Spawn on tick 2.
    PumpReplication(replServer, replClient, 1, *server, *client, 2);
    const Entity clientPawn = replClient.Map().Lookup(pawnId);
    REQUIRE_FALSE(clientPawn.IsNull());

    // Move + change the discrete score on tick 4, then snapshot.
    server->SetChangeTick(4);
    server->Get<Transform>(pawn).Position.x = 9.0f;
    server->Get<VengTest::TestScore>(pawn).Value = 2;
    PumpReplication(replServer, replClient, 1, *server, *client, 4);

    // The discrete change applied immediately (stale-but-consistent beats interpolated for discrete state).
    CHECK(client->Get<VengTest::TestScore>(clientPawn).Value == 2);

    // The moved Transform is buffered, not snapped onto the live pose: the live Transform still holds
    // the spawn value until the View-phase system renders a sample.
    CHECK(client->Get<Transform>(clientPawn).Position.x == doctest::Approx(0.0f));
    const RemoteInterpolation& interp = client->Get<RemoteInterpolation>(clientPawn);
    REQUIRE_FALSE(interp.Samples.empty());
    CHECK(interp.Samples.back().Position.x == doctest::Approx(9.0f));
    CHECK(interp.Samples.back().ServerTick == 4);
}

TEST_CASE("A snapshot for an unknown NetId drops idempotently and converges after the spawn")
{
    TypeRegistry serverTypes;
    RegisterBuiltinTypes(serverTypes);
    Unique<Scene> server = Scene::Create(serverTypes);

    // Populated with no tick stepped: the snapshot below is encoded for a later server tick, but the
    // pawn's own change tick is the floor, so the record is selected by the pre-tick path.
    const Entity pawn = server->CreateEntity();
    server->Add<Transform>(pawn, Transform{.Position = vec3(3.0f, 0.0f, 0.0f)});

    NetIdAllocator allocator;
    AssignServerNetIds(*server, allocator);
    const NetId pawnId = server->Get<NetIdentity>(pawn).Id;

    TypeRegistry clientTypes;
    RegisterBuiltinTypes(clientTypes);
    Unique<Scene> client = Scene::Create(clientTypes);
    ReplicationClient replClient = MakeClient();

    // A snapshot before the entity is spawned: its record has no local binding, so it drops.
    const vector<u8> snapshot = EncodeSnapshot(*server, 4, 0);
    const SnapshotApplyResult dropped = replClient.ApplySnapshot(snapshot, *client);
    CHECK(dropped.HeaderValid);
    CHECK(dropped.EntitiesApplied == 0);
    CHECK(dropped.EntitiesDropped == 1);

    // After the spawn binds the id, re-applying the same snapshot converges (idempotent).
    ReplicationServer replServer;
    replServer.AddConnection(1);
    for (const ReplicationMessage& message :
         replServer.Generate(1, *server, 1)) // odd tick: spawn only
    {
        if (message.Channel == Channel::ReliableOrdered)
        {
            replClient.ApplyReliable(message.Bytes, *client, FakeAssets());
        }
    }
    const Entity clientPawn = replClient.Map().Lookup(pawnId);
    REQUIRE_FALSE(clientPawn.IsNull());

    const SnapshotApplyResult applied = replClient.ApplySnapshot(snapshot, *client);
    CHECK(applied.EntitiesApplied == 1);
    CHECK(applied.EntitiesDropped == 0);
    CHECK(client->Get<RemoteInterpolation>(clientPawn).Samples.back().Position.x ==
          doctest::Approx(3.0f));
}

TEST_CASE("A despawn destroys the client mirror and unbinds the map")
{
    TypeRegistry serverTypes;
    RegisterBuiltinTypes(serverTypes);
    Unique<Scene> server = Scene::Create(serverTypes);

    const Entity pawn = server->CreateEntity();
    server->Add<Transform>(pawn);

    NetIdAllocator allocator;
    AssignServerNetIds(*server, allocator);
    const NetId pawnId = server->Get<NetIdentity>(pawn).Id;

    TypeRegistry clientTypes;
    RegisterBuiltinTypes(clientTypes);
    Unique<Scene> client = Scene::Create(clientTypes);

    ReplicationServer replServer;
    ReplicationClient replClient = MakeClient();
    replServer.AddConnection(1);

    PumpReplication(replServer, replClient, 1, *server, *client, 2);
    const Entity clientPawn = replClient.Map().Lookup(pawnId);
    REQUIRE_FALSE(clientPawn.IsNull());
    CHECK(replClient.Map().Size() == 1);

    // The server destroys the entity; the next Generate emits a Despawn.
    server->DestroyEntity(pawn);
    PumpReplication(replServer, replClient, 1, *server, *client, 4);

    CHECK(replClient.Map().Lookup(pawnId).IsNull());
    CHECK(replClient.Map().Size() == 0);
    CHECK_FALSE(client->IsAlive(clientPawn));
}

TEST_CASE("Two worlds converge over loopback: connect, spawn, interpolated movement")
{
    auto [serverT, clientT] = LoopbackTransport::CreatePair();

    const ConnectionConfig config{
        .ResendInterval = 0.02, .KeepaliveInterval = 0.05, .TimeoutInterval = 5.0};
    Unique<Server> server =
        *Server::Create(ServerInfo{.TransportOverride = serverT.get(), .Connection = config});
    Unique<Client> client =
        *Client::Connect(ClientInfo{.TransportOverride = clientT.get(), .Connection = config});

    TypeRegistry serverTypes;
    RegisterBuiltinTypes(serverTypes);
    Unique<Scene> serverScene = Scene::Create(serverTypes);
    const Entity mover = serverScene->CreateEntity();
    serverScene->Add<Transform>(mover, Transform{.Position = vec3(0.0f)});

    TypeRegistry clientTypes;
    RegisterBuiltinTypes(clientTypes);
    Unique<Scene> clientScene = Scene::Create(clientTypes);

    NetIdAllocator allocator;
    ReplicationServer replServer;
    ReplicationClient replClient = MakeClient();
    RemoteInterpolationSystem interp;
    interp.SetSettings(RemoteInterpolationSystem::Settings{
        .SnapshotInterval = 2, .InterpolationDelayIntervals = 2, .SimTickRate = 60.0});
    FakeContext ctx;

    f64 now = 0.0;
    NetId moverId = InvalidNetId;
    for (u64 tick = 1; tick <= 60; ++tick)
    {
        now += 1.0 / 60.0;

        // Server sim: advance the mover along +x each tick.
        serverScene->SetChangeTick(tick);
        serverScene->Get<Transform>(mover).Position.x = static_cast<f32>(tick) * 0.1f;
        AssignServerNetIds(*serverScene, allocator);
        moverId = serverScene->Get<NetIdentity>(mover).Id;

        for (const ConnectionId id : server->Connections())
        {
            replServer.AddConnection(id);
            for (const ReplicationMessage& message : replServer.Generate(id, *serverScene, tick))
            {
                (void)server->Get(id).Send(message.Channel, message.Bytes);
            }
        }
        server->Pump(now);
        client->Pump(now);

        // Client: apply reliable spawn/despawn (from the app inbox), then unreliable snapshots.
        for (const vector<u8>& message : client->ReliableAppMessages())
        {
            replClient.ApplyReliable(message, *clientScene, FakeAssets());
        }
        while (const optional<vector<u8>> snapshot =
                   client->Server().Receive(Channel::UnreliableSequenced))
        {
            replClient.ApplySnapshot(*snapshot, *clientScene);
        }

        interp.OnUpdate(*clientScene, 1.0f / 60.0f, ctx.Make());
    }

    REQUIRE(client->State() == ClientState::Connected);
    REQUIRE(moverId != InvalidNetId);

    const Entity clientMover = replClient.Map().Lookup(moverId);
    REQUIRE_FALSE(clientMover.IsNull());
    CHECK(clientScene->Get<Authority>(clientMover).Tier == Tier::Remote);

    // The remote tracks the server's motion: it has advanced well along +x, and lags behind the
    // live server position (rendered in the past), never ahead of it.
    const f32 serverX = serverScene->Get<Transform>(mover).Position.x;
    const f32 clientX = clientScene->Get<Transform>(clientMover).Position.x;
    CHECK(clientX > 0.5f);           // it moved
    CHECK(clientX < serverX);        // it lags the live server position
    CHECK(clientX > serverX - 2.0f); // bounded error — within a couple of ticks' worth of motion
}
