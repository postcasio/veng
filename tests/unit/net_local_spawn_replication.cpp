// Locally-created host entities on the wire: spawn-time prefab provenance, the opt-in replication
// marker that turns it into an engine-driven prefab association, and the spawn payload's full-state
// contract — every replicated component the entity carries reaches a joiner, whatever tick each was
// stamped at. Every case is device-free — two in-process scenes over a LoopbackTransport with time
// and ticks injected, the net_directory_resolution.cpp pattern — plus the provenance unit cases and
// the direct-codec spawn cases over bare scenes.

#include <doctest/doctest.h>

#include <Veng/Asset/Prefab.h>
#include <Veng/Net/Client.h>
#include <Veng/Net/Host.h>
#include <Veng/Net/LoopbackTransport.h>
#include <Veng/Net/Replication.h>
#include <Veng/Net/Server.h>
#include <Veng/Net/WorldKey.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

#include "support/TestComponents.h"

#include <utility>

using namespace Veng;
using namespace Veng::Net;

namespace
{
    // A dependency-free prefab spawn never touches the manager, so a never-dereferenced reference is
    // safe (the net_join_flow.cpp precedent).
    AssetManager& FakeAssets()
    {
        alignas(16) static unsigned char bytes[64]{};
        return *reinterpret_cast<AssetManager*>(bytes);
    }

    const ConnectionConfig Config{
        .ResendInterval = 0.02, .KeepaliveInterval = 0.05, .TimeoutInterval = 5.0};

    constexpr AssetId HostLevel{0x00000000000000B1ULL};
    constexpr AssetId PawnPrefabId{0x00000000000000B2ULL};
    constexpr WorldInstanceId HostWorld{.Value = 1};

    vector<u8> ComponentRecord(const TypeRegistry& registry, TypeId id, const void* value)
    {
        vector<u8> out;
        WriteFields(out, value, registry.Info(id), registry);
        return out;
    }

    // A one-entity prefab carrying a replicated leaf (Transform) and a non-replicated one (Name).
    // Name is the discriminator every prefab-association case asserts on: it can only reach a
    // joiner by the joiner instantiating the prefab, never through the spawn's component payload.
    Ref<Prefab> MakePawnPrefab(const TypeRegistry& registry, const AssetId source)
    {
        const Transform pose{.Position = vec3(-1.0f, -1.0f, -1.0f)};
        const Name label{.Value = "pawn"};

        Prefab::PrefabEntity entity;
        entity.Components.push_back(
            Prefab::Component{.Type = TypeIdOf<Transform>(),
                              .Record = ComponentRecord(registry, TypeIdOf<Transform>(), &pose)});
        entity.Components.push_back(
            Prefab::Component{.Type = TypeIdOf<Name>(),
                              .Record = ComponentRecord(registry, TypeIdOf<Name>(), &label)});

        vector<Prefab::PrefabEntity> entities;
        entities.push_back(std::move(entity));
        return Prefab::Create(std::move(entities), {}, source);
    }

    // A listen host and one joining client over a LoopbackTransport. The host owns a single world
    // the client auto-joins by DefaultWorldKey; the client opens a fresh scene per join and resolves
    // exactly the pawn prefab. Every scene is owned here and outlives both hosts.
    struct Peers
    {
        TypeRegistry ServerTypes;
        TypeRegistry ClientTypes;
        Unique<Scene> HostScene;
        vector<Unique<Scene>> ClientScenes;
        Ref<Prefab> Pawn;

        Unique<LoopbackTransport> ServerTransport;
        Unique<LoopbackTransport> ClientTransport;
        Unique<ServerHost> Host;
        Unique<Client> Connection;
        Unique<ClientHost> Joiner;

        f64 Now = 0.0;
        u64 Tick = 0;
        // Whether Step advances the host world's change tick, the way a world drive would. Clearing
        // it leaves the world at its change-tick floor forever while time and host ticks still
        // advance — the shape a consumer that populates its world and never mutates it again hits.
        bool DriveChangeTick = true;

        Peers()
        {
            RegisterBuiltinTypes(ServerTypes);
            ServerTypes.Register<VengTest::TestScore>();
            RegisterBuiltinTypes(ClientTypes);
            ClientTypes.Register<VengTest::TestScore>();
            HostScene = Scene::Create(ServerTypes);
            Pawn = MakePawnPrefab(ServerTypes, PawnPrefabId);

            std::pair<Unique<LoopbackTransport>, Unique<LoopbackTransport>> pair =
                LoopbackTransport::CreatePair();
            ServerTransport = std::move(pair.first);
            ClientTransport = std::move(pair.second);

            Result<Unique<ServerHost>> host = ServerHost::Create(ServerHostInfo{
                .Server =
                    ServerInfo{.TransportOverride = ServerTransport.get(), .Connection = Config},
                .WorldId = HostWorld,
                .World = *HostScene,
                .Assets = FakeAssets(),
                .LevelId = HostLevel,
            });
            REQUIRE(host.has_value());
            Host = std::move(*host);

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
                .ResolvePrefab = [this](AssetId id) -> Ref<Prefab>
                { return id == PawnPrefabId ? Pawn : nullptr; },
            });
        }

        void Step(const u32 ticks)
        {
            for (u32 i = 0; i < ticks; ++i)
            {
                Tick += 1;
                Now += 1.0 / 60.0;
                if (DriveChangeTick)
                {
                    HostScene->SetChangeTick(Tick);
                }
                Host->Pump(Now, Tick);
                Joiner->Pump(Now);
            }
        }

        // Runs the connect + auto-join to completion.
        void Join()
        {
            for (u32 tick = 0; tick < 60 && !Joiner->IsJoined(); ++tick)
            {
                Step(1);
            }
            REQUIRE(Joiner->IsJoined());
        }

        // The client's mirror of a host entity, or Entity::Null while none has arrived.
        [[nodiscard]] Entity MirrorOf(const Entity hostEntity) const
        {
            const auto* identity = HostScene->TryGet<NetIdentity>(hostEntity);
            if (identity == nullptr || identity->Id == InvalidNetId)
            {
                return Entity::Null;
            }
            return Joiner->Replication().Map().Lookup(identity->Id);
        }

        [[nodiscard]] Scene& ClientWorld() const { return *Joiner->World(); }
    };
}

// ---- The pre-tick population reaches the joiner -------------------------------------------------

TEST_CASE("A replicated component stamped before the world's first tick reaches a joiner")
{
    Peers fx;

    // A host entity created and populated before the world drive has stamped a tick — the level
    // load / pre-tick window. The component is written once and never again, at the scene's floor.
    const Entity ghost = fx.HostScene->CreateEntity();
    fx.HostScene->Add<Transform>(ghost, Transform{.Position = vec3(4.0f, 0.0f, 0.0f)});
    fx.HostScene->Add<VengTest::TestScore>(ghost, VengTest::TestScore{.Value = 7});
    REQUIRE(fx.HostScene->GetComponentChangeTick(ghost, TypeIdOf<VengTest::TestScore>()) ==
            Scene::MinChangeTick);

    fx.Join();
    fx.Step(20);

    // The spawn is a full-state operation, so it carries every replicated component the entity holds
    // regardless of the tick each was stamped at — the joiner is whole without waiting a snapshot.
    const Entity mirror = fx.MirrorOf(ghost);
    REQUIRE_FALSE(mirror.IsNull());
    REQUIRE(fx.ClientWorld().Has<VengTest::TestScore>(mirror));
    CHECK(fx.ClientWorld().Get<VengTest::TestScore>(mirror).Value == 7);
    CHECK(fx.ClientWorld().Has<Transform>(mirror));
}

TEST_CASE("The same component stamped once the world has ticked reaches the joiner")
{
    Peers fx;
    fx.Join();

    // Identical entity, identical single write — only the scene's change tick differs.
    const Entity live = fx.HostScene->CreateEntity();
    fx.HostScene->Add<Transform>(live, Transform{.Position = vec3(4.0f, 0.0f, 0.0f)});
    fx.HostScene->Add<VengTest::TestScore>(live, VengTest::TestScore{.Value = 7});
    REQUIRE(fx.HostScene->GetComponentChangeTick(live, TypeIdOf<VengTest::TestScore>()) > 0);

    fx.Step(20);

    const Entity mirror = fx.MirrorOf(live);
    REQUIRE_FALSE(mirror.IsNull());
    REQUIRE(fx.ClientWorld().Has<VengTest::TestScore>(mirror));
    CHECK(fx.ClientWorld().Get<VengTest::TestScore>(mirror).Value == 7);
}

TEST_CASE(
    "A world whose change tick never moves still converges a joiner, before and after connect")
{
    Peers fx;
    // The naive shape end to end: nothing in this case, and nothing in the pump, ever touches the
    // host world's change tick. Every write below therefore stamps the floor and is never restamped,
    // which is precisely the population a consumer produces by loading a level and leaving it alone.
    fx.DriveChangeTick = false;

    const Entity before = fx.HostScene->CreateEntity();
    fx.HostScene->Add<Transform>(before, Transform{.Position = vec3(1.0f, 0.0f, 0.0f)});
    fx.HostScene->Add<VengTest::TestScore>(before, VengTest::TestScore{.Value = 3});

    fx.Join();

    // …and one more entity authored after the connection exists, so it reaches the peer through the
    // spawn path a live world uses rather than through the join's initial sweep.
    const Entity after = fx.HostScene->CreateEntity();
    fx.HostScene->Add<Transform>(after, Transform{.Position = vec3(2.0f, 0.0f, 0.0f)});
    fx.HostScene->Add<VengTest::TestScore>(after, VengTest::TestScore{.Value = 4});

    fx.Step(20);
    REQUIRE(fx.HostScene->GetChangeTick() == Scene::MinChangeTick);

    // The peer's view of both entities is field-identical to the host's.
    for (const auto& [entity, score] : {std::pair{before, 3}, std::pair{after, 4}})
    {
        const Entity mirror = fx.MirrorOf(entity);
        REQUIRE_FALSE(mirror.IsNull());
        REQUIRE(fx.ClientWorld().Has<VengTest::TestScore>(mirror));
        CHECK(fx.ClientWorld().Get<VengTest::TestScore>(mirror).Value == score);
        REQUIRE(fx.ClientWorld().Has<Transform>(mirror));
        CHECK(fx.ClientWorld().Get<Transform>(mirror).Position.x ==
              doctest::Approx(fx.HostScene->Get<Transform>(entity).Position.x));
    }
}

// ---- The spawn payload read directly off the codec ----------------------------------------------
//
// The two-peer fixture above cannot straddle the first tick within one spawn (the join itself
// advances the clock), so the mixed-population and payload-count cases drive ReplicationServer
// directly, where the scene's change tick and the moment the connection appears are both the case's
// own to place.

namespace
{
    constexpr Net::ConnectionId DirectConnection = 1;

    // A server scene with its replication server, and the client scene its stream applies into.
    struct DirectPair
    {
        TypeRegistry ServerTypes;
        TypeRegistry ClientTypes;
        Unique<Scene> Server;
        Unique<Scene> Client;
        ReplicationServer ReplServer;
        ReplicationClient ReplClient;

        explicit DirectPair(const ReplicationServer::Settings& settings = {})
            : ReplServer(settings), ReplClient([](AssetId) -> Ref<Prefab> { return nullptr; })
        {
            RegisterBuiltinTypes(ServerTypes);
            ServerTypes.Register<VengTest::TestScore>();
            RegisterBuiltinTypes(ClientTypes);
            ClientTypes.Register<VengTest::TestScore>();
            Server = Scene::Create(ServerTypes);
            Client = Scene::Create(ClientTypes);
        }

        // Identifies every server-authoritative entity and returns the connection's stream for @p tick.
        vector<ReplicationMessage> Generate(const u64 tick)
        {
            NetIdAllocator allocator;
            AssignServerNetIds(*Server, allocator);
            return ReplServer.Generate(DirectConnection, *Server, tick);
        }

        // Applies a generated stream into the client scene and returns the tick of the last snapshot
        // it carried, or zero when the stream held no snapshot at all.
        u64 Apply(const vector<ReplicationMessage>& messages)
        {
            u64 appliedTick = 0;
            for (const ReplicationMessage& message : messages)
            {
                if (message.Channel == Net::Channel::ReliableOrdered)
                {
                    ReplClient.ApplyReliable(message.Bytes, *Client, FakeAssets());
                }
                else
                {
                    appliedTick = ReplClient.ApplySnapshot(message.Bytes, *Client).ServerTick;
                }
            }
            return appliedTick;
        }
    };

    // Whether a generated stream carries any snapshot at all. Generate emits no snapshot message
    // when every relevant entity is clean, so absence is the observable form of "nothing to resend".
    bool CarriesSnapshot(const vector<ReplicationMessage>& messages)
    {
        for (const ReplicationMessage& message : messages)
        {
            if (message.Channel == Net::Channel::UnreliableSequenced)
            {
                return true;
            }
        }
        return false;
    }

    // The component count a Spawn message declares, or nothing when @p bytes is not a Spawn. Mirrors
    // the wire layout: the message id, the wire id and owner, the optional prefab id and anchor pair,
    // then the count — every multi-byte field little-endian.
    optional<u32> SpawnComponentCount(const vector<u8>& bytes)
    {
        constexpr u8 SpawnMessageId = 16;
        usize cursor = 0;
        const auto take = [&bytes, &cursor](const usize count)
        {
            cursor += count;
            return cursor <= bytes.size();
        };

        if (bytes.empty() || bytes[0] != SpawnMessageId)
        {
            return std::nullopt;
        }
        cursor = 1;
        if (!take(sizeof(u32) * 2)) // wire id, owner
        {
            return std::nullopt;
        }
        const usize prefabFlag = cursor;
        if (!take(1) || (bytes[prefabFlag] != 0 && !take(sizeof(u64))))
        {
            return std::nullopt;
        }
        const usize anchorFlag = cursor;
        if (!take(1) || (bytes[anchorFlag] != 0 && !take(sizeof(u64) * 2)))
        {
            return std::nullopt;
        }
        const usize countAt = cursor;
        if (!take(sizeof(u32)))
        {
            return std::nullopt;
        }

        u32 count = 0;
        for (u32 i = 0; i < sizeof(u32); ++i)
        {
            count |= static_cast<u32>(bytes[countAt + i]) << (8 * i);
        }
        return count;
    }
}

TEST_CASE("An entity mixing pre-tick and post-tick components arrives whole")
{
    DirectPair fx;

    // Transform is written during the pre-tick population window; TestScore once the world drive has
    // stamped a real tick. The two straddle the boundary on one entity.
    const Entity entity = fx.Server->CreateEntity();
    fx.Server->Add<Transform>(entity, Transform{.Position = vec3(2.0f, 3.0f, 4.0f)});
    REQUIRE(fx.Server->GetComponentChangeTick(entity, TypeIdOf<Transform>()) ==
            Scene::MinChangeTick);

    fx.Server->SetChangeTick(7);
    fx.Server->Add<VengTest::TestScore>(entity, VengTest::TestScore{.Value = 11});
    REQUIRE(fx.Server->GetComponentChangeTick(entity, TypeIdOf<VengTest::TestScore>()) == 7);

    // The connection appears after both writes, so one spawn record carries the whole entity.
    fx.ReplServer.AddConnection(DirectConnection);
    for (const ReplicationMessage& message : fx.Generate(7))
    {
        if (message.Channel == Net::Channel::ReliableOrdered)
        {
            fx.ReplClient.ApplyReliable(message.Bytes, *fx.Client, FakeAssets());
        }
    }

    const Entity mirror = fx.ReplClient.Map().Lookup(fx.Server->Get<NetIdentity>(entity).Id);
    REQUIRE_FALSE(mirror.IsNull());
    REQUIRE(fx.Client->Has<VengTest::TestScore>(mirror));
    CHECK(fx.Client->Get<VengTest::TestScore>(mirror).Value == 11);
    REQUIRE(fx.Client->Has<Transform>(mirror));
    CHECK(fx.Client->Get<Transform>(mirror).Position.x == doctest::Approx(2.0f));
}

TEST_CASE("A spawn record's component count is the entity's replicated component count")
{
    DirectPair fx;

    // Two replicated components and one that is not: Name never rides the payload, so the declared
    // count is exactly two whether or not any tick has been stamped.
    const Entity entity = fx.Server->CreateEntity();
    fx.Server->Add<Transform>(entity);
    fx.Server->Add<VengTest::TestScore>(entity, VengTest::TestScore{.Value = 3});
    fx.Server->Add<Name>(entity, Name{.Value = "local"});

    fx.ReplServer.AddConnection(DirectConnection);

    optional<u32> declared;
    for (const ReplicationMessage& message : fx.Generate(0))
    {
        if (const optional<u32> count = SpawnComponentCount(message.Bytes); count.has_value())
        {
            REQUIRE_FALSE(declared.has_value()); // exactly one entity is replicated here
            declared = count;
        }
    }

    REQUIRE(declared.has_value());
    CHECK(*declared == 2);
}

// ---- The snapshot arm: a pre-tick write onto an entity the peer already holds --------------------

TEST_CASE("A pre-tick component added to an already-spawned entity rides the next snapshot")
{
    DirectPair fx;

    // The entity is spawned on the peer during the pre-tick window, so the spawn record is the only
    // thing the connection has ever seen, and it has acked nothing.
    const Entity entity = fx.Server->CreateEntity();
    fx.Server->Add<Transform>(entity, Transform{.Position = vec3(1.0f, 0.0f, 0.0f)});

    fx.ReplServer.AddConnection(DirectConnection);
    fx.Apply(fx.Generate(0));

    const Entity mirror = fx.ReplClient.Map().Lookup(fx.Server->Get<NetIdentity>(entity).Id);
    REQUIRE_FALSE(mirror.IsNull());
    REQUIRE_FALSE(fx.Client->Has<VengTest::TestScore>(mirror));

    // A second write, still with no tick stepped. It stamps the scene's change tick, which floors at
    // one, so it is strictly newer than the connection's zero baseline and enters the snapshot.
    fx.Server->Add<VengTest::TestScore>(entity, VengTest::TestScore{.Value = 5});
    fx.Apply(fx.Generate(2));

    REQUIRE(fx.Client->Has<VengTest::TestScore>(mirror));
    CHECK(fx.Client->Get<VengTest::TestScore>(mirror).Value == 5);
}

TEST_CASE("Change tick zero is reserved for a component that was never stamped")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    Unique<Scene> scene = Scene::Create(types);
    const Entity entity = scene->CreateEntity();

    // The entity does not carry Transform, so the query reports the reserved before-any-tick value.
    CHECK(scene->GetComponentChangeTick(entity, TypeIdOf<Transform>()) == 0);

    // A write, in contrast, can never produce it: a fresh scene already stamps the floor.
    CHECK(scene->GetChangeTick() > 0);
    scene->Add<Transform>(entity);
    CHECK(scene->GetComponentChangeTick(entity, TypeIdOf<Transform>()) > 0);
}

// ---- The steady-state negatives -----------------------------------------------------------------
//
// The change-tick floor lifts every write above a fresh baseline, so the way it fails is by lifting
// too much: a component that has been acked resending forever, or a keyframe re-admitting what the
// dirty gate excluded. Both cases below populate pre-tick — the floor's own path — and assert the
// stream falls silent, which is the observable form of "nothing to resend". Delta, interest,
// prediction and reconciliation are covered by their own suites passing unchanged.

TEST_CASE("An acked component is not resent on the following snapshot")
{
    DirectPair fx;

    const Entity entity = fx.Server->CreateEntity();
    fx.Server->Add<Transform>(entity, Transform{.Position = vec3(1.0f, 2.0f, 3.0f)});

    fx.ReplServer.AddConnection(DirectConnection);
    const u64 applied = fx.Apply(fx.Generate(2));
    REQUIRE(applied == 2);
    fx.ReplServer.Acknowledge(DirectConnection, applied);

    // Nothing has been written since the ack, so the delta gate holds the component back and the
    // stream carries no snapshot at all.
    CHECK_FALSE(CarriesSnapshot(fx.Generate(4)));
    CHECK_FALSE(CarriesSnapshot(fx.Generate(6)));
}

TEST_CASE("A keyframe forces a full encoding only for components that are dirty")
{
    DirectPair fx{ReplicationServer::Settings{.SnapshotInterval = 1, .KeyframeInterval = 1}};

    const Entity entity = fx.Server->CreateEntity();
    fx.Server->Add<Transform>(entity, Transform{.Position = vec3(1.0f, 2.0f, 3.0f)});

    fx.ReplServer.AddConnection(DirectConnection);

    // Every snapshot here is a keyframe. Unacked, the component is dirty and rides one.
    REQUIRE(CarriesSnapshot(fx.Generate(1)));
    const u64 applied = fx.Apply(fx.Generate(2));
    REQUIRE(applied == 2);
    fx.ReplServer.Acknowledge(DirectConnection, applied);

    // Acked and untouched, it is clean — the keyframe cadence re-bases what is dirty, it does not
    // re-admit what the gate excluded.
    CHECK_FALSE(CarriesSnapshot(fx.Generate(3)));
}

// ---- Spawn provenance ---------------------------------------------------------------------------

TEST_CASE("SpawnInto stamps each spawned root with the prefab it came from")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    Unique<Scene> scene = Scene::Create(types);

    const Ref<Prefab> prefab = MakePawnPrefab(types, PawnPrefabId);
    const Prefab::SpawnResult spawned = prefab->SpawnInto(*scene, FakeAssets());
    REQUIRE(spawned.Roots.size() == 1);

    REQUIRE(scene->Has<PrefabSource>(spawned.Roots.front()));
    CHECK(scene->Get<PrefabSource>(spawned.Roots.front()).Prefab == PawnPrefabId);
}

TEST_CASE("A prefab with no source id stamps no provenance")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    Unique<Scene> scene = Scene::Create(types);

    // A runtime-built prefab is not AssetId-addressable, so there is no provenance to record.
    const Ref<Prefab> prefab = MakePawnPrefab(types, AssetId{});
    const Prefab::SpawnResult spawned = prefab->SpawnInto(*scene, FakeAssets());
    REQUIRE(spawned.Roots.size() == 1);
    CHECK_FALSE(scene->Has<PrefabSource>(spawned.Roots.front()));
}

// ---- The engine-driven prefab association -------------------------------------------------------

TEST_CASE("A marked locally-spawned host entity reaches a joiner as a real prefab spawn")
{
    Peers fx;
    fx.Join();

    const Prefab::SpawnResult spawned = fx.Pawn->SpawnInto(*fx.HostScene, FakeAssets());
    REQUIRE(spawned.Roots.size() == 1);
    const Entity pawn = spawned.Roots.front();
    fx.HostScene->Add<NetSpawn>(pawn);
    fx.HostScene->Get<Transform>(pawn).Position = vec3(3.0f, 0.0f, 0.0f);

    fx.Step(20);

    const Entity mirror = fx.MirrorOf(pawn);
    REQUIRE_FALSE(mirror.IsNull());
    // The joiner instantiated the prefab, so it holds the prefab's non-replicated structure…
    REQUIRE(fx.ClientWorld().Has<Name>(mirror));
    CHECK(fx.ClientWorld().Get<Name>(mirror).Value == "pawn");
    // …and the spawn's replicated state still overwrote the prefab's authored default.
    REQUIRE(fx.ClientWorld().Has<Transform>(mirror));
    CHECK(fx.ClientWorld().Get<Transform>(mirror).Position.x == doctest::Approx(3.0f));
    CHECK(fx.ClientWorld().Get<Authority>(mirror).Tier == Tier::Remote);
}

TEST_CASE("A marked entity spawned before the join is picked up when the client arrives")
{
    Peers fx;

    // Spawned into a world that replicates but has no joins yet, with no tick stepped — so this now
    // covers the pre-tick population path rather than one lifted above it by hand.
    const Prefab::SpawnResult spawned = fx.Pawn->SpawnInto(*fx.HostScene, FakeAssets());
    const Entity pawn = spawned.Roots.front();
    fx.HostScene->Add<NetSpawn>(pawn);
    fx.HostScene->Get<Transform>(pawn).Position = vec3(5.0f, 0.0f, 0.0f);

    fx.Join();
    fx.Step(20);

    const Entity mirror = fx.MirrorOf(pawn);
    REQUIRE_FALSE(mirror.IsNull());
    REQUIRE(fx.ClientWorld().Has<Name>(mirror));
    CHECK(fx.ClientWorld().Get<Transform>(mirror).Position.x == doctest::Approx(5.0f));
}

TEST_CASE("An unmarked provenance-carrying host entity replicates exactly as it did")
{
    Peers fx;
    fx.Join();

    const Prefab::SpawnResult spawned = fx.Pawn->SpawnInto(*fx.HostScene, FakeAssets());
    const Entity prop = spawned.Roots.front();
    REQUIRE(fx.HostScene->Has<PrefabSource>(prop));
    fx.HostScene->Get<Transform>(prop).Position = vec3(8.0f, 0.0f, 0.0f);

    fx.Step(20);

    // Provenance alone changes nothing: the spawn still rides the runtime-constructed arm, so the
    // joiner gets the replicated leaves and none of the prefab's structure.
    const Entity mirror = fx.MirrorOf(prop);
    REQUIRE_FALSE(mirror.IsNull());
    CHECK_FALSE(fx.ClientWorld().Has<Name>(mirror));
    REQUIRE(fx.ClientWorld().Has<Transform>(mirror));
    CHECK(fx.ClientWorld().Get<Transform>(mirror).Position.x == doctest::Approx(8.0f));
}

TEST_CASE("A marked non-authoritative entity is still never replicated")
{
    Peers fx;
    fx.Join();

    const Prefab::SpawnResult spawned = fx.Pawn->SpawnInto(*fx.HostScene, FakeAssets());
    const Entity local = spawned.Roots.front();
    fx.HostScene->Add<Authority>(local, Authority{.Tier = Tier::Local});
    fx.HostScene->Add<NetSpawn>(local);

    fx.Step(20);

    // The marker names how an entity replicates, never whether: authority still decides that.
    CHECK_FALSE(fx.HostScene->Has<NetIdentity>(local));
    CHECK(fx.MirrorOf(local).IsNull());
}

TEST_CASE("Destroying a marked entity tears its mirror down and leaves no dangling id")
{
    Peers fx;
    fx.Join();

    const Prefab::SpawnResult spawned = fx.Pawn->SpawnInto(*fx.HostScene, FakeAssets());
    const Entity pawn = spawned.Roots.front();
    fx.HostScene->Add<NetSpawn>(pawn);
    fx.Step(20);

    const NetId id = fx.HostScene->Get<NetIdentity>(pawn).Id;
    const Entity mirror = fx.MirrorOf(pawn);
    REQUIRE_FALSE(mirror.IsNull());

    fx.HostScene->DestroyEntity(pawn);
    fx.Step(20);

    CHECK(fx.Joiner->Replication().Map().Lookup(id).IsNull());
    CHECK_FALSE(fx.ClientWorld().IsAlive(mirror));
}
