// IsLocallyOwned and the three seat scans that route through it: a seat knows whether it is locally
// owned. Fast-band, no transport — a Scene built by hand exercises the predicate's three rules
// (a published LocalSeat marker, a host's Authority, the single-player default) and the
// prefer-locally-owned-then-first policy the presentation and input-stamp scans share. The
// integration that the net layer actually publishes the marker on the seat ClientHost::Seat names
// lives in net_two_world.cpp, over a real loopback.

#include <doctest/doctest.h>

#include <Veng/Net/InputFeed.h>
#include <Veng/Net/Replication.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/InputMappingSystem.h>
#include <Veng/Scene/Scene.h>

#include "ManagedRebind.h"

using namespace Veng;

namespace
{
    // The one action a stamped input carries, so a decode can tell two seats' inputs apart by value.
    constexpr ActionId MoveAction{0xA1};

    Entity MakeViewerSeat(Scene& scene)
    {
        const Entity seat = scene.CreateEntity();
        scene.Add<Viewer>(seat);
        return seat;
    }

    ActionState MoveState(const f32 x)
    {
        ActionState state;
        state.Actions.push_back(ActionSample{.Id = MoveAction, .Value = {x, 0.0f}});
        return state;
    }
}

TEST_CASE("IsLocallyOwned: nothing published resolves every seat locally")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    const Unique<Scene> scene = Scene::Create(types);

    // The single-player / minimal-template seat: no marker, no Authority.
    const Entity bare = MakeViewerSeat(*scene);
    CHECK(IsLocallyOwned(*scene, bare));

    // A host's own seat is Server-tier and unowned (Owner == 0), so it still resolves locally.
    const Entity ownSeat = MakeViewerSeat(*scene);
    scene->Add<Authority>(ownSeat, Authority{.Tier = Tier::Server, .Owner = 0});
    CHECK(IsLocallyOwned(*scene, ownSeat));
}

TEST_CASE("IsLocallyOwned: a host's seat a remote connection owns is not locally owned")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    const Unique<Scene> scene = Scene::Create(types);

    const Entity ownSeat = MakeViewerSeat(*scene);
    scene->Add<Authority>(ownSeat, Authority{.Tier = Tier::Server, .Owner = 0});
    const Entity joinerA = MakeViewerSeat(*scene);
    scene->Add<Authority>(joinerA, Authority{.Tier = Tier::Server, .Owner = 1});
    const Entity joinerB = MakeViewerSeat(*scene);
    scene->Add<Authority>(joinerB, Authority{.Tier = Tier::Server, .Owner = 2});

    CHECK(IsLocallyOwned(*scene, ownSeat));
    CHECK_FALSE(IsLocallyOwned(*scene, joinerA));
    CHECK_FALSE(IsLocallyOwned(*scene, joinerB));
}

TEST_CASE("IsLocallyOwned: a published LocalSeat marks exactly this peer's seat")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    const Unique<Scene> scene = Scene::Create(types);

    // Three replicated seats — none carry Authority, which does not cross the wire — and the marker
    // on the middle one. Three, not two: a "return the one that is not the first" bug would still
    // pass with two.
    const Entity peerA = MakeViewerSeat(*scene);
    const Entity mine = MakeViewerSeat(*scene);
    const Entity peerB = MakeViewerSeat(*scene);
    scene->Add<LocalSeat>(mine);

    CHECK_FALSE(IsLocallyOwned(*scene, peerA));
    CHECK(IsLocallyOwned(*scene, mine));
    CHECK_FALSE(IsLocallyOwned(*scene, peerB));
}

TEST_CASE("IsLocallyOwned: releasing the marker falls back cleanly to the local default")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    const Unique<Scene> scene = Scene::Create(types);

    const Entity mine = MakeViewerSeat(*scene);
    const Entity peer = MakeViewerSeat(*scene);
    scene->Add<LocalSeat>(mine);
    REQUIRE(IsLocallyOwned(*scene, mine));
    REQUIRE_FALSE(IsLocallyOwned(*scene, peer));

    // Release: with no marker left, every seat answers locally again rather than stranding the peer
    // on a dead handle's answer.
    scene->Remove<LocalSeat>(mine);
    CHECK(IsLocallyOwned(*scene, mine));
    CHECK(IsLocallyOwned(*scene, peer));
}

TEST_CASE("ResolvePresentationSeat prefers the locally-owned Viewer, else the first")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    const Unique<Scene> scene = Scene::Create(types);

    const Entity first = MakeViewerSeat(*scene);
    const Entity mine = MakeViewerSeat(*scene);
    const Entity last = MakeViewerSeat(*scene);
    (void)last;

    // No marker: the first Viewer wins, the single-seat behavior unchanged with several.
    CHECK(ResolvePresentationSeat(*scene, Entity::Null) == first);

    // Marker on the middle seat: the locally-owned one wins over iteration order.
    scene->Add<LocalSeat>(mine);
    CHECK(ResolvePresentationSeat(*scene, Entity::Null) == mine);

    // A live bound viewer is kept regardless of ownership — the rebind preserves the bound seat.
    CHECK(ResolvePresentationSeat(*scene, first) == first);
}

TEST_CASE("StampLocalSeatInput stamps the locally-owned seat, else the first")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    const Unique<Scene> scene = Scene::Create(types);

    const Entity seatA = MakeViewerSeat(*scene);
    scene->Add<SeatInput>(seatA);
    scene->Add<PlayerInput>(seatA).State = MoveState(0.25f);
    const Entity seatB = MakeViewerSeat(*scene);
    scene->Add<SeatInput>(seatB);
    scene->Add<PlayerInput>(seatB).State = MoveState(0.75f);

    // No marker: the first (SeatInput, PlayerInput) seat is stamped — today's behavior.
    {
        InputSendBuffer send(InputSendBuffer::Settings{.Redundancy = 1});
        StampLocalSeatInput(send, *scene, 1);
        const Result<InputPacket> packet = DecodeInputPacket(send.Encode(0, types), types);
        REQUIRE(packet.has_value());
        REQUIRE(packet->Inputs.size() == 1);
        CHECK(packet->Inputs.front().State.GetValue(MoveAction).x == doctest::Approx(0.25f));
    }

    // Marker on the second seat: it is stamped instead of the first.
    {
        scene->Add<LocalSeat>(seatB);
        InputSendBuffer send(InputSendBuffer::Settings{.Redundancy = 1});
        StampLocalSeatInput(send, *scene, 1);
        const Result<InputPacket> packet = DecodeInputPacket(send.Encode(0, types), types);
        REQUIRE(packet.has_value());
        REQUIRE(packet->Inputs.size() == 1);
        CHECK(packet->Inputs.front().State.GetValue(MoveAction).x == doctest::Approx(0.75f));
    }
}
