// Locally-created host entities on the wire: spawn-time prefab provenance, the opt-in replication
// marker that turns it into an engine-driven prefab association, and the change-tick floor a
// replicated component has to clear to reach a joiner at all. Every case is device-free — two
// in-process scenes over a LoopbackTransport with time and ticks injected, the
// net_directory_resolution.cpp pattern — plus the provenance unit cases over a bare scene.

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
                HostScene->SetChangeTick(Tick);
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

// ---- Step 0: the observed ghost, reproduced and named -------------------------------------------

TEST_CASE("A replicated component stamped before the world's first tick never reaches a joiner")
{
    Peers fx;

    // A host entity created and populated while the scene's change tick is still zero — the level
    // load / pre-tick window. The component is written once and never again.
    const Entity ghost = fx.HostScene->CreateEntity();
    fx.HostScene->Add<Transform>(ghost, Transform{.Position = vec3(4.0f, 0.0f, 0.0f)});
    fx.HostScene->Add<VengTest::TestScore>(ghost, VengTest::TestScore{.Value = 7});
    REQUIRE(fx.HostScene->GetComponentChangeTick(ghost, TypeIdOf<VengTest::TestScore>()) == 0);

    fx.Join();
    fx.Step(20);

    // The entity itself arrives — AssignServerNetIds identified it and the spawn went out — but it
    // arrives empty: the spawn's payload and every snapshot gate on a change tick strictly greater
    // than the baseline, and both baselines start at zero, so a component stamped at tick zero is
    // indistinguishable from an unstamped one and is never selected.
    const Entity mirror = fx.MirrorOf(ghost);
    REQUIRE_FALSE(mirror.IsNull());
    CHECK_FALSE(fx.ClientWorld().Has<VengTest::TestScore>(mirror));
    CHECK_FALSE(fx.ClientWorld().Has<Transform>(mirror));
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

    // Spawned into a world that replicates but has no joins yet, and stamped past the change-tick
    // floor so its replicated leaf is selectable.
    fx.HostScene->SetChangeTick(1);
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
