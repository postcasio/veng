// The replication model: the Replicated mark, per-entity change ticks, and the snapshot codec —
// pure and device-free, exercised with two in-process scenes and no transport. A server scene is
// encoded and applied into a client scene; the tests pin field-identical convergence, Entity-field
// NetId translation, dirty gating against an acked tick, the recovery posture (unknown NetId/TypeId,
// truncation), and that a const read never dirties a component.

#include <doctest/doctest.h>

#include <Veng/Log.h>
#include <Veng/Net/Replication.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>

#include "support/TestComponents.h"
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

#include <algorithm>
#include <string_view>

using namespace Veng;

namespace
{
    // Appends every Log::Error message body to @p out for the guard's lifetime, restoring the
    // default sink on destruction so a failing assertion never leaves a later test logging into a
    // dangling capture. The sink writes through the borrowed vector, not the guard, so the guard
    // itself carries no mutated state.
    struct ErrorCapture
    {
        explicit ErrorCapture(vector<string>& out)
        {
            Log::SetSink(
                [&out](const Log::Level level, const std::string_view message)
                {
                    if (level == Log::Level::Error)
                    {
                        out.emplace_back(message);
                    }
                });
        }

        ~ErrorCapture() { Log::SetSink(nullptr); }

        ErrorCapture(const ErrorCapture&) = delete;
        ErrorCapture& operator=(const ErrorCapture&) = delete;
    };

    // True when any captured error message contains @p needle.
    bool AnyContains(const vector<string>& errors, std::string_view needle)
    {
        return std::ranges::any_of(errors, [needle](const string& e)
                                   { return e.find(needle) != string::npos; });
    }
    // Little-endian framing helpers matching the codec, for hand-crafting adversarial packets.
    void AppendU32LE(vector<u8>& out, u32 value)
    {
        for (u32 i = 0; i < 4; ++i)
        {
            out.push_back(static_cast<u8>(value >> (8 * i)));
        }
    }

    void AppendU64LE(vector<u8>& out, u64 value)
    {
        for (u32 i = 0; i < 8; ++i)
        {
            out.push_back(static_cast<u8>(value >> (8 * i)));
        }
    }

    // A client mirror of a server entity: a fresh local handle bound to the same NetId.
    Entity SpawnClientMirror(Scene& client, NetIdMap& map, NetId id)
    {
        const Entity entity = client.CreateEntity();
        client.Add<NetIdentity>(entity).Id = id;
        map.Bind(id, entity);
        return entity;
    }
}

TEST_CASE("The Replicated mark is authored on the right builtin types")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    types.Register<VengTest::TestScore>();

    CHECK(types.Info(TypeIdOf<Transform>()).Replicated);
    CHECK(types.Info(TypeIdOf<VengTest::TestScore>()).Replicated);
    CHECK(types.Info(TypeIdOf<Possesses>()).Replicated);
    CHECK(types.Info(TypeIdOf<Viewer>()).Replicated);

    // The wire key itself and the client-local / derived state are deliberately unmarked.
    CHECK_FALSE(types.Info(TypeIdOf<NetIdentity>()).Replicated);
    CHECK_FALSE(types.Info(TypeIdOf<Intent>()).Replicated);
    CHECK_FALSE(types.Info(TypeIdOf<PlayerInput>()).Replicated);
    CHECK_FALSE(types.Info(TypeIdOf<SeatInput>()).Replicated);
    CHECK_FALSE(types.Info(TypeIdOf<CameraFollow>()).Replicated);
    CHECK_FALSE(types.Info(TypeIdOf<CameraLook>()).Replicated);
}

TEST_CASE("NetIdAllocator hands out fresh, non-zero, never-reused ids")
{
    NetIdAllocator allocator;
    CHECK(allocator.Last() == InvalidNetId);
    const NetId a = allocator.Next();
    const NetId b = allocator.Next();
    const NetId c = allocator.Next();
    CHECK(a == 1);
    CHECK(b == 2);
    CHECK(c == 3);
    CHECK(a != InvalidNetId);
    CHECK(allocator.Last() == 3);
}

TEST_CASE("AssignServerNetIds identifies server-authoritative entities only")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    Unique<Scene> scene = Scene::Create(types);

    const Entity implicitServer = scene->CreateEntity(); // no Authority ⇒ Server default
    scene->Add<Transform>(implicitServer);

    const Entity explicitServer = scene->CreateEntity();
    scene->Add<Authority>(explicitServer, Authority{.Tier = Tier::Server});

    const Entity local = scene->CreateEntity();
    scene->Add<Authority>(local, Authority{.Tier = Tier::Local});

    NetIdAllocator allocator;
    const usize assigned = AssignServerNetIds(*scene, allocator);
    CHECK(assigned == 2);

    CHECK(scene->Has<NetIdentity>(implicitServer));
    CHECK(scene->Has<NetIdentity>(explicitServer));
    CHECK_FALSE(scene->Has<NetIdentity>(local));

    // Re-running assigns nothing new (ids are stable for an entity's lifetime).
    CHECK(AssignServerNetIds(*scene, allocator) == 0);
}

TEST_CASE("Round-trip: a snapshot converges the client to field-identical replicated state")
{
    TypeRegistry serverTypes;
    RegisterBuiltinTypes(serverTypes);
    serverTypes.Register<VengTest::TestScore>();
    Unique<Scene> server = Scene::Create(serverTypes);

    // Authored with no tick stepped — the pre-tick population window a level load hits. Each write
    // stamps the scene's change-tick floor, which is strictly above a fresh connection's zero.
    const Entity pawn = server->CreateEntity();
    server->Add<Transform>(pawn,
                           Transform{.Position = vec3(1.0f, 2.0f, 3.0f), .Scale = vec3(2.0f)});

    const Entity scoreEntity = server->CreateEntity();
    server->Add<VengTest::TestScore>(scoreEntity, VengTest::TestScore{.Value = 7});

    NetIdAllocator allocator;
    AssignServerNetIds(*server, allocator);
    const NetId pawnId = server->Get<NetIdentity>(pawn).Id;
    const NetId scoreId = server->Get<NetIdentity>(scoreEntity).Id;

    // The client has spawned matching entities (Plan 04's flow) and bound their ids.
    TypeRegistry clientTypes;
    RegisterBuiltinTypes(clientTypes);
    clientTypes.Register<VengTest::TestScore>();
    Unique<Scene> client = Scene::Create(clientTypes);
    NetIdMap map;
    const Entity clientPawn = SpawnClientMirror(*client, map, pawnId);
    const Entity clientScore = SpawnClientMirror(*client, map, scoreId);

    const vector<u8> packet = EncodeSnapshot(*server, 3, 0);
    const SnapshotApplyResult applied = ApplySnapshot(packet, *client, map);

    CHECK(applied.HeaderValid);
    CHECK(applied.ServerTick == 3);
    CHECK(applied.EntitiesApplied == 2);
    CHECK(applied.EntitiesDropped == 0);

    const auto* pawnTransform = client->TryGet<Transform>(clientPawn);
    REQUIRE(pawnTransform != nullptr);
    CHECK(pawnTransform->Position.x == doctest::Approx(1.0f));
    CHECK(pawnTransform->Position.y == doctest::Approx(2.0f));
    CHECK(pawnTransform->Position.z == doctest::Approx(3.0f));
    CHECK(pawnTransform->Scale.x == doctest::Approx(2.0f));

    const auto* score = client->TryGet<VengTest::TestScore>(clientScore);
    REQUIRE(score != nullptr);
    CHECK(score->Value == 7);
}

TEST_CASE("Entity-field remap: a replicated reference resolves to the local handle on apply")
{
    TypeRegistry serverTypes;
    RegisterBuiltinTypes(serverTypes);
    Unique<Scene> server = Scene::Create(serverTypes);

    const Entity pawn = server->CreateEntity();
    server->Add<Transform>(pawn);
    const Entity seat = server->CreateEntity();
    server->Add<Possesses>(seat, Possesses{.Pawn = pawn});

    NetIdAllocator allocator;
    AssignServerNetIds(*server, allocator);
    const NetId pawnId = server->Get<NetIdentity>(pawn).Id;
    const NetId seatId = server->Get<NetIdentity>(seat).Id;

    TypeRegistry clientTypes;
    RegisterBuiltinTypes(clientTypes);
    Unique<Scene> client = Scene::Create(clientTypes);
    NetIdMap map;
    // Spawn the pawn mirror second, so the client's local handles differ from the server's — a plain
    // handle copy would land on the wrong entity; only a NetId round-trip resolves correctly.
    const Entity clientSeat = SpawnClientMirror(*client, map, seatId);
    const Entity clientPawn = SpawnClientMirror(*client, map, pawnId);

    const vector<u8> packet = EncodeSnapshot(*server, 1, 0);
    ApplySnapshot(packet, *client, map);

    const auto* possesses = client->TryGet<Possesses>(clientSeat);
    REQUIRE(possesses != nullptr);
    CHECK(possesses->Pawn == clientPawn);
    CHECK_FALSE(possesses->Pawn == pawn); // not the server-space handle
}

TEST_CASE("A reference to an unreplicated target encodes as the null wire id")
{
    TypeRegistry serverTypes;
    RegisterBuiltinTypes(serverTypes);
    Unique<Scene> server = Scene::Create(serverTypes);

    // A Local-tier target carries no NetIdentity — referencing it from replicated state is an
    // authoring error the codec encodes as a null reference.
    const Entity localTarget = server->CreateEntity();
    server->Add<Authority>(localTarget, Authority{.Tier = Tier::Local});
    const Entity seat = server->CreateEntity();
    server->Add<Possesses>(seat, Possesses{.Pawn = localTarget});

    NetIdAllocator allocator;
    AssignServerNetIds(*server, allocator);
    const NetId seatId = server->Get<NetIdentity>(seat).Id;

    TypeRegistry clientTypes;
    RegisterBuiltinTypes(clientTypes);
    Unique<Scene> client = Scene::Create(clientTypes);
    NetIdMap map;
    const Entity clientSeat = SpawnClientMirror(*client, map, seatId);

    const vector<u8> packet = EncodeSnapshot(*server, 1, 0);
    ApplySnapshot(packet, *client, map);

    const auto* possesses = client->TryGet<Possesses>(clientSeat);
    REQUIRE(possesses != nullptr);
    CHECK(possesses->Pawn.IsNull());
}

TEST_CASE("The unreplicated-reference diagnostic discriminates on the target component")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);

    // A seat whose Possesses.Pawn names an alive, Local-tier target (no NetIdentity) is the one
    // case the encoder reports; the other three collapse to the same null wire value but are silent.
    const auto buildSeatToward = [&types](Entity (*makeTarget)(Scene&)) -> Unique<Scene>
    {
        Unique<Scene> scene = Scene::Create(types);
        const Entity target = makeTarget(*scene);
        const Entity seat = scene->CreateEntity();
        scene->Add<Possesses>(seat, Possesses{.Pawn = target});
        NetIdAllocator allocator;
        AssignServerNetIds(*scene, allocator);
        return scene;
    };

    SUBCASE("an alive, unreplicated target is reported once, naming the component, field, entities")
    {
        const Unique<Scene> scene = buildSeatToward(
            [](Scene& s)
            {
                const Entity t = s.CreateEntity();
                s.Add<Authority>(t, Authority{.Tier = Tier::Local});
                return t;
            });

        vector<string> errors;
        const ErrorCapture capture(errors);
        UnreplicatedReferenceReporter reporter;
        (void)EncodeSnapshot(*scene, 1, 0, 0, 0, &reporter);

        REQUIRE(errors.size() == 1);
        CHECK(AnyContains(errors, "Possesses"));
        CHECK(AnyContains(errors, "Pawn"));
        CHECK(AnyContains(errors, "NetIdentity"));

        // The report does not repeat per tick for the same (type, field, referring entity).
        (void)EncodeSnapshot(*scene, 2, 0, 0, 0, &reporter);
        CHECK(errors.size() == 1);
    }

    SUBCASE("a deliberately null field is silent")
    {
        const Unique<Scene> scene = buildSeatToward([](Scene&) { return Entity::Null; });
        vector<string> errors;
        const ErrorCapture capture(errors);
        UnreplicatedReferenceReporter reporter;
        (void)EncodeSnapshot(*scene, 1, 0, 0, 0, &reporter);
        CHECK(errors.empty());
    }

    SUBCASE("a field naming a destroyed entity is silent")
    {
        Unique<Scene> scene = Scene::Create(types);
        const Entity target = scene->CreateEntity();
        scene->Add<Authority>(target, Authority{.Tier = Tier::Local});
        const Entity seat = scene->CreateEntity();
        scene->Add<Possesses>(seat, Possesses{.Pawn = target});
        NetIdAllocator allocator;
        AssignServerNetIds(*scene, allocator);
        scene->DestroyEntity(target); // the seat's Possesses.Pawn is now a stale handle

        vector<string> errors;
        const ErrorCapture capture(errors);
        UnreplicatedReferenceReporter reporter;
        (void)EncodeSnapshot(*scene, 1, 0, 0, 0, &reporter);
        CHECK(errors.empty());
    }

    SUBCASE("a replicated target is silent, including one whose id is not yet assigned")
    {
        // A target carrying NetIdentity replicates; an unassigned id (the seat-spawn transient)
        // encodes identically to no component, so the discriminator is the component, not the id.
        const Unique<Scene> scene = buildSeatToward(
            [](Scene& s)
            {
                const Entity t = s.CreateEntity();
                s.Add<NetIdentity>(t).Id = InvalidNetId; // carries the component, id unbound
                return t;
            });

        vector<string> errors;
        const ErrorCapture capture(errors);
        UnreplicatedReferenceReporter reporter;
        (void)EncodeSnapshot(*scene, 1, 0, 0, 0, &reporter);
        CHECK(errors.empty());
    }
}

TEST_CASE("A distinct second offender of the same (type, field) is still reported")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    Unique<Scene> scene = Scene::Create(types);

    // Two seats, each possessing its own alive Local-tier pawn: the dedup is keyed on the referring
    // entity too, so "which one of these is wrong" is not hidden behind the first.
    for (int i = 0; i < 2; ++i)
    {
        const Entity target = scene->CreateEntity();
        scene->Add<Authority>(target, Authority{.Tier = Tier::Local});
        const Entity seat = scene->CreateEntity();
        scene->Add<Possesses>(seat, Possesses{.Pawn = target});
    }
    NetIdAllocator allocator;
    AssignServerNetIds(*scene, allocator);

    vector<string> errors;
    const ErrorCapture capture(errors);
    UnreplicatedReferenceReporter reporter;
    (void)EncodeSnapshot(*scene, 1, 0, 0, 0, &reporter);
    CHECK(errors.size() == 2);
}

TEST_CASE("A field opting out of the diagnostic (Viewer.Camera) is silent")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    Unique<Scene> scene = Scene::Create(types);

    // Viewer.Camera deliberately names a client-local camera and declares
    // AllowUnreplicatedReference, so the engine's own commonest replicated seat draws nothing.
    const Entity camera = scene->CreateEntity();
    scene->Add<Camera>(camera);
    scene->Add<Authority>(camera, Authority{.Tier = Tier::Local});
    const Entity seat = scene->CreateEntity();
    scene->Add<Viewer>(seat, Viewer{.Camera = camera});
    NetIdAllocator allocator;
    AssignServerNetIds(*scene, allocator);

    vector<string> errors;
    const ErrorCapture capture(errors);
    UnreplicatedReferenceReporter reporter;
    (void)EncodeSnapshot(*scene, 1, 0, 0, 0, &reporter);
    CHECK(errors.empty());
}

TEST_CASE(
    "The diagnostic is a side effect: encoded bytes are identical with and without a reporter")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    Unique<Scene> scene = Scene::Create(types);

    const Entity target = scene->CreateEntity();
    scene->Add<Authority>(target, Authority{.Tier = Tier::Local});
    const Entity seat = scene->CreateEntity();
    scene->Add<Possesses>(seat, Possesses{.Pawn = target});
    NetIdAllocator allocator;
    AssignServerNetIds(*scene, allocator);

    const vector<u8> withoutReporter = EncodeSnapshot(*scene, 1, 0);
    vector<string> errors;
    const ErrorCapture capture(errors);
    UnreplicatedReferenceReporter reporter;
    const vector<u8> withReporter = EncodeSnapshot(*scene, 1, 0, 0, 0, &reporter);

    CHECK(withoutReporter == withReporter);
    CHECK(errors.size() == 1); // the reporter still fired, it just changed no bytes
}

TEST_CASE("Dirty gating: only components changed since the acked tick are sent")
{
    TypeRegistry serverTypes;
    RegisterBuiltinTypes(serverTypes);
    Unique<Scene> server = Scene::Create(serverTypes);

    const Entity a = server->CreateEntity();
    server->Add<Transform>(a);
    const Entity b = server->CreateEntity();
    server->Add<Transform>(b);

    NetIdAllocator allocator;
    AssignServerNetIds(*server, allocator);
    const NetId idA = server->Get<NetIdentity>(a).Id;
    const NetId idB = server->Get<NetIdentity>(b).Id;

    TypeRegistry clientTypes;
    RegisterBuiltinTypes(clientTypes);
    Unique<Scene> client = Scene::Create(clientTypes);
    NetIdMap map;
    SpawnClientMirror(*client, map, idA);
    SpawnClientMirror(*client, map, idB);

    // Baseline: both entities changed after tick 0.
    const SnapshotApplyResult baseline = ApplySnapshot(EncodeSnapshot(*server, 1, 0), *client, map);
    CHECK(baseline.EntitiesApplied == 2);

    // Tick 2 touches only A (a non-const access stamps its change tick).
    server->SetChangeTick(2);
    server->Get<Transform>(a).Position.x = 9.0f;

    // With the last acked tick at 1, only A is dirty; B (still tick 1) is omitted.
    const SnapshotApplyResult delta = ApplySnapshot(EncodeSnapshot(*server, 2, 1), *client, map);
    CHECK(delta.EntitiesApplied == 1);

    // Advancing the acked tick to 2 suppresses the resend: nothing exceeds it.
    const SnapshotApplyResult acked = ApplySnapshot(EncodeSnapshot(*server, 2, 2), *client, map);
    CHECK(acked.HeaderValid);
    CHECK(acked.EntitiesApplied == 0);
    CHECK(acked.EntitiesDropped == 0);
}

TEST_CASE("An unknown NetId drops its record; the rest still apply")
{
    TypeRegistry serverTypes;
    RegisterBuiltinTypes(serverTypes);
    Unique<Scene> server = Scene::Create(serverTypes);

    const Entity a = server->CreateEntity();
    server->Add<Transform>(a, Transform{.Position = vec3(5.0f)});
    const Entity b = server->CreateEntity();
    server->Add<Transform>(b, Transform{.Position = vec3(6.0f)});

    NetIdAllocator allocator;
    AssignServerNetIds(*server, allocator);
    const NetId idA = server->Get<NetIdentity>(a).Id;
    const NetId idB = server->Get<NetIdentity>(b).Id;

    TypeRegistry clientTypes;
    RegisterBuiltinTypes(clientTypes);
    Unique<Scene> client = Scene::Create(clientTypes);
    NetIdMap map;
    // Only A is bound on the client; B's NetId is unknown (its spawn has not arrived yet).
    const Entity clientA = SpawnClientMirror(*client, map, idA);

    const SnapshotApplyResult applied = ApplySnapshot(EncodeSnapshot(*server, 1, 0), *client, map);
    CHECK(applied.HeaderValid);
    CHECK(applied.EntitiesApplied == 1);
    CHECK(applied.EntitiesDropped == 1);

    const auto* transform = client->TryGet<Transform>(clientA);
    REQUIRE(transform != nullptr);
    CHECK(transform->Position.x == doctest::Approx(5.0f));
    (void)idB;
}

TEST_CASE("An unknown TypeId is skipped without disturbing the walk")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    Unique<Scene> client = Scene::Create(types);
    NetIdMap map;
    const Entity entity = SpawnClientMirror(*client, map, 1);

    // Hand-craft a packet: one entity record with one component of an unregistered TypeId.
    vector<u8> packet;
    AppendU64LE(packet, 5);             // server tick
    AppendU32LE(packet, 0);             // input feedback
    AppendU64LE(packet, 0);             // last consumed input tick
    AppendU32LE(packet, 1);             // NetId
    AppendU32LE(packet, 1);             // component count
    AppendU64LE(packet, 0xDEADBEEFULL); // unregistered TypeId
    AppendU32LE(packet, 0);             // byte length (empty payload)

    const SnapshotApplyResult applied = ApplySnapshot(packet, *client, map);
    CHECK(applied.HeaderValid);
    CHECK(applied.ServerTick == 5);
    CHECK(applied.EntitiesApplied == 0); // the known entity had nothing applicable
    CHECK(applied.EntitiesDropped == 0);
    CHECK(client->IsAlive(entity));
}

TEST_CASE("A truncated packet applies what it can and never runs off the end")
{
    TypeRegistry serverTypes;
    RegisterBuiltinTypes(serverTypes);
    Unique<Scene> server = Scene::Create(serverTypes);

    const Entity a = server->CreateEntity();
    server->Add<Transform>(a, Transform{.Position = vec3(3.0f)});
    const Entity b = server->CreateEntity();
    server->Add<Transform>(b, Transform{.Position = vec3(4.0f)});

    NetIdAllocator allocator;
    AssignServerNetIds(*server, allocator);
    const NetId idA = server->Get<NetIdentity>(a).Id;
    const NetId idB = server->Get<NetIdentity>(b).Id;

    TypeRegistry clientTypes;
    RegisterBuiltinTypes(clientTypes);
    Unique<Scene> client = Scene::Create(clientTypes);
    NetIdMap map;
    SpawnClientMirror(*client, map, idA);
    SpawnClientMirror(*client, map, idB);

    vector<u8> packet = EncodeSnapshot(*server, 1, 0);

    SUBCASE("A packet with only a header applies nothing but reports the tick")
    {
        vector<u8> headerOnly(packet.begin(),
                              packet.begin() + static_cast<isize>(SnapshotHeaderSize));
        const SnapshotApplyResult applied = ApplySnapshot(headerOnly, *client, map);
        CHECK(applied.HeaderValid);
        CHECK(applied.EntitiesApplied == 0);
    }

    SUBCASE("A packet truncated below its header applies nothing at all")
    {
        vector<u8> stub(packet.begin(), packet.begin() + 4);
        const SnapshotApplyResult applied = ApplySnapshot(stub, *client, map);
        CHECK_FALSE(applied.HeaderValid);
        CHECK(applied.EntitiesApplied == 0);
    }

    SUBCASE("A mid-record truncation stops cleanly")
    {
        // Drop the final few bytes so the last entity record runs off the end.
        packet.resize(packet.size() - 3);
        const SnapshotApplyResult applied = ApplySnapshot(packet, *client, map);
        CHECK(applied.HeaderValid);
        // The first entity record decoded fully; the truncated tail was skipped, no crash.
        CHECK(applied.EntitiesApplied <= 1);
    }
}

TEST_CASE("The snapshot header carries a signed input-feedback field that round-trips")
{
    TypeRegistry serverTypes;
    RegisterBuiltinTypes(serverTypes);
    Unique<Scene> server = Scene::Create(serverTypes);
    const Entity e = server->CreateEntity();
    server->Add<Transform>(e, Transform{.Position = vec3(1.0f)});

    NetIdAllocator allocator;
    AssignServerNetIds(*server, allocator);

    TypeRegistry clientTypes;
    RegisterBuiltinTypes(clientTypes);
    Unique<Scene> client = Scene::Create(clientTypes);
    NetIdMap map;
    SpawnClientMirror(*client, map, server->Get<NetIdentity>(e).Id);

    // A negative feedback (the client running late) survives the u32<->i32 header round-trip.
    const SnapshotApplyResult negative =
        ApplySnapshot(EncodeSnapshot(*server, 5, 0, -3), *client, map);
    CHECK(negative.HeaderValid);
    CHECK(negative.ServerTick == 5);
    CHECK(negative.InputFeedback == -3);

    // A positive feedback (the client leading further than needed) likewise, and the default is zero.
    CHECK(ApplySnapshot(EncodeSnapshot(*server, 6, 0, 4), *client, map).InputFeedback == 4);
    CHECK(ApplySnapshot(EncodeSnapshot(*server, 7, 0), *client, map).InputFeedback == 0);
}

TEST_CASE("The snapshot header carries the last-consumed input tick, beside the feedback")
{
    TypeRegistry serverTypes;
    RegisterBuiltinTypes(serverTypes);
    Unique<Scene> server = Scene::Create(serverTypes);
    const Entity e = server->CreateEntity();
    server->Add<Transform>(e, Transform{.Position = vec3(1.0f)});

    NetIdAllocator allocator;
    AssignServerNetIds(*server, allocator);

    TypeRegistry clientTypes;
    RegisterBuiltinTypes(clientTypes);
    Unique<Scene> client = Scene::Create(clientTypes);
    NetIdMap map;
    SpawnClientMirror(*client, map, server->Get<NetIdentity>(e).Id);

    // The consumed-input tick rides the header additively beside the feedback and round-trips
    // independently of it (feedback -2, consumed 41).
    const SnapshotApplyResult applied =
        ApplySnapshot(EncodeSnapshot(*server, 9, 0, -2, 41), *client, map);
    CHECK(applied.HeaderValid);
    CHECK(applied.ServerTick == 9);
    CHECK(applied.InputFeedback == -2);
    CHECK(applied.LastConsumedInputTick == 41);

    // Defaulted to zero when the encoder omits it.
    CHECK(ApplySnapshot(EncodeSnapshot(*server, 10, 0), *client, map).LastConsumedInputTick == 0);
}

TEST_CASE("Change ticks are untouched by const iteration but stamped by a mutable access")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    Unique<Scene> scene = Scene::Create(types);

    scene->SetChangeTick(4);
    const Entity e = scene->CreateEntity();
    scene->Add<Transform>(e);
    CHECK(scene->GetComponentChangeTick(e, TypeIdOf<Transform>()) == 4);

    // A const walk reads without dirtying: the change tick stays put even though the "current tick"
    // has since advanced.
    scene->SetChangeTick(9);
    const Scene& constScene = *scene;
    for (auto [entity, transform] : constScene.View<Transform>())
    {
        (void)entity;
        CHECK(transform.Position.x == doctest::Approx(0.0f));
    }
    CHECK(scene->GetComponentChangeTick(e, TypeIdOf<Transform>()) == 4);

    // A non-const access stamps the current tick, per entity.
    scene->Get<Transform>(e).Position.x = 1.0f;
    CHECK(scene->GetComponentChangeTick(e, TypeIdOf<Transform>()) == 9);
}
