// The locally-controlled marker: the engine's answer to "which pawn is mine?", derived as
// presenting viewport → its bound seat → Possesses → that pawn. These drive the derivation
// directly against scenes, with no window and no transport, plus one real snapshot round-trip for
// the client path where Possesses changes through the stream rather than a local write. The
// load-bearing negative is the possessing, Tier::Local mirror seat: on a client every mirrored pawn
// carries one, so a rule keyed on the seat's tier alone marks every pawn on screen.

#include <doctest/doctest.h>

#include <Veng/Net/Replication.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/LocalControl.h>
#include <Veng/Scene/Scene.h>

#include <array>

using namespace Veng;

namespace
{
    // A seat entity possessing a fresh pawn, at the given authority tier.
    struct SeatAndPawn
    {
        Entity Seat = Entity::Null;
        Entity Pawn = Entity::Null;
    };

    SeatAndPawn MakeSeat(Scene& scene, const Tier tier)
    {
        const Entity pawn = scene.CreateEntity();
        const Entity seat = scene.CreateEntity();
        scene.Add<Viewer>(seat);
        scene.Add<Authority>(seat, Authority{.Tier = tier});
        scene.Add<Possesses>(seat, Possesses{.Pawn = pawn});
        return {.Seat = seat, .Pawn = pawn};
    }

    // The live marker count, so a case can pin that nothing beyond the expected pawn is marked.
    usize MarkerCount(const Scene& scene)
    {
        usize count = 0;
        for (auto [entity, control] : scene.View<const LocalControl>())
        {
            (void)entity;
            (void)control;
            ++count;
        }
        return count;
    }

    // The one-viewport reconcile: the shape Application runs per presented world each frame.
    void Present(Scene& scene, const Entity seat)
    {
        const std::array<Entity, 1> seats{seat};
        ReconcileLocalControl(scene, seats);
    }
}

TEST_CASE("A presenting viewport's seat marks the pawn it possesses")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    Unique<Scene> scene = Scene::Create(types);

    const SeatAndPawn own = MakeSeat(*scene, Tier::Local);
    Present(*scene, own.Seat);

    REQUIRE(scene->Has<LocalControl>(own.Pawn));
    CHECK(scene->Get<LocalControl>(own.Pawn).Seat == own.Seat);
    CHECK(ResolveLocalControlledPawn(*scene, own.Seat) == own.Pawn);
}

TEST_CASE("A seat possessing nothing marks nothing")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    Unique<Scene> scene = Scene::Create(types);

    const Entity seat = scene->CreateEntity();
    scene->Add<Viewer>(seat);
    scene->Add<Possesses>(seat);
    Present(*scene, seat);

    CHECK(ResolveLocalControlledPawn(*scene, seat) == Entity::Null);
    CHECK(MarkerCount(*scene) == 0);
}

TEST_CASE("Possession transferred to a remote seat clears the presenting seat's marker")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    Unique<Scene> scene = Scene::Create(types);

    const SeatAndPawn own = MakeSeat(*scene, Tier::Local);
    const Entity remoteSeat = scene->CreateEntity();
    scene->Add<Viewer>(remoteSeat);
    scene->Add<Possesses>(remoteSeat);

    Present(*scene, own.Seat);
    REQUIRE(scene->Has<LocalControl>(own.Pawn));

    // The pawn changes hands: the presenting seat releases it and the remote seat takes it.
    scene->Get<Possesses>(own.Seat).Pawn = Entity::Null;
    scene->Get<Possesses>(remoteSeat).Pawn = own.Pawn;
    Present(*scene, own.Seat);

    CHECK_FALSE(scene->Has<LocalControl>(own.Pawn));
    CHECK(ResolveLocalControlledPawn(*scene, own.Seat) == Entity::Null);
}

TEST_CASE("The marker follows the presenting seat to a newly possessed pawn")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    Unique<Scene> scene = Scene::Create(types);

    const SeatAndPawn own = MakeSeat(*scene, Tier::Local);
    Present(*scene, own.Seat);

    const Entity next = scene->CreateEntity();
    scene->Get<Possesses>(own.Seat).Pawn = next;
    Present(*scene, own.Seat);

    CHECK_FALSE(scene->Has<LocalControl>(own.Pawn));
    CHECK(scene->Has<LocalControl>(next));
    CHECK(ResolveLocalControlledPawn(*scene, own.Seat) == next);
}

TEST_CASE("A possessing, local-tier mirror seat on a client is not marked")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    Unique<Scene> scene = Scene::Create(types);

    // The client's own seat, and two other players' mirrored seats — each carrying its own
    // instantiated Tier::Local seat possessing its own pawn, exactly as a client's world holds them.
    const SeatAndPawn own = MakeSeat(*scene, Tier::Local);
    const SeatAndPawn mirrorA = MakeSeat(*scene, Tier::Local);
    const SeatAndPawn mirrorB = MakeSeat(*scene, Tier::Local);

    Present(*scene, own.Seat);

    // "Possessed by a Tier::Local seat" would mark all three; only the presenting viewport's does.
    CHECK(scene->Has<LocalControl>(own.Pawn));
    CHECK_FALSE(scene->Has<LocalControl>(mirrorA.Pawn));
    CHECK_FALSE(scene->Has<LocalControl>(mirrorB.Pawn));
    CHECK(MarkerCount(*scene) == 1);
    CHECK(ResolveLocalControlledPawn(*scene, mirrorA.Seat) == Entity::Null);
}

TEST_CASE("A dedicated host presents no viewport, so it marks nothing")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    Unique<Scene> scene = Scene::Create(types);

    const SeatAndPawn a = MakeSeat(*scene, Tier::Server);
    const SeatAndPawn b = MakeSeat(*scene, Tier::Server);

    // No presenting viewport ⇒ no presenting seat.
    ReconcileLocalControl(*scene, {});

    CHECK_FALSE(scene->Has<LocalControl>(a.Pawn));
    CHECK_FALSE(scene->Has<LocalControl>(b.Pawn));
}

TEST_CASE("A listen host marks the pawn its own presenting seat controls, and no other")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    Unique<Scene> scene = Scene::Create(types);

    // The host presents through its own local seat; the joined player's server-spawned seat is in
    // the same world and possesses its own pawn.
    const SeatAndPawn host = MakeSeat(*scene, Tier::Local);
    const SeatAndPawn joiner = MakeSeat(*scene, Tier::Server);

    Present(*scene, host.Seat);

    CHECK(scene->Has<LocalControl>(host.Pawn));
    CHECK_FALSE(scene->Has<LocalControl>(joiner.Pawn));
}

TEST_CASE("Split-screen: each presenting viewport reads back its own pawn")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    Unique<Scene> scene = Scene::Create(types);

    const SeatAndPawn one = MakeSeat(*scene, Tier::Local);
    const SeatAndPawn two = MakeSeat(*scene, Tier::Local);

    const std::array<Entity, 2> seats{one.Seat, two.Seat};
    ReconcileLocalControl(*scene, seats);

    CHECK(ResolveLocalControlledPawn(*scene, one.Seat) == one.Pawn);
    CHECK(ResolveLocalControlledPawn(*scene, two.Seat) == two.Pawn);
    CHECK(scene->Get<LocalControl>(one.Pawn).Seat == one.Seat);
    CHECK(scene->Get<LocalControl>(two.Pawn).Seat == two.Seat);

    // One viewport closing leaves the other's marker alone and clears only its own.
    const std::array<Entity, 1> remaining{two.Seat};
    ReconcileLocalControl(*scene, remaining);
    CHECK_FALSE(scene->Has<LocalControl>(one.Pawn));
    CHECK(scene->Has<LocalControl>(two.Pawn));
}

TEST_CASE("Teardown leaves no stale marker")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    Unique<Scene> scene = Scene::Create(types);

    const SeatAndPawn own = MakeSeat(*scene, Tier::Local);
    Present(*scene, own.Seat);
    REQUIRE(scene->Has<LocalControl>(own.Pawn));

    // The seat goes away (a world switch, a seat despawn): the marker it derived must go with it.
    scene->DestroyEntity(own.Seat);
    Present(*scene, own.Seat);

    CHECK_FALSE(scene->Has<LocalControl>(own.Pawn));
    CHECK(MarkerCount(*scene) == 0);
}

TEST_CASE("A destroyed pawn leaves no marker behind")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    Unique<Scene> scene = Scene::Create(types);

    const SeatAndPawn own = MakeSeat(*scene, Tier::Local);
    Present(*scene, own.Seat);

    scene->DestroyEntity(own.Pawn);
    Present(*scene, own.Seat);

    CHECK(MarkerCount(*scene) == 0);
    CHECK(ResolveLocalControlledPawn(*scene, own.Seat) == Entity::Null);
}

TEST_CASE("Marker moves are reported once, and a stable frame reports none")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    Unique<Scene> scene = Scene::Create(types);

    const SeatAndPawn own = MakeSeat(*scene, Tier::Local);
    const std::array<Entity, 1> seats{own.Seat};

    vector<LocalControlChange> changed;
    ReconcileLocalControl(*scene, seats, &changed);
    REQUIRE(changed.size() == 1);
    CHECK(changed[0].Seat == own.Seat);
    CHECK(changed[0].Pawn == own.Pawn);

    changed.clear();
    ReconcileLocalControl(*scene, seats, &changed);
    CHECK(changed.empty());

    // Releasing the pawn is a move to nothing, reported once.
    scene->Get<Possesses>(own.Seat).Pawn = Entity::Null;
    ReconcileLocalControl(*scene, seats, &changed);
    REQUIRE(changed.size() == 1);
    CHECK(changed[0].Seat == own.Seat);
    CHECK(changed[0].Pawn == Entity::Null);
}

TEST_CASE("A client's Possesses change arriving through snapshot apply moves the marker")
{
    TypeRegistry serverTypes;
    RegisterBuiltinTypes(serverTypes);
    Unique<Scene> server = Scene::Create(serverTypes);

    // Server truth: one seat and two pawns, the seat possessing the first.
    const Entity firstPawn = server->CreateEntity();
    server->Add<Transform>(firstPawn);
    const Entity secondPawn = server->CreateEntity();
    server->Add<Transform>(secondPawn);
    const Entity seat = server->CreateEntity();
    server->Add<Viewer>(seat);
    server->Add<Possesses>(seat, Possesses{.Pawn = firstPawn});

    NetIdAllocator allocator;
    AssignServerNetIds(*server, allocator);
    const NetId firstId = server->Get<NetIdentity>(firstPawn).Id;
    const NetId secondId = server->Get<NetIdentity>(secondPawn).Id;
    const NetId seatId = server->Get<NetIdentity>(seat).Id;

    TypeRegistry clientTypes;
    RegisterBuiltinTypes(clientTypes);
    Unique<Scene> client = Scene::Create(clientTypes);
    NetIdMap map;
    const auto mirror = [&](const NetId id)
    {
        const Entity entity = client->CreateEntity();
        client->Add<NetIdentity>(entity).Id = id;
        client->Add<Authority>(entity, Authority{.Tier = Tier::Remote});
        map.Bind(id, entity);
        return entity;
    };
    const Entity clientSeat = mirror(seatId);
    const Entity clientFirst = mirror(firstId);
    const Entity clientSecond = mirror(secondId);

    ApplySnapshot(EncodeSnapshot(*server, 1, 0), *client, map);
    Present(*client, clientSeat);
    CHECK(ResolveLocalControlledPawn(*client, clientSeat) == clientFirst);

    // The possession changes server-side and reaches the client purely through the stream — no
    // client-side write to Possesses at all. The next reconcile must move the marker with it.
    server->SetChangeTick(2);
    server->Get<Possesses>(seat).Pawn = secondPawn;
    ApplySnapshot(EncodeSnapshot(*server, 2, 1), *client, map);
    Present(*client, clientSeat);

    CHECK_FALSE(client->Has<LocalControl>(clientFirst));
    CHECK(ResolveLocalControlledPawn(*client, clientSeat) == clientSecond);
}

TEST_CASE("The marker never rides the wire")
{
    TypeRegistry serverTypes;
    RegisterBuiltinTypes(serverTypes);
    CHECK_FALSE(serverTypes.Info(TypeIdOf<LocalControl>()).Replicated);

    Unique<Scene> server = Scene::Create(serverTypes);
    const Entity pawn = server->CreateEntity();
    server->Add<Transform>(pawn);

    NetIdAllocator allocator;
    AssignServerNetIds(*server, allocator);
    const NetId pawnId = server->Get<NetIdentity>(pawn).Id;

    // A listen host presents its own world, so its authoritative pawn carries the marker while it
    // encodes snapshots. The marker must not reach the joiner.
    const Entity seat = server->CreateEntity();
    server->Add<Viewer>(seat);
    server->Add<Possesses>(seat, Possesses{.Pawn = pawn});
    Present(*server, seat);
    REQUIRE(server->Has<LocalControl>(pawn));

    TypeRegistry clientTypes;
    RegisterBuiltinTypes(clientTypes);
    Unique<Scene> client = Scene::Create(clientTypes);
    NetIdMap map;
    const Entity clientPawn = client->CreateEntity();
    client->Add<NetIdentity>(clientPawn).Id = pawnId;
    map.Bind(pawnId, clientPawn);

    ApplySnapshot(EncodeSnapshot(*server, 1, 0), *client, map);

    CHECK_FALSE(client->Has<LocalControl>(clientPawn));
    CHECK(MarkerCount(*client) == 0);
}
