// The admission profile: the opaque, bounded, host-terminal account payload a client presents at
// connect. Driven device-free over the in-process transports with time injected through Pump(now).
// Covered: the connect-request codec round-trips arbitrary bytes untouched and an absent profile
// costs only the empty blob header; a presented profile surfaces on ServerHost::ProfileOf and on
// the Net::JoinRequestInfo the Authorize hook reads; absence resolves nullptr on both; a reconnect
// re-presents (the fresh profile winning) and a teardown clears only the entry its own connection
// owns; a listen host's own account resolves its profile with no connect; an over-budget profile is
// refused locally with DenyReason::ProfileTooLarge while a budget-filling one still connects; and no
// peer's datagrams ever carry another account's profile bytes.

#include <doctest/doctest.h>

#include <Veng/Net/Client.h>
#include <Veng/Net/Host.h>
#include <Veng/Net/LoopbackTransport.h>
#include <Veng/Net/Server.h>
#include <Veng/Net/Transport.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Scene.h>

#include "Net/Handshake.h"

#include <algorithm>
#include <deque>
#include <unordered_map>
#include <utility>

using namespace Veng;
using namespace Veng::Net;

namespace
{
    // A dependency-free seat spawn never dereferences the manager, so a never-dereferenced
    // reference is safe (the net_join_flow.cpp / game_mode.cpp precedent).
    AssetManager& FakeAssets()
    {
        alignas(16) static unsigned char bytes[64]{};
        return *reinterpret_cast<AssetManager*>(bytes);
    }

    const ConnectionConfig Config{
        .ResendInterval = 0.02, .KeepaliveInterval = 0.05, .TimeoutInterval = 1.0};

    constexpr AssetId LevelId{0x00000000000000AAULL};
    constexpr TypeId ProfileType = 0x00000000000000C1ULL;

    Blob MakeProfile(u8 seed, usize size)
    {
        Blob profile{.Type = ProfileType};
        profile.Bytes.reserve(size);
        for (usize i = 0; i < size; ++i)
        {
            profile.Bytes.push_back(static_cast<u8>(seed + i));
        }
        return profile;
    }

    // A multi-peer in-process medium (the net_lifecycle.cpp Hub) that also records, per destination
    // endpoint, every datagram body the server sent there — what the peer-isolation case scans.
    struct Hub
    {
        struct Packet
        {
            EndpointId From = EndpointId::None;
            vector<u8> Bytes;
        };

        std::unordered_map<u32, std::deque<Packet>> Queues;
        std::unordered_map<u32, vector<vector<u8>>> Delivered;
        u32 NextEndpoint = 1;

        u32 Register()
        {
            const u32 id = NextEndpoint;
            NextEndpoint += 1;
            Queues[id];
            return id;
        }
    };

    class HubTransport final : public Transport
    {
    public:
        HubTransport(Ref<Hub> hub, u32 self, u32 resolveTo)
            : m_Hub(std::move(hub)), m_Self(self), m_ResolveTo(resolveTo)
        {
        }

        VoidResult Send(EndpointId to, std::span<const u8> bytes) override
        {
            const auto it = m_Hub->Queues.find(static_cast<u32>(to));
            if (it != m_Hub->Queues.end())
            {
                it->second.push_back(Hub::Packet{.From = static_cast<EndpointId>(m_Self),
                                                 .Bytes = vector<u8>(bytes.begin(), bytes.end())});
                m_Hub->Delivered[static_cast<u32>(to)].emplace_back(bytes.begin(), bytes.end());
            }
            return {};
        }

        optional<Datagram> Receive() override
        {
            std::deque<Hub::Packet>& queue = m_Hub->Queues[m_Self];
            if (queue.empty())
            {
                return {};
            }
            m_Scratch = std::move(queue.front().Bytes);
            const EndpointId from = queue.front().From;
            queue.pop_front();
            return Datagram{.From = from, .Bytes = m_Scratch};
        }

        Result<EndpointId> Resolve(string_view, u16) override
        {
            return static_cast<EndpointId>(m_ResolveTo);
        }

    private:
        Ref<Hub> m_Hub;
        u32 m_Self;
        u32 m_ResolveTo;
        vector<u8> m_Scratch;
    };

    // True if @p needle appears anywhere in @p haystack — the byte-level "this never crossed" probe.
    bool Contains(std::span<const u8> haystack, const vector<u8>& needle)
    {
        return std::ranges::search(haystack, needle).begin() != haystack.end();
    }
}

TEST_CASE("The connect request round-trips an account profile's bytes untouched")
{
    const Blob profile = MakeProfile(0x11, 96);
    const ConnectRequestMessage message{
        .ProtocolVersion = ProtocolVersion,
        .Content = ContentDigest{.Lo = 0xAB, .Hi = 0xCD},
        .AppVersion = 7,
        .Account = AccountId{.Lo = 3, .Hi = 4},
        .Profile = profile,
    };

    const optional<ConnectRequestMessage> decoded =
        DecodeConnectRequest(EncodeConnectRequest(message));
    REQUIRE(decoded.has_value());
    CHECK(decoded->Account == message.Account);
    CHECK(decoded->Profile.Type == ProfileType);
    CHECK(decoded->Profile.Bytes == profile.Bytes);
}

TEST_CASE("An absent profile costs only the empty blob header, and the budget matches the framing")
{
    const ConnectRequestMessage bare{.ProtocolVersion = ProtocolVersion};
    const vector<u8> encoded = EncodeConnectRequest(bare);
    CHECK(encoded.size() == ConnectRequestOverhead);

    const optional<ConnectRequestMessage> decoded = DecodeConnectRequest(encoded);
    REQUIRE(decoded.has_value());
    CHECK(decoded->Profile.Bytes.empty());
    CHECK(decoded->Profile.Type == InvalidTypeId);

    // A profile filling the budget exactly still fits one reliable message; one byte more does not.
    const ConnectRequestMessage full{.ProtocolVersion = ProtocolVersion,
                                     .Profile = MakeProfile(0x20, MaxProfileBytes)};
    CHECK(EncodeConnectRequest(full).size() == MaxReliableMessageSize);
}

TEST_CASE("A presented profile reaches ProfileOf and the join request the Authorize hook reads")
{
    auto [serverT, clientT] = LoopbackTransport::CreatePair();

    TypeRegistry serverTypes;
    RegisterBuiltinTypes(serverTypes);
    Unique<Scene> serverScene = Scene::Create(serverTypes);

    Blob authorized;
    bool sawProfile = false;
    Result<Unique<ServerHost>> host = ServerHost::Create(ServerHostInfo{
        .Server = ServerInfo{.TransportOverride = serverT.get(), .Connection = Config},
        .World = *serverScene,
        .Assets = FakeAssets(),
        .LevelId = LevelId,
        .Authorize =
            [&](const JoinRequestInfo& request)
        {
            sawProfile = request.Profile != nullptr;
            if (request.Profile != nullptr)
            {
                authorized = *request.Profile;
            }
            return true;
        },
    });
    REQUIRE(host.has_value());

    const Blob profile = MakeProfile(0x31, 48);
    const AccountId account{.Lo = 0xF00D, .Hi = 0xBEEF};
    Unique<Client> client = *Client::Connect(ClientInfo{.Account = account,
                                                        .Profile = profile,
                                                        .TransportOverride = clientT.get(),
                                                        .Connection = Config});

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
    for (u64 tick = 1; tick <= 16 && !clientHost->IsJoined(); ++tick)
    {
        now += 1.0 / 60.0;
        serverScene->SetChangeTick(tick);
        (*host)->Pump(now, tick);
        clientHost->Pump(now);
    }
    REQUIRE(clientHost->IsJoined());

    // The host holds it per admitted account, byte-identical to what the client presented.
    const Blob* held = (*host)->ProfileOf(account);
    REQUIRE(held != nullptr);
    CHECK(held->Type == ProfileType);
    CHECK(held->Bytes == profile.Bytes);

    // And the same blob rode the join request the policy hooks read.
    CHECK(sawProfile);
    CHECK(authorized == profile);

    // An account that never connected holds nothing.
    CHECK((*host)->ProfileOf(AccountId{.Lo = 0x1234, .Hi = 0}) == nullptr);
}

TEST_CASE("An account presenting no profile resolves nullptr on both surfaces")
{
    auto [serverT, clientT] = LoopbackTransport::CreatePair();

    TypeRegistry serverTypes;
    RegisterBuiltinTypes(serverTypes);
    Unique<Scene> serverScene = Scene::Create(serverTypes);

    bool authorizeRan = false;
    const Blob* seen = reinterpret_cast<const Blob*>(1);
    Result<Unique<ServerHost>> host = ServerHost::Create(ServerHostInfo{
        .Server = ServerInfo{.TransportOverride = serverT.get(), .Connection = Config},
        .World = *serverScene,
        .Assets = FakeAssets(),
        .LevelId = LevelId,
        .Authorize =
            [&](const JoinRequestInfo& request)
        {
            authorizeRan = true;
            seen = request.Profile;
            return true;
        },
    });
    REQUIRE(host.has_value());

    const AccountId account{.Lo = 9, .Hi = 9};
    Unique<Client> client = *Client::Connect(
        ClientInfo{.Account = account, .TransportOverride = clientT.get(), .Connection = Config});

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
    for (u64 tick = 1; tick <= 16 && !clientHost->IsJoined(); ++tick)
    {
        now += 1.0 / 60.0;
        serverScene->SetChangeTick(tick);
        (*host)->Pump(now, tick);
        clientHost->Pump(now);
    }
    REQUIRE(clientHost->IsJoined());

    CHECK(authorizeRan);
    CHECK(seen == nullptr);
    CHECK((*host)->ProfileOf(account) == nullptr);
}

TEST_CASE("A reconnect re-presents the profile, and the fresh connection's entry wins")
{
    TypeRegistry serverTypes;
    RegisterBuiltinTypes(serverTypes);
    Unique<Scene> serverScene = Scene::Create(serverTypes);

    const auto hub = CreateRef<Hub>();
    const u32 serverEndpoint = hub->Register();
    HubTransport serverTransport(hub, serverEndpoint, 0);

    Result<Unique<ServerHost>> host = ServerHost::Create(ServerHostInfo{
        .Server = ServerInfo{.TransportOverride = &serverTransport, .Connection = Config},
        .World = *serverScene,
        .Assets = FakeAssets(),
        .LevelId = LevelId,
    });
    REQUIRE(host.has_value());

    const AccountId account{.Lo = 0x5150, .Hi = 0x1984};
    const Blob first = MakeProfile(0x40, 24);
    const Blob second = MakeProfile(0x80, 40);

    f64 now = 0.0;
    const auto step = [&](u64 ticks, Client* client)
    {
        for (u64 i = 0; i < ticks; ++i)
        {
            now += 1.0 / 60.0;
            (*host)->Pump(now, 1);
            if (client != nullptr)
            {
                client->Pump(now);
            }
        }
    };

    {
        const u32 endpoint = hub->Register();
        HubTransport transport(hub, endpoint, serverEndpoint);
        Unique<Client> client = *Client::Connect(ClientInfo{.Account = account,
                                                            .Profile = first,
                                                            .TransportOverride = &transport,
                                                            .Connection = Config});
        step(12, client.get());
        REQUIRE(client->State() == ClientState::Connected);
        const Blob* held = (*host)->ProfileOf(account);
        REQUIRE(held != nullptr);
        CHECK(held->Bytes == first.Bytes);

        client->Disconnect();
        step(4, client.get());
    }

    // The disconnect cleared the entry the departed connection owned.
    CHECK((*host)->ProfileOf(account) == nullptr);

    {
        const u32 endpoint = hub->Register();
        HubTransport transport(hub, endpoint, serverEndpoint);
        Unique<Client> client = *Client::Connect(ClientInfo{.Account = account,
                                                            .Profile = second,
                                                            .TransportOverride = &transport,
                                                            .Connection = Config});
        step(12, client.get());
        REQUIRE(client->State() == ClientState::Connected);
        const Blob* held = (*host)->ProfileOf(account);
        REQUIRE(held != nullptr);
        CHECK(held->Bytes == second.Bytes);
    }
}

TEST_CASE("A listen host's own account resolves its profile with no connect")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    Unique<Scene> scene = Scene::Create(types);

    auto [serverT, clientT] = LoopbackTransport::CreatePair();

    const AccountId local{.Lo = 0xAA, .Hi = 0xBB};
    const Blob profile = MakeProfile(0x55, 32);

    Result<Unique<ServerHost>> host = ServerHost::Create(ServerHostInfo{
        .Server = ServerInfo{.TransportOverride = serverT.get(), .Connection = Config},
        .World = *scene,
        .Assets = FakeAssets(),
        .LevelId = LevelId,
        .LocalAccount = local,
        .LocalProfile = profile,
    });
    REQUIRE(host.has_value());

    const Blob* held = (*host)->ProfileOf(local);
    REQUIRE(held != nullptr);
    CHECK(held->Bytes == profile.Bytes);

    // A host whose local player presents nothing holds nothing for it.
    Result<Unique<ServerHost>> bare = ServerHost::Create(ServerHostInfo{
        .Server = ServerInfo{.TransportOverride = clientT.get(), .Connection = Config},
        .World = *scene,
        .Assets = FakeAssets(),
        .LevelId = LevelId,
        .LocalAccount = local,
    });
    REQUIRE(bare.has_value());
    CHECK((*bare)->ProfileOf(local) == nullptr);
}

TEST_CASE("An over-budget profile refuses the connect at the client rather than truncating")
{
    auto [serverT, clientT] = LoopbackTransport::CreatePair();

    Unique<Server> server =
        *Server::Create(ServerInfo{.TransportOverride = serverT.get(), .Connection = Config});
    Unique<Client> client = *Client::Connect(ClientInfo{
        .Profile = MakeProfile(0x60, MaxProfileBytes + 1),
        .TransportOverride = clientT.get(),
        .Connection = Config,
    });

    CHECK(client->State() == ClientState::Denied);
    REQUIRE(client->GetDenyReason().has_value());
    CHECK(*client->GetDenyReason() == DenyReason::ProfileTooLarge);

    // Nothing was sent, so the server never saw a request.
    f64 now = 0.0;
    for (int tick = 0; tick < 8; ++tick)
    {
        now += 0.02;
        server->Pump(now);
        client->Pump(now);
    }
    CHECK(server->Connections().empty());
}

TEST_CASE("A profile filling the budget exactly still connects, and one byte more cannot be sent")
{
    auto [serverT, clientT] = LoopbackTransport::CreatePair();

    const Blob full = MakeProfile(0x70, MaxProfileBytes);
    const AccountId account{.Lo = 0x7777, .Hi = 0x8888};
    Unique<Server> server =
        *Server::Create(ServerInfo{.TransportOverride = serverT.get(), .Connection = Config});
    Unique<Client> client = *Client::Connect(ClientInfo{.Account = account,
                                                        .Profile = full,
                                                        .TransportOverride = clientT.get(),
                                                        .Connection = Config});

    f64 now = 0.0;
    for (int tick = 0; tick < 40 && client->State() == ClientState::Connecting; ++tick)
    {
        now += 0.02;
        server->Pump(now);
        client->Pump(now);
    }

    REQUIRE(client->State() == ClientState::Connected);
    const Blob* held = server->ProfileFor(client->AssignedId());
    REQUIRE(held != nullptr);
    CHECK(held->Bytes == full.Bytes);

    // The budget is exactly the reliable channel's per-message bound minus the request framing, so
    // one byte past it no longer fits a single reliable message and Send refuses it outright — the
    // host's own door check covers a peer whose framing disagrees, which no in-process peer can be.
    const Result<EndpointId> peer = clientT->Resolve("", 0);
    REQUIRE(peer.has_value());
    Connection raw(*clientT, *peer, Config);
    const vector<u8> oversize = EncodeConnectRequest(ConnectRequestMessage{
        .ProtocolVersion = ProtocolVersion, .Profile = MakeProfile(0x90, MaxProfileBytes + 1)});
    CHECK(oversize.size() > MaxReliableMessageSize);
    CHECK_FALSE(raw.Send(Channel::ReliableOrdered, oversize).has_value());
}

TEST_CASE("No peer's datagrams ever carry another account's profile")
{
    TypeRegistry serverTypes;
    RegisterBuiltinTypes(serverTypes);
    Unique<Scene> serverScene = Scene::Create(serverTypes);

    const auto hub = CreateRef<Hub>();
    const u32 serverEndpoint = hub->Register();
    HubTransport serverTransport(hub, serverEndpoint, 0);

    Result<Unique<ServerHost>> host = ServerHost::Create(ServerHostInfo{
        .Server = ServerInfo{.TransportOverride = &serverTransport, .Connection = Config},
        .World = *serverScene,
        .Assets = FakeAssets(),
        .LevelId = LevelId,
    });
    REQUIRE(host.has_value());

    const u32 endpointA = hub->Register();
    const u32 endpointB = hub->Register();
    HubTransport transportA(hub, endpointA, serverEndpoint);
    HubTransport transportB(hub, endpointB, serverEndpoint);

    const Blob profileA = MakeProfile(0x91, 64);
    const Blob profileB = MakeProfile(0xC1, 64);

    Unique<Client> clientA = *Client::Connect(ClientInfo{.Account = AccountId{.Lo = 1, .Hi = 0},
                                                         .Profile = profileA,
                                                         .TransportOverride = &transportA,
                                                         .Connection = Config});
    Unique<Client> clientB = *Client::Connect(ClientInfo{.Account = AccountId{.Lo = 2, .Hi = 0},
                                                         .Profile = profileB,
                                                         .TransportOverride = &transportB,
                                                         .Connection = Config});

    TypeRegistry clientTypes;
    RegisterBuiltinTypes(clientTypes);
    Unique<Scene> sceneA;
    Unique<Scene> sceneB;
    Unique<ClientHost> hostA = ClientHost::Create(ClientHostInfo{
        .Client = *clientA,
        .Assets = FakeAssets(),
        .LoadLevel = [&](AssetId) -> Scene*
        {
            sceneA = Scene::Create(clientTypes);
            return sceneA.get();
        },
        .ResolvePrefab = [](AssetId) -> Ref<Prefab> { return nullptr; },
    });
    Unique<ClientHost> hostB = ClientHost::Create(ClientHostInfo{
        .Client = *clientB,
        .Assets = FakeAssets(),
        .LoadLevel = [&](AssetId) -> Scene*
        {
            sceneB = Scene::Create(clientTypes);
            return sceneB.get();
        },
        .ResolvePrefab = [](AssetId) -> Ref<Prefab> { return nullptr; },
    });

    f64 now = 0.0;
    for (u64 tick = 1; tick <= 40; ++tick)
    {
        now += 1.0 / 60.0;
        serverScene->SetChangeTick(tick);
        (*host)->Pump(now, tick);
        hostA->Pump(now);
        hostB->Pump(now);
    }
    REQUIRE(hostA->IsJoined());
    REQUIRE(hostB->IsJoined());

    // The profile terminates at the host: every datagram it addressed to one peer is free of the
    // other's profile bytes, and a peer never sees its own echoed back either.
    for (const vector<u8>& datagram : hub->Delivered[endpointA])
    {
        CHECK_FALSE(Contains(datagram, profileB.Bytes));
        CHECK_FALSE(Contains(datagram, profileA.Bytes));
    }
    for (const vector<u8>& datagram : hub->Delivered[endpointB])
    {
        CHECK_FALSE(Contains(datagram, profileA.Bytes));
        CHECK_FALSE(Contains(datagram, profileB.Bytes));
    }
}
