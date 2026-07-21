// World-directory resolution: the key→live-instance reads and the host-side resolves built on the
// same state. Three bands, all device-free: the directory's own InstancesOf over a bare
// get-or-place fixture (a hosted key resolves to its bucket, a capped key resolves to *both* its
// live buckets, and a reaped bucket stops resolving); ClientHost::JoinForKey over a real join flow
// (a client finds its own join by key and misses a key it never joined); and
// ServerHost::IsReplicatingWorld across a world's whole life (hosted worlds replicate, an unknown
// id does not, and a factory-opened world stops replicating once its dwell expires and the
// directory reaps it). The host band runs two in-process scenes over a LoopbackTransport with time
// and ticks injected, the net_join_flow.cpp pattern.

#include <doctest/doctest.h>

#include <Veng/Net/Client.h>
#include <Veng/Net/Host.h>
#include <Veng/Net/LoopbackTransport.h>
#include <Veng/Net/Server.h>
#include <Veng/Net/WorldKey.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Scene.h>
#include <Veng/WorldDirectory.h>

#include <utility>

using namespace Veng;
using namespace Veng::Net;

namespace
{
    // A dependency-free seat spawn never touches the manager, so a never-dereferenced reference is
    // safe (the net_join_flow.cpp precedent).
    AssetManager& FakeAssets()
    {
        alignas(16) static unsigned char bytes[64]{};
        return *reinterpret_cast<AssetManager*>(bytes);
    }

    const ConnectionConfig Config{
        .ResendInterval = 0.02, .KeepaliveInterval = 0.05, .TimeoutInterval = 5.0};

    // ---- The directory band ---------------------------------------------------------------------

    // A directory whose factory opens one fresh bucket per miss over a shared bare scene (the
    // directory never dereferences a bucket's Scene) and whose reaps route through the CloseWorld
    // hook. The per-instance cap is settable, so a case can drive a key to hold several buckets.
    struct DirectoryFixture
    {
        TypeRegistry Types;
        Unique<Scene> WorldScene = Scene::Create(Types);
        u64 NextWorld = 1;
        Unique<WorldDirectory> Directory;

        explicit DirectoryFixture(const u32 maxPerInstance = 0, const f64 dwell = 5.0)
        {
            Directory = WorldDirectory::Create(WorldDirectoryInfo{
                .MaxPlayersPerInstance = maxPerInstance,
                .IdleKeepWarmDwell = dwell,
                .WorldFactory = [this](const JoinRequestInfo&, const WorldKey&,
                                       const Blob&) -> optional<ServerWorldResolution>
                {
                    return ServerWorldResolution{.WorldId = WorldInstanceId{.Value = NextWorld++},
                                                 .World = WorldScene.get()};
                },
            });
        }

        // Resolves a key with the given payload; the payload outlives the synchronous call.
        WorldResolveResult Resolve(const WorldKey& key, const Blob& payload = {})
        {
            return Directory->Resolve(JoinRequestInfo{.Connection = ConnectionId{},
                                                      .Account = AccountId{},
                                                      .Key = key,
                                                      .Payload = payload},
                                      /*heldWorlds=*/0);
        }
    };

    // ---- The host band --------------------------------------------------------------------------

    constexpr AssetId PrimaryLevel{0x00000000000000A1ULL};
    constexpr AssetId DataLevel{0x00000000000000A2ULL};
    constexpr AssetId FactoryLevel{0x00000000000000A3ULL};

    const WorldKey DataKey = WorldKey::FromU64(0xDA7A);
    const WorldKey FactoryKey = WorldKey::FromU64(0xF00D);
    const WorldKey NeverJoinedKey = WorldKey::FromU64(0xBEEF);

    constexpr WorldInstanceId PrimaryWorld{.Value = 1};
    constexpr WorldInstanceId DataWorld{.Value = 2};
    constexpr WorldInstanceId FactoryWorld{.Value = 3};

    // A server host and a client host over one LoopbackTransport, stepped tick by tick with time
    // injected. The server hosts a primary world (joined by the client's auto-join), a
    // pre-registered data world, and a factory that opens one further world on a miss; the client
    // opens a fresh scene per join. Every scene is owned here and outlives both hosts.
    struct HostPair
    {
        TypeRegistry ServerTypes;
        TypeRegistry ClientTypes;
        Unique<Scene> PrimaryScene;
        Unique<Scene> DataScene;
        Unique<Scene> FactoryScene;
        vector<Unique<Scene>> ClientScenes;

        Unique<LoopbackTransport> ServerTransport;
        Unique<LoopbackTransport> ClientTransport;
        Unique<ServerHost> Host;
        Unique<Client> Connection;
        Unique<ClientHost> Joiner;

        f64 Now = 0.0;
        u64 Tick = 0;

        explicit HostPair(const f64 dwell = 5.0)
        {
            RegisterBuiltinTypes(ServerTypes);
            RegisterBuiltinTypes(ClientTypes);
            PrimaryScene = Scene::Create(ServerTypes);
            DataScene = Scene::Create(ServerTypes);
            FactoryScene = Scene::Create(ServerTypes);

            std::pair<Unique<LoopbackTransport>, Unique<LoopbackTransport>> pair =
                LoopbackTransport::CreatePair();
            ServerTransport = std::move(pair.first);
            ClientTransport = std::move(pair.second);

            Result<Unique<ServerHost>> host = ServerHost::Create(ServerHostInfo{
                .Server =
                    ServerInfo{.TransportOverride = ServerTransport.get(), .Connection = Config},
                .WorldId = PrimaryWorld,
                .World = *PrimaryScene,
                .Assets = FakeAssets(),
                .LevelId = PrimaryLevel,
                .IdleKeepWarmDwell = dwell,
                .WorldFactory = [this](const JoinRequestInfo&, const WorldKey&,
                                       const Blob&) -> optional<ServerWorldResolution>
                {
                    return ServerWorldResolution{.WorldId = FactoryWorld,
                                                 .World = FactoryScene.get(),
                                                 .LevelId = FactoryLevel};
                },
            });
            REQUIRE(host.has_value());
            Host = std::move(*host);
            Host->AddWorld(ServerWorldInfo{
                .WorldId = DataWorld, .Key = DataKey, .World = *DataScene, .LevelId = DataLevel});

            Connection = *Client::Connect(
                ClientInfo{.TransportOverride = ClientTransport.get(), .Connection = Config});
            Joiner = ClientHost::Create(ClientHostInfo{
                .Client = *Connection,
                .Assets = FakeAssets(),
                .LoadLevel = [this](AssetId) -> Scene*
                {
                    ClientScenes.push_back(Scene::Create(ClientTypes));
                    return ClientScenes.back().get();
                },
                .ResolvePrefab = [](AssetId) -> Ref<Prefab> { return nullptr; },
            });
        }

        // Advances both ends one frame each for the given number of ticks, stamping the primary
        // world's change tick as a sim step would.
        void Step(const u32 ticks)
        {
            for (u32 i = 0; i < ticks; ++i)
            {
                Tick += 1;
                Now += 1.0 / 60.0;
                PrimaryScene->SetChangeTick(Tick);
                Host->Pump(Now, Tick);
                Joiner->Pump(Now);
            }
        }

        // Advances both ends over the given span of injected time in one-frame steps, so a case can
        // run a keep-warm dwell out without spinning thousands of ticks.
        void StepSeconds(const f64 seconds)
        {
            const u32 ticks = static_cast<u32>(seconds * 60.0) + 2;
            Step(ticks);
        }
    };
}

TEST_CASE("A hosted key resolves to its live bucket, and an unhosted key to nothing")
{
    DirectoryFixture fx;
    const WorldKey key = WorldKey::FromU64(0xA11CE);
    const Blob payload{.Bytes = {0x07}};

    const WorldResolveResult resolve = fx.Resolve(key, payload);
    REQUIRE(resolve.Outcome == WorldResolveOutcome::Opened);
    fx.Directory->AddJoin(resolve.World);

    const std::span<const WorldPlacement> live = fx.Directory->InstancesOf(key);
    REQUIRE(live.size() == 1);
    CHECK(live[0].World == resolve.World);
    // The bucket is offered exactly as the placement policy sees it: presence and recorded params.
    CHECK(live[0].LiveSeats == 1);
    CHECK(live[0].Payload.Bytes == payload.Bytes);

    CHECK(fx.Directory->InstancesOf(WorldKey::FromU64(0xD00D)).empty());
}

TEST_CASE("A key holding two live buckets resolves to both")
{
    // One seat per instance, so the second resolve cannot place into the first bucket and the
    // factory opens a second under the same key — the case a singular resolve could not express.
    DirectoryFixture fx(/*maxPerInstance=*/1);
    const WorldKey key = WorldKey::FromU64(0xCAFE);

    const WorldResolveResult first = fx.Resolve(key);
    REQUIRE(first.Outcome == WorldResolveOutcome::Opened);
    fx.Directory->AddJoin(first.World);

    const WorldResolveResult second = fx.Resolve(key);
    REQUIRE(second.Outcome == WorldResolveOutcome::Opened);
    CHECK(second.World != first.World);

    const std::span<const WorldPlacement> live = fx.Directory->InstancesOf(key);
    REQUIRE(live.size() == 2);
    const bool sawFirst = live[0].World == first.World || live[1].World == first.World;
    const bool sawSecond = live[0].World == second.World || live[1].World == second.World;
    CHECK(sawFirst);
    CHECK(sawSecond);
}

TEST_CASE("A reaped bucket stops resolving, and its key with it")
{
    DirectoryFixture fx(/*maxPerInstance=*/0, /*dwell=*/1.0);
    const WorldKey key = WorldKey::FromU64(0x5EED);

    const WorldResolveResult resolve = fx.Resolve(key);
    REQUIRE(resolve.Outcome == WorldResolveOutcome::Opened);
    fx.Directory->AddJoin(resolve.World);
    REQUIRE(fx.Directory->InstancesOf(key).size() == 1);

    // Presence-less but still inside the dwell: the bucket is warm, so it still resolves.
    fx.Directory->RemoveJoin(resolve.World, /*now=*/10.0);
    CHECK(fx.Directory->InstancesOf(key).size() == 1);

    const vector<WorldInstanceId> reaped = fx.Directory->ReapIdle(/*now=*/12.0);
    REQUIRE(reaped.size() == 1);
    CHECK(reaped[0] == resolve.World);
    CHECK(fx.Directory->InstancesOf(key).empty());
}

TEST_CASE("A pre-registered bucket resolves under its key from the moment it is registered")
{
    DirectoryFixture fx;
    const WorldKey key = WorldKey::FromU64(0x1234);
    const WorldInstanceId world{.Value = 42};

    CHECK(fx.Directory->InstancesOf(key).empty());
    fx.Directory->Register(key, world, Blob{.Bytes = {0x01, 0x02}});

    const std::span<const WorldPlacement> live = fx.Directory->InstancesOf(key);
    REQUIRE(live.size() == 1);
    CHECK(live[0].World == world);
    CHECK(live[0].LiveSeats == 0);
    CHECK(live[0].Payload.Bytes == vector<u8>{0x01, 0x02});
}

TEST_CASE("A client resolves its own join by key, and misses a key it never joined")
{
    HostPair fx;

    for (u32 tick = 0; tick < 30 && !fx.Joiner->IsJoined(); ++tick)
    {
        fx.Step(1);
    }
    REQUIRE(fx.Joiner->IsJoined());

    // The auto-joined primary world resolves to the current join, and its key round-trips.
    const optional<JoinId> primaryJoin = fx.Joiner->JoinForKey(DefaultWorldKey);
    REQUIRE(primaryJoin.has_value());
    CHECK(*primaryJoin == fx.Joiner->CurrentJoinId());
    CHECK(fx.Joiner->JoinKey(*primaryJoin) == DefaultWorldKey);

    // A key this client never joined resolves to nothing, hosted or not.
    CHECK_FALSE(fx.Joiner->JoinForKey(DataKey).has_value());
    CHECK_FALSE(fx.Joiner->JoinForKey(NeverJoinedKey).has_value());

    // A second join keys independently: each key finds its own join, not merely the first one.
    fx.Joiner->Join(DataKey);
    for (u32 tick = 0; tick < 30 && fx.Joiner->Joins().size() < 2; ++tick)
    {
        fx.Step(1);
    }
    REQUIRE(fx.Joiner->Joins().size() == 2);

    const optional<JoinId> dataJoin = fx.Joiner->JoinForKey(DataKey);
    REQUIRE(dataJoin.has_value());
    CHECK(*dataJoin != *primaryJoin);
    CHECK(fx.Joiner->JoinKey(*dataJoin) == DataKey);
    CHECK(fx.Joiner->JoinForKey(DefaultWorldKey) == primaryJoin);

    // A left join takes its key with it: the client no longer holds one for it.
    fx.Joiner->Leave(*dataJoin);
    fx.Step(4);
    CHECK_FALSE(fx.Joiner->JoinForKey(DataKey).has_value());
    CHECK(fx.Joiner->JoinForKey(DefaultWorldKey) == primaryJoin);
}

TEST_CASE("The host replicates every world it holds, and no world it does not")
{
    HostPair fx;

    // Both worlds replicate from the moment they are hosted — before any client joins either.
    CHECK(fx.Host->IsReplicatingWorld(PrimaryWorld));
    CHECK(fx.Host->IsReplicatingWorld(DataWorld));
    // A world this host never opened, and the id that names no world at all.
    CHECK_FALSE(fx.Host->IsReplicatingWorld(WorldInstanceId{.Value = 99}));
    CHECK_FALSE(fx.Host->IsReplicatingWorld(WorldInstanceId{}));

    for (u32 tick = 0; tick < 30 && !fx.Joiner->IsJoined(); ++tick)
    {
        fx.Step(1);
    }
    REQUIRE(fx.Joiner->IsJoined());
    CHECK(fx.Host->IsReplicatingWorld(PrimaryWorld));
}

TEST_CASE("A factory-opened world replicates from its join and stops when the dwell reaps it")
{
    HostPair fx(/*dwell=*/0.1);

    for (u32 tick = 0; tick < 30 && !fx.Joiner->IsJoined(); ++tick)
    {
        fx.Step(1);
    }
    REQUIRE(fx.Joiner->IsJoined());
    // The factory has not run, so its world is not hosted and nothing replicates it.
    CHECK_FALSE(fx.Host->IsReplicatingWorld(FactoryWorld));

    fx.Joiner->Join(FactoryKey);
    for (u32 tick = 0; tick < 30 && !fx.Joiner->JoinForKey(FactoryKey).has_value(); ++tick)
    {
        fx.Step(1);
    }
    const optional<JoinId> factoryJoin = fx.Joiner->JoinForKey(FactoryKey);
    REQUIRE(factoryJoin.has_value());
    CHECK(fx.Host->IsReplicatingWorld(FactoryWorld));

    // The last join leaves, the dwell runs out, and the reap drops the host's replication state
    // with the directory's bucket — the predicate clears at exactly that moment.
    fx.Joiner->Leave(*factoryJoin);
    fx.StepSeconds(0.5);
    CHECK_FALSE(fx.Host->IsReplicatingWorld(FactoryWorld));
    // The pre-registered worlds are never reaped, so they replicate throughout.
    CHECK(fx.Host->IsReplicatingWorld(PrimaryWorld));
    CHECK(fx.Host->IsReplicatingWorld(DataWorld));
}
