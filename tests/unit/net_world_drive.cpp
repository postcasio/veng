// Plan 07's world-drive input feed, exercised device-free over two in-process scenes on
// LoopbackTransport. The Application world loop threads Plan 05's buffers through the hosts: the
// client stamps its local seat's resolved input and sends the redundant window, the server ingests
// each connection's packet into a jitter buffer and feeds the buffered input into the seat at the
// matching tick. These are the InputFeed helpers Application drives — tested here with no window, no
// device, no Vulkan symbol touched.

#include <doctest/doctest.h>

#include <Veng/Net/Client.h>
#include <Veng/Net/Host.h>
#include <Veng/Net/InputFeed.h>
#include <Veng/Net/LoopbackTransport.h>
#include <Veng/Net/Replication.h>
#include <Veng/Net/Server.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

#include <glm/geometric.hpp>

#include <unordered_map>

using namespace Veng;
using namespace Veng::Net;

namespace
{
    constexpr ActionId MoveAction{0xA1};

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

    const ConnectionConfig Config{
        .ResendInterval = 0.02, .KeepaliveInterval = 0.05, .TimeoutInterval = 5.0};
}

TEST_CASE("StampLocalSeatInput records the first local seat's resolved input, or nothing")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    Unique<Scene> scene = Scene::Create(types);

    InputSendBuffer send;

    // No local input seat: the window stays empty (a header-only ack) — the spectator path.
    StampLocalSeatInput(send, *scene, /*clientTick=*/1);
    CHECK(send.Size() == 0);

    // A local input seat (SeatInput + PlayerInput) is stamped.
    const Entity seat = scene->CreateEntity();
    scene->Add<Viewer>(seat);
    scene->Add<SeatInput>(seat);
    scene->Add<PlayerInput>(seat).State = MoveState(vec2(1.0f, -1.0f));

    StampLocalSeatInput(send, *scene, /*clientTick=*/2);
    CHECK(send.Size() == 1);
}

TEST_CASE("The world-drive input feed carries a client's input into the server seat")
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

    // Drive the handshake to Connected — the accept spawns the connection's seat.
    f64 now = 0.0;
    for (u64 tick = 1; tick <= 8 && client->State() == ClientState::Connecting; ++tick)
    {
        now += 1.0 / 60.0;
        serverScene->SetChangeTick(tick);
        (*host)->Pump(now, tick);
        client->Pump(now);
    }
    REQUIRE(client->State() == ClientState::Connected);

    const ConnectionId id = client->AssignedId();
    const Entity seat = (*host)->SeatFor(id);
    REQUIRE_FALSE(seat.IsNull());

    // The client stamps a tick's input and sends the redundant window on the unreliable channel —
    // the client half of the world-drive input feed.
    InputSendBuffer send(InputSendBuffer::Settings{.Redundancy = 3});
    const vec2 move(0.5f, -0.25f);
    send.Stamp(/*clientTick=*/1, MoveState(move));
    (void)client->Server().Send(Channel::UnreliableSequenced, send.Encode(0, serverTypes));

    // The server receives, ingests into the connection's jitter buffer, then feeds the seat.
    std::unordered_map<ConnectionId, InputJitterBuffer> jitter;
    bool fed = false;
    for (u64 tick = 9; tick <= 20 && !fed; ++tick)
    {
        now += 1.0 / 60.0;
        serverScene->SetChangeTick(tick);
        client->Pump(now);
        (*host)->Pump(now, tick);
        IngestConnectionInputs(**host, jitter, InputJitterBuffer::Settings{}, serverTypes);
        FeedSeatInputs(**host, jitter, *serverScene);

        if (serverScene->Has<PlayerInput>(seat))
        {
            const vec2 fedMove = serverScene->Get<PlayerInput>(seat).GetValue(MoveAction);
            if (glm::length(fedMove - move) < 1e-5f)
            {
                fed = true;
            }
        }
    }

    // The wire input reached the seat's PlayerInput unchanged — the control system re-derives Intent
    // from it exactly as a local seat's.
    CHECK(fed);
    REQUIRE(serverScene->Has<PlayerInput>(seat));
    const vec2 result = serverScene->Get<PlayerInput>(seat).GetValue(MoveAction);
    CHECK(result.x == doctest::Approx(move.x));
    CHECK(result.y == doctest::Approx(move.y));
}
