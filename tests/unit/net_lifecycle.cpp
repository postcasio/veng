// Connection-lifecycle cases: Net::Server + Net::Client over the in-process
// transports, with time injected through Pump(now) so every case scripts its own
// clock — deterministic, device-free, no wall clock. Single-peer cases run over the
// delivered LoopbackTransport (and, where the point is loss tolerance, the
// FaultInjectionTransport wrapper); the multi-client cases (server-full, id
// monotonicity, no-reuse) run over a small test-local multi-peer hub, since a
// LoopbackTransport pair is strictly point-to-point. Covered: handshake accept with
// events fired both ends and a nonzero assigned id; protocol / content / server-full
// / app-refused deny arms; connection-id assignment (monotonic, distinct, no reuse);
// disconnect both directions (kick and leave); timeout reaping both ends; and
// handshake completion under seeded packet loss.

#include <doctest/doctest.h>

#include <Veng/Net/Client.h>
#include <Veng/Net/FaultInjectionTransport.h>
#include <Veng/Net/LoopbackTransport.h>
#include <Veng/Net/Server.h>
#include <Veng/Net/Transport.h>

#include <deque>
#include <unordered_map>
#include <utility>

using namespace Veng;
using namespace Veng::Net;

namespace
{
    // A multi-peer in-process medium: one inbound queue per endpoint handle. Send
    // appends to the destination's queue tagged with the sender; Receive pops the
    // caller's own. It is the LoopbackTransport medium generalized past a pair, so a
    // single server transport can talk to N client transports at once.
    struct Hub
    {
        struct Packet
        {
            EndpointId From = EndpointId::None;
            vector<u8> Bytes;
        };

        std::unordered_map<u32, std::deque<Packet>> Queues;
        u32 NextEndpoint = 1;

        u32 Register()
        {
            const u32 id = NextEndpoint;
            NextEndpoint += 1;
            Queues[id];
            return id;
        }
    };

    // A transport bound to one Hub endpoint. Resolve returns a fixed target (the
    // server's endpoint for a client), so clients address the server and the server
    // addresses each client by the From it learned.
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

    // A short timing config that keeps the lifecycle cases fast (sub-second timeout).
    ConnectionConfig FastConfig()
    {
        return ConnectionConfig{
            .ResendInterval = 0.05,
            .ResendBackoffMax = 0.2,
            .KeepaliveInterval = 0.1,
            .TimeoutInterval = 1.0,
        };
    }

    ContentDigest Digest(u64 lo, u64 hi)
    {
        return ContentDigest{.Lo = lo, .Hi = hi};
    }
}

TEST_CASE("a matching handshake is accepted, firing events at both ends")
{
    auto [serverT, clientT] = LoopbackTransport::CreatePair();

    const ContentDigest content = Digest(0xABCD, 0x1234);
    Unique<Server> server = *Server::Create(ServerInfo{
        .Content = content, .TransportOverride = serverT.get(), .Connection = FastConfig()});
    Unique<Client> client = *Client::Connect(ClientInfo{
        .Content = content, .TransportOverride = clientT.get(), .Connection = FastConfig()});

    bool serverSawConnect = false;
    ConnectionId serverId = ServerConnectionId;

    f64 now = 0.0;
    for (int tick = 0; tick < 40 && client->State() == ClientState::Connecting; ++tick)
    {
        now += 0.02;
        server->Pump(now);
        client->Pump(now);
        for (const NetEvent& e : server->Events())
        {
            if (e.Type == NetEventType::Connected)
            {
                serverSawConnect = true;
                serverId = e.Id;
            }
        }
    }

    CHECK(client->State() == ClientState::Connected);
    CHECK(client->AssignedId() != ServerConnectionId);
    CHECK(serverSawConnect);
    CHECK(serverId == client->AssignedId());
    REQUIRE(server->Connections().size() == 1);
    CHECK(server->Connections()[0] == client->AssignedId());
}

TEST_CASE("a protocol-version mismatch is denied loudly")
{
    auto [serverT, clientT] = LoopbackTransport::CreatePair();

    Unique<Server> server = *Server::Create(ServerInfo{
        .ProtocolVersion = 7, .TransportOverride = serverT.get(), .Connection = FastConfig()});
    Unique<Client> client = *Client::Connect(ClientInfo{
        .ProtocolVersion = 8, .TransportOverride = clientT.get(), .Connection = FastConfig()});

    f64 now = 0.0;
    for (int tick = 0; tick < 40 && client->State() == ClientState::Connecting; ++tick)
    {
        now += 0.02;
        server->Pump(now);
        client->Pump(now);
    }

    CHECK(client->State() == ClientState::Denied);
    REQUIRE(client->GetDenyReason().has_value());
    CHECK(*client->GetDenyReason() == DenyReason::ProtocolMismatch);
    CHECK(server->Connections().empty());
}

TEST_CASE("a content-digest mismatch is denied")
{
    auto [serverT, clientT] = LoopbackTransport::CreatePair();

    Unique<Server> server = *Server::Create(ServerInfo{
        .Content = Digest(1, 2), .TransportOverride = serverT.get(), .Connection = FastConfig()});
    Unique<Client> client = *Client::Connect(ClientInfo{
        .Content = Digest(9, 9), .TransportOverride = clientT.get(), .Connection = FastConfig()});

    f64 now = 0.0;
    for (int tick = 0; tick < 40 && client->State() == ClientState::Connecting; ++tick)
    {
        now += 0.02;
        server->Pump(now);
        client->Pump(now);
    }

    CHECK(client->State() == ClientState::Denied);
    REQUIRE(client->GetDenyReason().has_value());
    CHECK(*client->GetDenyReason() == DenyReason::ContentMismatch);
    CHECK(server->Connections().empty());
}

TEST_CASE("an app connect-policy hook refuses a request")
{
    auto [serverT, clientT] = LoopbackTransport::CreatePair();

    Unique<Server> server = *Server::Create(ServerInfo{
        .OnConnectRequest = [](const ConnectRequestInfo& info) { return info.AppVersion == 42; },
        .TransportOverride = serverT.get(),
        .Connection = FastConfig()});
    Unique<Client> client = *Client::Connect(ClientInfo{.AppVersion = 7, // not 42 — refused
                                                        .TransportOverride = clientT.get(),
                                                        .Connection = FastConfig()});

    f64 now = 0.0;
    for (int tick = 0; tick < 40 && client->State() == ClientState::Connecting; ++tick)
    {
        now += 0.02;
        server->Pump(now);
        client->Pump(now);
    }

    CHECK(client->State() == ClientState::Denied);
    REQUIRE(client->GetDenyReason().has_value());
    CHECK(*client->GetDenyReason() == DenyReason::AppRefused);
}

TEST_CASE("connection ids are assigned monotonically and a full server is refused")
{
    const auto hub = CreateRef<Hub>();
    const u32 serverEndpoint = hub->Register();
    auto serverT = CreateUnique<HubTransport>(hub, serverEndpoint, serverEndpoint);

    Unique<Server> server = *Server::Create(ServerInfo{
        .MaxConnections = 2, .TransportOverride = serverT.get(), .Connection = FastConfig()});

    std::vector<Unique<HubTransport>> clientTransports;
    std::vector<Unique<Client>> clients;
    for (int i = 0; i < 3; ++i)
    {
        clientTransports.push_back(
            CreateUnique<HubTransport>(hub, hub->Register(), serverEndpoint));
        clients.push_back(*Client::Connect(ClientInfo{
            .TransportOverride = clientTransports.back().get(), .Connection = FastConfig()}));
    }

    f64 now = 0.0;
    for (int tick = 0; tick < 60; ++tick)
    {
        now += 0.02;
        server->Pump(now);
        for (Unique<Client>& c : clients)
        {
            c->Pump(now);
        }
    }

    int accepted = 0;
    int refused = 0;
    std::vector<ConnectionId> ids;
    for (Unique<Client>& c : clients)
    {
        if (c->State() == ClientState::Connected)
        {
            accepted += 1;
            ids.push_back(c->AssignedId());
        }
        else if (c->State() == ClientState::Denied)
        {
            refused += 1;
            CHECK(*c->GetDenyReason() == DenyReason::ServerFull);
        }
    }

    CHECK(accepted == 2);
    CHECK(refused == 1);
    REQUIRE(ids.size() == 2);
    CHECK(ids[0] != ids[1]);
    CHECK(ids[0] != ServerConnectionId);
    CHECK(ids[1] != ServerConnectionId);
    CHECK(server->Connections().size() == 2);
}

TEST_CASE("a connection id is never reused after a disconnect")
{
    const auto hub = CreateRef<Hub>();
    const u32 serverEndpoint = hub->Register();
    auto serverT = CreateUnique<HubTransport>(hub, serverEndpoint, serverEndpoint);
    Unique<Server> server =
        *Server::Create(ServerInfo{.TransportOverride = serverT.get(), .Connection = FastConfig()});

    const auto connect = [&](f64& now) -> ConnectionId
    {
        auto transport = CreateUnique<HubTransport>(hub, hub->Register(), serverEndpoint);
        Unique<Client> client = *Client::Connect(
            ClientInfo{.TransportOverride = transport.get(), .Connection = FastConfig()});
        for (int tick = 0; tick < 40 && client->State() == ClientState::Connecting; ++tick)
        {
            now += 0.02;
            server->Pump(now);
            client->Pump(now);
        }
        REQUIRE(client->State() == ClientState::Connected);
        const ConnectionId id = client->AssignedId();
        // Client leaves; drive the server past the graceful close so the slot frees.
        client->Disconnect();
        for (int tick = 0; tick < 5; ++tick)
        {
            now += 0.02;
            server->Pump(now);
            client->Pump(now);
        }
        return id;
    };

    f64 now = 0.0;
    const ConnectionId first = connect(now);
    const ConnectionId second = connect(now);
    CHECK(first != ServerConnectionId);
    CHECK(second != first);
}

TEST_CASE("the server can kick a connection, surfacing a Disconnected event")
{
    auto [serverT, clientT] = LoopbackTransport::CreatePair();
    Unique<Server> server =
        *Server::Create(ServerInfo{.TransportOverride = serverT.get(), .Connection = FastConfig()});
    Unique<Client> client = *Client::Connect(
        ClientInfo{.TransportOverride = clientT.get(), .Connection = FastConfig()});

    f64 now = 0.0;
    while (client->State() == ClientState::Connecting)
    {
        now += 0.02;
        server->Pump(now);
        client->Pump(now);
    }
    REQUIRE(client->State() == ClientState::Connected);
    const ConnectionId id = client->AssignedId();

    REQUIRE(server->Disconnect(id, DisconnectReason::Kicked).has_value());
    CHECK_FALSE(server->Disconnect(9999, DisconnectReason::Kicked).has_value()); // unknown id

    bool sawKick = false;
    for (int tick = 0; tick < 20; ++tick)
    {
        now += 0.02;
        server->Pump(now);
        client->Pump(now);
        for (const NetEvent& e : server->Events())
        {
            if (e.Type == NetEventType::Disconnected && e.Id == id)
            {
                sawKick = true;
                CHECK(e.Reason == DisconnectReason::Kicked);
            }
        }
    }

    CHECK(sawKick);
    CHECK(server->Connections().empty());
    CHECK(client->State() == ClientState::Lost);
}

TEST_CASE("a client can leave, surfacing a Left event on the server")
{
    auto [serverT, clientT] = LoopbackTransport::CreatePair();
    Unique<Server> server =
        *Server::Create(ServerInfo{.TransportOverride = serverT.get(), .Connection = FastConfig()});
    Unique<Client> client = *Client::Connect(
        ClientInfo{.TransportOverride = clientT.get(), .Connection = FastConfig()});

    f64 now = 0.0;
    while (client->State() == ClientState::Connecting)
    {
        now += 0.02;
        server->Pump(now);
        client->Pump(now);
    }
    const ConnectionId id = client->AssignedId();

    client->Disconnect();
    CHECK(client->State() == ClientState::Lost);

    bool sawLeft = false;
    for (int tick = 0; tick < 20; ++tick)
    {
        now += 0.02;
        client->Pump(now);
        server->Pump(now);
        for (const NetEvent& e : server->Events())
        {
            if (e.Type == NetEventType::Disconnected && e.Id == id)
            {
                sawLeft = true;
                CHECK(e.Reason == DisconnectReason::Left);
            }
        }
    }

    CHECK(sawLeft);
    CHECK(server->Connections().empty());
}

TEST_CASE("a silent peer trips the timeout at both ends")
{
    SUBCASE("the server reaps a connection that goes silent")
    {
        auto [serverT, clientT] = LoopbackTransport::CreatePair();
        Unique<Server> server = *Server::Create(
            ServerInfo{.TransportOverride = serverT.get(), .Connection = FastConfig()});
        Unique<Client> client = *Client::Connect(
            ClientInfo{.TransportOverride = clientT.get(), .Connection = FastConfig()});

        f64 now = 0.0;
        while (client->State() == ClientState::Connecting)
        {
            now += 0.02;
            server->Pump(now);
            client->Pump(now);
        }
        const ConnectionId id = client->AssignedId();

        // The client vanishes; the server pumps alone past the 1s timeout interval.
        bool sawTimeout = false;
        for (int tick = 0; tick < 120; ++tick)
        {
            now += 0.02;
            server->Pump(now);
            for (const NetEvent& e : server->Events())
            {
                if (e.Type == NetEventType::Disconnected && e.Id == id)
                {
                    sawTimeout = true;
                    CHECK(e.Reason == DisconnectReason::Timeout);
                }
            }
        }

        CHECK(sawTimeout);
        CHECK(server->Connections().empty());
    }

    SUBCASE("a client whose server never answers goes Lost")
    {
        auto [serverT, clientT] = LoopbackTransport::CreatePair();
        (void)serverT; // no server pumps this end
        Unique<Client> client = *Client::Connect(
            ClientInfo{.TransportOverride = clientT.get(), .Connection = FastConfig()});

        f64 now = 0.0;
        for (int tick = 0; tick < 120 && client->State() == ClientState::Connecting; ++tick)
        {
            now += 0.02;
            client->Pump(now);
        }

        CHECK(client->State() == ClientState::Lost);
    }
}

TEST_CASE("the handshake completes under seeded packet loss")
{
    auto [serverT, clientT] = LoopbackTransport::CreatePair();

    const FaultInjectionConfig faults{.DropRate = 0.4f, .Seed = 909};
    FaultInjectionTransport serverLink(*serverT, faults);
    FaultInjectionTransport clientLink(*clientT, faults);

    // A generous timeout so resends have room to punch through the lossy link.
    ConnectionConfig config = FastConfig();
    config.TimeoutInterval = 20.0;

    Unique<Server> server =
        *Server::Create(ServerInfo{.TransportOverride = &serverLink, .Connection = config});
    Unique<Client> client =
        *Client::Connect(ClientInfo{.TransportOverride = &clientLink, .Connection = config});

    bool serverSawConnect = false;
    f64 now = 0.0;
    for (int tick = 0; tick < 2000 && client->State() == ClientState::Connecting; ++tick)
    {
        now += 0.02;
        server->Pump(now);
        client->Pump(now);
        for (const NetEvent& e : server->Events())
        {
            if (e.Type == NetEventType::Connected)
            {
                serverSawConnect = true;
            }
        }
    }

    CHECK(client->State() == ClientState::Connected);
    CHECK(serverSawConnect);
    CHECK(server->Connections().size() == 1);
}
