// Interest management: the per-connection relevancy filter. The set math is pure over scripted
// candidates (enter/leave hysteresis, the dwell floor, the always-relevant and policy arms, and the
// radius-0 off switch); the scene scans gather spatial candidates and always-relevant marks; and the
// replication integration proves interest gates spawns, a leave is a visibility despawn (not a
// destruction) that re-spawns and re-baselines on re-entry.

#include <doctest/doctest.h>

#include <glm/geometric.hpp>

#include <Veng/Asset/Prefab.h>
#include <Veng/Net/Interest.h>
#include <Veng/Net/Replication.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>

#include "support/TestComponents.h"
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

#include <array>

using namespace Veng;
using namespace Veng::Net;

namespace
{
    InterestCandidate At(NetId id, f32 distance)
    {
        return InterestCandidate{.Id = id, .Distance = distance};
    }
}

TEST_CASE("UpdateInterest enters within the radius and culls beyond the leave band")
{
    const InterestSettings settings{
        .Radius = 10.0f, .LeaveMultiplier = 1.5f, .MinDwellSnapshots = 0};
    InterestState state;

    const std::array near = {At(1, 5.0f), At(2, 50.0f)};
    const set<NetId> first = UpdateInterest(near, {}, {}, settings, state);
    CHECK(first.contains(1));       // within the radius
    CHECK_FALSE(first.contains(2)); // far beyond the leave band
}

TEST_CASE("UpdateInterest holds a member through the hysteresis band")
{
    const InterestSettings settings{
        .Radius = 10.0f, .LeaveMultiplier = 1.5f, .MinDwellSnapshots = 0};
    InterestState state;

    // Enter at 5, then drift to 13 (within the 15 leave radius) — it stays, no flap.
    UpdateInterest(std::array{At(1, 5.0f)}, {}, {}, settings, state);
    const set<NetId> band = UpdateInterest(std::array{At(1, 13.0f)}, {}, {}, settings, state);
    CHECK(band.contains(1));

    // Past the leave radius (16 > 15) it finally leaves.
    const set<NetId> left = UpdateInterest(std::array{At(1, 16.0f)}, {}, {}, settings, state);
    CHECK_FALSE(left.contains(1));
}

TEST_CASE("The dwell floor keeps a member for a minimum number of snapshots")
{
    const InterestSettings settings{
        .Radius = 10.0f, .LeaveMultiplier = 1.0f, .MinDwellSnapshots = 3};
    InterestState state;

    UpdateInterest(std::array{At(1, 5.0f)}, {}, {}, settings, state); // enter, dwell = 3
    // Immediately far away every snapshot; the dwell floor holds it for MinDwell snapshots.
    CHECK(
        UpdateInterest(std::array{At(1, 99.0f)}, {}, {}, settings, state).contains(1)); // dwell 3→2
    CHECK(
        UpdateInterest(std::array{At(1, 99.0f)}, {}, {}, settings, state).contains(1)); // dwell 2→1
    CHECK(
        UpdateInterest(std::array{At(1, 99.0f)}, {}, {}, settings, state).contains(1)); // dwell 1→0
    CHECK_FALSE(
        UpdateInterest(std::array{At(1, 99.0f)}, {}, {}, settings, state).contains(1)); // gone
}

TEST_CASE("Always-relevant and policy arms are unconditional; radius 0 disables the spatial arm")
{
    InterestState state;

    const std::array<NetId, 1> always = {7};
    const std::array<NetId, 1> extra = {8};

    SUBCASE("radius 0 keeps only always-relevant and policy, never the spatial candidates")
    {
        const InterestSettings off{.Radius = 0.0f};
        const set<NetId> result =
            UpdateInterest(std::array{At(1, 1.0f)}, always, extra, off, state);
        CHECK_FALSE(result.contains(1));
        CHECK(result.contains(7));
        CHECK(result.contains(8));
    }
    SUBCASE("a far always-relevant entity is still in the set")
    {
        const InterestSettings on{.Radius = 5.0f};
        const set<NetId> result = UpdateInterest(std::array{At(7, 9999.0f)}, always, {}, on, state);
        CHECK(result.contains(7)); // always-relevant beats the spatial cull
    }
}

TEST_CASE("GatherAlwaysRelevant finds the seat mark and a registered always-relevant mark")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    types.Register<VengTest::TestScore>();
    CHECK(types.Info(TypeIdOf<VengTest::TestScore>()).AlwaysRelevant);
    CHECK(types.Info(TypeIdOf<Viewer>()).AlwaysRelevant);
    CHECK_FALSE(types.Info(TypeIdOf<Transform>()).AlwaysRelevant);

    Unique<Scene> scene = Scene::Create(types);
    const Entity seat = scene->CreateEntity();
    scene->Add<Viewer>(seat);
    scene->Add<NetIdentity>(seat).Id = 42;
    const Entity prop = scene->CreateEntity();
    scene->Add<Transform>(prop);
    scene->Add<NetIdentity>(prop).Id = 43;

    const vector<NetId> ids = GatherAlwaysRelevant(*scene);
    CHECK(ids.size() == 1);
    CHECK(ids.front() == 42u); // the seat, not the plain prop
}

TEST_CASE("GatherSpatialCandidates returns entities within the leave radius with distances")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    Unique<Scene> scene = Scene::Create(types);

    const Entity a = scene->CreateEntity();
    scene->Add<Transform>(a, Transform{.Position = vec3(3.0f, 0.0f, 0.0f)});
    scene->Add<NetIdentity>(a).Id = 1;
    const Entity b = scene->CreateEntity();
    scene->Add<Transform>(b, Transform{.Position = vec3(100.0f, 0.0f, 0.0f)});
    scene->Add<NetIdentity>(b).Id = 2;

    const vector<InterestCandidate> got = GatherSpatialCandidates(*scene, vec3(0.0f), 10.0f);
    REQUIRE(got.size() == 1);
    CHECK(got.front().Id == 1u);
    CHECK(got.front().Distance == doctest::Approx(3.0f));
}

// ---- Replication integration: interest gates the wire and leave is a visibility despawn -----------

namespace
{
    AssetManager& FakeAssets()
    {
        alignas(16) static unsigned char bytes[64]{};
        return *reinterpret_cast<AssetManager*>(bytes);
    }
}

TEST_CASE("Interest gates spawns; a leave is a visibility despawn that re-baselines on re-entry")
{
    TypeRegistry serverTypes;
    RegisterBuiltinTypes(serverTypes);
    Unique<Scene> server = Scene::Create(serverTypes);
    server->SetChangeTick(1);

    const Entity a = server->CreateEntity();
    server->Add<Transform>(a, Transform{.Position = vec3(0.0f)});
    const Entity b = server->CreateEntity();
    server->Add<Transform>(b, Transform{.Position = vec3(0.0f)});
    NetIdAllocator allocator;
    AssignServerNetIds(*server, allocator);
    const NetId idA = server->Get<NetIdentity>(a).Id;
    const NetId idB = server->Get<NetIdentity>(b).Id;

    TypeRegistry clientTypes;
    RegisterBuiltinTypes(clientTypes);
    Unique<Scene> client = Scene::Create(clientTypes);
    ReplicationClient replClient([](AssetId) -> Ref<Prefab> { return nullptr; });

    ReplicationServer replServer(ReplicationServer::Settings{.SnapshotInterval = 2});
    replServer.AddConnection(1);

    ReplicationClient::ReliableApplyResult lastDespawn;
    const auto pump = [&](u64 tick, const set<NetId>& interest)
    {
        for (const ReplicationMessage& message : replServer.Generate(1, *server, tick, &interest))
        {
            if (message.Channel == Net::Channel::ReliableOrdered)
            {
                const auto applied = replClient.ApplyReliable(message.Bytes, *client, FakeAssets());
                if (applied.Despawned)
                {
                    lastDespawn = applied;
                }
            }
            else
            {
                replClient.ApplySnapshot(message.Bytes, *client);
            }
        }
    };

    // Only A is relevant: the client spawns A, never B.
    pump(2, set<NetId>{idA});
    CHECK_FALSE(replClient.Map().Lookup(idA).IsNull());
    CHECK(replClient.Map().Lookup(idB).IsNull());

    // B enters interest → it spawns.
    pump(4, set<NetId>{idA, idB});
    CHECK_FALSE(replClient.Map().Lookup(idB).IsNull());

    // A leaves interest → a visibility despawn (not a destruction), and A is torn down locally.
    pump(6, set<NetId>{idB});
    CHECK(lastDespawn.Despawned);
    CHECK(lastDespawn.Reason == DespawnReason::Visibility);
    CHECK(lastDespawn.Id == idA);
    CHECK(replClient.Map().Lookup(idA).IsNull());

    // A re-enters → it re-spawns (and re-baselines: its state applies afresh with no stale baseline).
    server->SetChangeTick(8);
    server->Get<Transform>(a).Position = vec3(5.0f, 0.0f, 0.0f);
    pump(8, set<NetId>{idA, idB});
    const Entity reA = replClient.Map().Lookup(idA);
    REQUIRE_FALSE(reA.IsNull());
    // The re-entry spawn carried A's current full state (position 5), proving a clean re-baseline.
    CHECK(client->Get<Transform>(reA).Position.x == doctest::Approx(5.0f));
}
