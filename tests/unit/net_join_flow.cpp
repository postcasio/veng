// Plan 06's connection→seat and the join flow, exercised device-free with two in-process scenes over
// LoopbackTransport. The accept spawns a Viewer seat owned by the connection (no SeatInput) and names
// the level + the seat's wire id; the client loads the level with server-authoritative authored
// entities skipped, acks readiness, receives the baseline spawn stream, and wires its Local-tier
// camera to the replicated seat's possessed pawn. Disconnect tears the seat down and surfaces the
// event. The level-load skip is also unit-tested over a mixed-tier authored fixture.

#include <doctest/doctest.h>

#include <Veng/Asset/Prefab.h>
#include <Veng/Net/Client.h>
#include <Veng/Net/Host.h>
#include <Veng/Net/LoopbackTransport.h>
#include <Veng/Net/Server.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

using namespace Veng;
using namespace Veng::Net;

namespace
{
    // A dependency-free prefab never touches the manager, so a never-dereferenced reference is safe
    // (the game_mode.cpp / net_state_flow.cpp precedent).
    AssetManager& FakeAssets()
    {
        alignas(16) static unsigned char bytes[64]{};
        return *reinterpret_cast<AssetManager*>(bytes);
    }

    vector<u8> Record(const TypeRegistry& registry, TypeId id, const void* value)
    {
        vector<u8> out;
        WriteFields(out, value, registry.Info(id), registry);
        return out;
    }

    template <typename T>
    Prefab::Component MakeComponent(const TypeRegistry& registry, const T& value)
    {
        return Prefab::Component{.Type = TypeIdOf<T>(),
                                 .Record = Record(registry, TypeIdOf<T>(), &value)};
    }

    const ConnectionConfig Config{
        .ResendInterval = 0.02, .KeepaliveInterval = 0.05, .TimeoutInterval = 5.0};
}

TEST_CASE("The accept spawns a connection-owned Viewer seat with no SeatInput, and names it")
{
    auto [serverT, clientT] = LoopbackTransport::CreatePair();

    TypeRegistry serverTypes;
    RegisterBuiltinTypes(serverTypes);
    Unique<Scene> serverScene = Scene::Create(serverTypes);

    // A seat template that authors a SeatInput — the host must strip it (the remote seat path).
    Prefab::PrefabEntity seatEntity;
    seatEntity.Components.push_back(MakeComponent(serverTypes, Viewer{}));
    seatEntity.Components.push_back(MakeComponent(serverTypes, Possesses{}));
    seatEntity.Components.push_back(MakeComponent(serverTypes, SeatInput{}));
    vector<Prefab::PrefabEntity> seatEntities;
    seatEntities.push_back(std::move(seatEntity));
    const Ref<Prefab> seatPrefab = Prefab::Create(std::move(seatEntities), {});

    const AssetId levelId{0x0000000000ABCDEFULL};

    Result<Unique<ServerHost>> host = ServerHost::Create(ServerHostInfo{
        .Server = ServerInfo{.TransportOverride = serverT.get(), .Connection = Config},
        .World = *serverScene,
        .Assets = FakeAssets(),
        .LevelId = levelId,
        .SeatPrefab = seatPrefab,
    });
    REQUIRE(host.has_value());

    Unique<Client> client =
        *Client::Connect(ClientInfo{.TransportOverride = clientT.get(), .Connection = Config});

    // The seat spawns when the client joins a world by key, not at the connection accept. A minimal
    // ClientHost auto-joins the default key; its LoadLevel captures the level named in the join reply.
    TypeRegistry clientTypes;
    RegisterBuiltinTypes(clientTypes);
    Unique<Scene> clientScene;
    AssetId requestedLevel;
    Unique<ClientHost> clientHost = ClientHost::Create(ClientHostInfo{
        .Client = *client,
        .Assets = FakeAssets(),
        .LoadLevel = [&](AssetId requested) -> Scene*
        {
            requestedLevel = requested;
            clientScene = Scene::Create(clientTypes);
            return clientScene.get();
        },
        .ResolvePrefab = [](AssetId) -> Ref<Prefab> { return nullptr; },
    });

    f64 now = 0.0;
    for (u64 tick = 1; tick <= 12 && !clientHost->IsJoined(); ++tick)
    {
        now += 1.0 / 60.0;
        serverScene->SetChangeTick(tick);
        (*host)->Pump(now, tick);
        clientHost->Pump(now);
    }

    REQUIRE(client->State() == ClientState::Connected);
    REQUIRE(clientHost->IsJoined());

    // The join reply named the level the client loaded.
    CHECK(requestedLevel.Value == levelId.Value);
    const ConnectionId id = client->AssignedId();
    const Entity seat = (*host)->SeatFor(id);
    REQUIRE_FALSE(seat.IsNull());
    const u32 seatNetId = serverScene->Get<NetIdentity>(seat).Id;
    CHECK(seatNetId != InvalidNetId);

    // The seat is a Viewer owned by the connection, with the SeatInput stripped (the remote path).
    CHECK(serverScene->Has<Viewer>(seat));
    CHECK(serverScene->Has<Possesses>(seat));
    CHECK_FALSE(serverScene->Has<SeatInput>(seat));
    const Authority& authority = serverScene->Get<Authority>(seat);
    CHECK(authority.Tier == Tier::Server);
    CHECK(authority.Owner == id);
}

TEST_CASE("A client-mode level load skips server-authoritative authored entities")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);

    // Three authored entities: a Server-tier one (explicit), a Local-tier one, and one with no
    // Authority at all (the authored default is Server). Only the Local-tier entity survives a skip.
    const auto makeEntity = [&](optional<Tier> tier, const Name& name)
    {
        Prefab::PrefabEntity entity;
        entity.Components.push_back(MakeComponent(types, name));
        if (tier.has_value())
        {
            entity.Components.push_back(MakeComponent(types, Authority{.Tier = *tier}));
        }
        return entity;
    };

    vector<Prefab::PrefabEntity> entities;
    entities.push_back(makeEntity(Tier::Server, Name{.Value = "server"}));
    entities.push_back(makeEntity(Tier::Local, Name{.Value = "local"}));
    entities.push_back(makeEntity(std::nullopt, Name{.Value = "default"}));
    const Ref<Prefab> prefab = Prefab::Create(std::move(entities), {});

    // Full load: every authored entity spawns.
    Unique<Scene> full = Scene::Create(types);
    CHECK(prefab->SpawnInto(*full, FakeAssets()).Roots.size() == 3);

    // Client-mode load: the two server-authoritative entities are skipped; only the Local one spawns.
    Unique<Scene> client = Scene::Create(types);
    const Prefab::SpawnResult skipped = prefab->SpawnInto(
        *client, FakeAssets(), Prefab::SpawnOptions{.SkipServerAuthoritative = true});
    CHECK(skipped.Roots.size() == 1);

    u32 localCount = 0;
    u32 total = 0;
    for (auto [entity, name] : client->View<Name>())
    {
        ++total;
        if (name.Value == "local")
        {
            ++localCount;
        }
    }
    CHECK(total == 1);
    CHECK(localCount == 1);
}

TEST_CASE("Full join: readiness gates the stream, seat + pawn arrive, possession wires the camera")
{
    auto [serverT, clientT] = LoopbackTransport::CreatePair();

    TypeRegistry serverTypes;
    RegisterBuiltinTypes(serverTypes);
    Unique<Scene> serverScene = Scene::Create(serverTypes);

    const AssetId levelId{0x00000000000000AAULL};

    Result<Unique<ServerHost>> host = ServerHost::Create(ServerHostInfo{
        .Server = ServerInfo{.TransportOverride = serverT.get(), .Connection = Config},
        .World = *serverScene,
        .Assets = FakeAssets(),
        .LevelId = levelId,
        .Replication = ReplicationServer::Settings{.SnapshotInterval = 2},
    });
    REQUIRE(host.has_value());

    Unique<Client> client =
        *Client::Connect(ClientInfo{.TransportOverride = clientT.get(), .Connection = Config});

    // The client-side level load builds a Scene carrying a Local-tier camera the join wires. The
    // server-authoritative entities are absent — they arrive from the stream. The caller owns the
    // scene (the host borrows it); the test reaches it through clientHost->World().
    TypeRegistry clientTypes;
    RegisterBuiltinTypes(clientTypes);
    Unique<Scene> clientScene;
    Entity localCamera = Entity::Null;
    Entity wiredPawn = Entity::Null;
    u32 possessionCalls = 0;

    Unique<ClientHost> clientHost = ClientHost::Create(ClientHostInfo{
        .Client = *client,
        .Assets = FakeAssets(),
        .LoadLevel = [&](AssetId requested) -> Scene*
        {
            CHECK(requested.Value == levelId.Value);
            clientScene = Scene::Create(clientTypes);
            const Entity camera = clientScene->CreateEntity();
            clientScene->Add<Transform>(camera);
            clientScene->Add<Camera>(camera);
            clientScene->Add<Authority>(camera, Authority{.Tier = Tier::Local});
            clientScene->Add<CameraFollow>(camera);
            localCamera = camera;
            return clientScene.get();
        },
        .ResolvePrefab = [](AssetId) -> Ref<Prefab> { return nullptr; },
        .OnPossession =
            [&](Scene& world, Entity pawn)
        {
            ++possessionCalls;
            wiredPawn = pawn;
            if (!localCamera.IsNull() && world.IsAlive(localCamera))
            {
                world.Get<CameraFollow>(localCamera).Target = pawn;
            }
        },
    });

    Entity serverPawn = Entity::Null;
    NetId pawnNetId = InvalidNetId;

    f64 now = 0.0;
    for (u64 tick = 1; tick <= 40; ++tick)
    {
        now += 1.0 / 60.0;
        serverScene->SetChangeTick(tick);

        // The game-mode spawn rule (emulated): once the seat exists, pawn it — spawn a pawn and set
        // the seat's Possesses, copying the seat's owner. It needs no net awareness.
        if (!(*host)->Server().Connections().empty() && serverPawn.IsNull())
        {
            const ConnectionId id = (*host)->Server().Connections().front();
            const Entity seat = (*host)->SeatFor(id);
            if (!seat.IsNull())
            {
                serverPawn = serverScene->CreateEntity();
                serverScene->Add<Transform>(serverPawn,
                                            Transform{.Position = vec3(1.0f, 2.0f, 3.0f)});
                serverScene->Add<Authority>(serverPawn,
                                            Authority{.Tier = Tier::Server, .Owner = id});
                serverScene->Get<Possesses>(seat).Pawn = serverPawn;
            }
        }

        (*host)->Pump(now, tick);
        clientHost->Pump(now);

        if (!serverPawn.IsNull())
        {
            pawnNetId = serverScene->Get<NetIdentity>(serverPawn).Id;
        }
    }

    REQUIRE(client->State() == ClientState::Connected);
    REQUIRE(clientHost->IsJoined());
    REQUIRE(clientHost->World() != nullptr);
    Scene& world = *clientHost->World();

    // The own seat bound: a replicated Viewer marked Remote, carrying Possesses.
    const Entity seat = clientHost->Seat();
    REQUIRE_FALSE(seat.IsNull());
    CHECK(world.Has<Viewer>(seat));
    CHECK(world.Get<Authority>(seat).Tier == Tier::Remote);

    // The pawn arrived exactly once and the seat's possession resolved to it locally.
    REQUIRE(pawnNetId != InvalidNetId);
    const Entity clientPawn = clientHost->Replication().Map().Lookup(pawnNetId);
    REQUIRE_FALSE(clientPawn.IsNull());
    CHECK(world.Get<Possesses>(seat).Pawn == clientPawn);

    // The possession wired the Local-tier camera to the replicated pawn.
    CHECK(possessionCalls >= 1);
    CHECK(wiredPawn == clientPawn);
    CHECK(clientHost->PossessedPawn() == clientPawn);
    REQUIRE_FALSE(localCamera.IsNull());
    CHECK(world.Get<CameraFollow>(localCamera).Target == clientPawn);
    // The local camera is client-local, never a replicated mirror.
    CHECK(world.Get<Authority>(localCamera).Tier == Tier::Local);
}

TEST_CASE("Disconnect tears the seat down and surfaces the event")
{
    auto [serverT, clientT] = LoopbackTransport::CreatePair();

    TypeRegistry serverTypes;
    RegisterBuiltinTypes(serverTypes);
    Unique<Scene> serverScene = Scene::Create(serverTypes);

    Result<Unique<ServerHost>> host = ServerHost::Create(ServerHostInfo{
        .Server = ServerInfo{.TransportOverride = serverT.get(), .Connection = Config},
        .World = *serverScene,
        .Assets = FakeAssets(),
        .LevelId = AssetId{0x0000000000000001ULL},
    });
    REQUIRE(host.has_value());

    Unique<Client> client =
        *Client::Connect(ClientInfo{.TransportOverride = clientT.get(), .Connection = Config});

    // A ClientHost auto-joins the default world so a seat is spawned to tear down.
    TypeRegistry clientTypes;
    RegisterBuiltinTypes(clientTypes);
    Unique<Scene> clientScene;
    Unique<ClientHost> clientHost = ClientHost::Create(ClientHostInfo{
        .Client = *client,
        .Assets = FakeAssets(),
        .LoadLevel = [&](AssetId) -> Scene*
        {
            clientScene = Scene::Create(clientTypes);
            return clientScene.get();
        },
        .ResolvePrefab = [](AssetId) -> Ref<Prefab> { return nullptr; },
    });

    f64 now = 0.0;
    for (u64 tick = 1; tick <= 12 && !clientHost->IsJoined(); ++tick)
    {
        now += 1.0 / 60.0;
        serverScene->SetChangeTick(tick);
        (*host)->Pump(now, tick);
        clientHost->Pump(now);
    }
    REQUIRE(client->State() == ClientState::Connected);
    REQUIRE(clientHost->IsJoined());

    const ConnectionId id = client->AssignedId();
    const Entity seat = (*host)->SeatFor(id);
    REQUIRE_FALSE(seat.IsNull());
    REQUIRE(serverScene->IsAlive(seat));

    // The client leaves gracefully; the server surfaces a Disconnected event and tears the seat down.
    client->Disconnect();

    bool sawDisconnect = false;
    for (u64 tick = 13; tick <= 30; ++tick)
    {
        now += 1.0 / 60.0;
        serverScene->SetChangeTick(tick);
        client->Pump(now);
        (*host)->Pump(now, tick);
        for (const NetEvent& event : (*host)->Events())
        {
            if (event.Type == NetEventType::Disconnected && event.Id == id)
            {
                sawDisconnect = true;
            }
        }
        if (sawDisconnect)
        {
            break;
        }
    }

    CHECK(sawDisconnect);
    CHECK_FALSE(serverScene->IsAlive(seat));
    CHECK((*host)->SeatFor(id).IsNull());
}
