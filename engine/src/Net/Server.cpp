#include <Veng/Net/Server.h>

#include <Veng/Assert.h>
#include <Veng/Log.h>
#include "Handshake.h"
#include <Veng/Net/Protocol.h>
#include <Veng/Net/UdpTransport.h>

#include <algorithm>
#include <deque>
#include <utility>

namespace Veng::Net
{
    namespace
    {
        // A per-connection transport view over the server's one shared transport.
        //
        // The server owns a single listen transport but a Connection is point-to-point, so each
        // connection reads through its own PeerLink: the server demultiplexes received datagrams
        // by source endpoint into the matching link's inbox, and the Connection's own Receive
        // drains that inbox. Sends pass straight through to the shared transport, addressed to the
        // peer the Connection resolved from its first datagram.
        class PeerLink final : public Transport
        {
        public:
            PeerLink(Transport& shared, EndpointId peer) : m_Shared(&shared), m_Peer(peer) {}

            VoidResult Send(EndpointId to, std::span<const u8> bytes) override
            {
                return m_Shared->Send(to, bytes);
            }

            optional<Datagram> Receive() override
            {
                if (m_Inbox.empty())
                {
                    return {};
                }
                m_Current = std::move(m_Inbox.front());
                m_Inbox.pop_front();
                return Datagram{.From = m_Peer, .Bytes = m_Current};
            }

            Result<EndpointId> Resolve(string_view, u16) override { return m_Peer; }

            // Queues a datagram the server demultiplexed to this peer.
            void Enqueue(std::span<const u8> bytes)
            {
                m_Inbox.emplace_back(bytes.begin(), bytes.end());
            }

        private:
            Transport* m_Shared;
            EndpointId m_Peer;
            std::deque<vector<u8>> m_Inbox;
            vector<u8> m_Current;
        };

        // One tracked peer: its link, its reliability layer, and its lifecycle flags.
        struct PeerConnection
        {
            EndpointId Peer = EndpointId::None;
            Unique<PeerLink> Link;
            Unique<Connection> Conn;
            ConnectionId Id = ServerConnectionId; // nonzero only once established
            AccountId Account;                    // the admitted account, valid once established
            bool Established = false;
            bool Closing = false; // Disconnect() called; flush the message, then reap
            DisconnectReason CloseReason = DisconnectReason::Kicked;
            bool Remove = false;            // marked for reaping this pump
            vector<vector<u8>> AppReliable; // non-handshake reliable messages received this pump
        };
    }

    struct Server::State
    {
        ServerInfo Info;
        Unique<Transport>
            OwnedInner; // the bound socket when a NetSim wrapper owns the listen transport
        Unique<Transport>
            OwnedTransport; // non-null when Create bound its own socket (or its NetSim wrapper)
        UdpTransport* OwnedUdp = nullptr;       // the bound socket, for LocalPort()
        FaultInjectionTransport* Sim = nullptr; // the --netsim wrapper, advanced each Pump
        Transport* Transport = nullptr;         // owned or overridden — the listen transport
        ConnectionId NextId = ServerConnectionId + 1;
        vector<Unique<PeerConnection>> Connections;
        vector<ConnectionId> EstablishedIds;
        vector<NetEvent> Events;

        [[nodiscard]] PeerConnection* FindByPeer(EndpointId peer)
        {
            for (Unique<PeerConnection>& c : Connections)
            {
                if (c->Peer == peer)
                {
                    return c.get();
                }
            }
            return nullptr;
        }

        [[nodiscard]] u32 EstablishedCount() const
        {
            u32 count = 0;
            for (const Unique<PeerConnection>& c : Connections)
            {
                if (c->Established && !c->Remove)
                {
                    count += 1;
                }
            }
            return count;
        }

        [[nodiscard]] u32 ProvisionalCount() const
        {
            u32 count = 0;
            for (const Unique<PeerConnection>& c : Connections)
            {
                if (!c->Established && !c->Remove)
                {
                    count += 1;
                }
            }
            return count;
        }

        PeerConnection& CreateProvisional(EndpointId peer)
        {
            auto record = CreateUnique<PeerConnection>();
            record->Peer = peer;
            record->Link = CreateUnique<PeerLink>(*Transport, peer);
            // Peer None: the Connection adopts the endpoint from its first (demuxed) datagram.
            record->Conn =
                CreateUnique<Connection>(*record->Link, EndpointId::None, Info.Connection);
            PeerConnection& ref = *record;
            Connections.emplace_back(std::move(record));
            return ref;
        }

        void Deny(PeerConnection& record, DenyReason reason)
        {
            const vector<u8> bytes = EncodeConnectDeny(ConnectDenyMessage{.Reason = reason});
            (void)record.Conn->Send(Channel::ReliableOrdered, bytes);
            record.Remove = true;
        }

        void HandleConnectRequest(PeerConnection& record, std::span<const u8> message)
        {
            const optional<ConnectRequestMessage> request = DecodeConnectRequest(message);
            if (!request.has_value())
            {
                return; // malformed — drop, recoverable
            }
            if (record.Established)
            {
                return; // duplicate request after acceptance — ignore
            }

            if (request->ProtocolVersion != Info.ProtocolVersion)
            {
                Log::Warn(
                    "Net::Server denying connection: protocol mismatch (client {}, server {})",
                    request->ProtocolVersion, Info.ProtocolVersion);
                Deny(record, DenyReason::ProtocolMismatch);
                return;
            }
            if (!(request->Content == Info.Content))
            {
                Log::Warn(
                    "Net::Server denying connection: content mismatch (client {:016X}{:016X}, "
                    "server {:016X}{:016X})",
                    request->Content.Hi, request->Content.Lo, Info.Content.Hi, Info.Content.Lo);
                Deny(record, DenyReason::ContentMismatch);
                return;
            }
            if (EstablishedCount() >= Info.MaxConnections)
            {
                Log::Warn("Net::Server denying connection: server full ({} of {})",
                          EstablishedCount(), Info.MaxConnections);
                Deny(record, DenyReason::ServerFull);
                return;
            }
            if (Info.OnConnectRequest &&
                !Info.OnConnectRequest(ConnectRequestInfo{.AppVersion = request->AppVersion,
                                                          .Content = request->Content}))
            {
                Log::Warn("Net::Server denying connection: refused by app policy");
                Deny(record, DenyReason::AppRefused);
                return;
            }

            // Account admission: the hook admits (or normalizes) the presented id; unset accepts it
            // as presented. The id the connection will hold is assigned first so the hook sees it; a
            // refusal burns the id, which is harmless (ids are monotonic and never reused).
            record.Id = NextId;
            NextId += 1;
            const optional<AccountId> admitted =
                Info.AdmitAccount ? Info.AdmitAccount(record.Id, request->Account)
                                  : optional<AccountId>(request->Account);
            if (!admitted.has_value() || !admitted->IsValid())
            {
                Log::Warn("Net::Server denying connection: account refused");
                Deny(record, DenyReason::AccountRefused);
                return;
            }

            // Exactly one live connection may hold an account: a duplicate is refused, the existing
            // binding undisturbed, until the stale connection leaves or times out (the deny reason is
            // documented retryable across that zombie window).
            for (const Unique<PeerConnection>& other : Connections)
            {
                if (other->Established && !other->Remove && other->Account == *admitted)
                {
                    Log::Warn("Net::Server denying connection: account already connected (as {})",
                              other->Id);
                    Deny(record, DenyReason::AccountAlreadyConnected);
                    return;
                }
            }

            record.Account = *admitted;
            record.Established = true;
            // The connection accept establishes the link only; which world the client loads and which
            // seat is its own ride the per-world join reply, not this acceptance.
            const vector<u8> accept = EncodeConnectAccept(ConnectAcceptMessage{.Id = record.Id});
            (void)record.Conn->Send(Channel::ReliableOrdered, accept);
            Events.push_back(NetEvent{
                .Type = NetEventType::Connected, .Id = record.Id, .Account = record.Account});
            Log::Info("Net::Server accepted connection {}", record.Id);
        }

        void ProcessControl(PeerConnection& record)
        {
            while (const optional<vector<u8>> message =
                       record.Conn->Receive(Channel::ReliableOrdered))
            {
                const optional<ControlMessageType> type = PeekControlType(*message);
                if (!type.has_value())
                {
                    // A non-handshake reliable message (the join layer's ClientReady): surface it to
                    // the world glue rather than dropping it.
                    if (record.Established)
                    {
                        record.AppReliable.push_back(*message);
                    }
                    continue;
                }
                switch (*type)
                {
                case ControlMessageType::ConnectRequest:
                    HandleConnectRequest(record, *message);
                    break;
                case ControlMessageType::Disconnect:
                    if (DecodeDisconnect(*message).has_value())
                    {
                        if (record.Established)
                        {
                            Events.push_back(NetEvent{.Type = NetEventType::Disconnected,
                                                      .Id = record.Id,
                                                      .Reason = DisconnectReason::Left});
                        }
                        record.Remove = true;
                    }
                    break;
                case ControlMessageType::ConnectAccept:
                case ControlMessageType::ConnectDeny:
                    break; // server→client only; ignore inbound
                }
            }
        }
    };

    Server::Server(Unique<State> state) : m_State(std::move(state)) {}

    Server::~Server() = default;

    Result<Unique<Server>> Server::Create(const ServerInfo& info)
    {
        auto state = CreateUnique<State>();
        state->Info = info;

        if (info.TransportOverride != nullptr)
        {
            state->Transport = info.TransportOverride;
        }
        else
        {
            Result<Unique<UdpTransport>> udp = UdpTransport::Bind(info.Port);
            if (!udp.has_value())
            {
                return std::unexpected(udp.error());
            }
            state->OwnedUdp = udp->get();
            if (info.NetSim.has_value())
            {
                // Wrap the bound socket in the network simulation; the socket outlives the wrapper.
                state->OwnedInner = std::move(*udp);
                auto sim = CreateUnique<FaultInjectionTransport>(*state->OwnedInner, *info.NetSim);
                state->Sim = sim.get();
                state->OwnedTransport = std::move(sim);
            }
            else
            {
                state->OwnedTransport = std::move(*udp);
            }
            state->Transport = state->OwnedTransport.get();
        }

        return Unique<Server>(new Server(std::move(state)));
    }

    void Server::Pump(f64 now)
    {
        State& s = *m_State;
        if (s.Sim != nullptr)
        {
            s.Sim->SetTime(now);
        }
        s.Events.clear();
        for (Unique<PeerConnection>& c : s.Connections)
        {
            c->AppReliable.clear();
        }

        // Drain the listen transport and demultiplex each datagram to its peer's link, opening a
        // provisional connection for a first-contact peer (bounded by the provisional pool).
        while (const optional<Datagram> datagram = s.Transport->Receive())
        {
            PeerConnection* record = s.FindByPeer(datagram->From);
            if (record == nullptr)
            {
                if (s.ProvisionalCount() >= s.Info.MaxConnections)
                {
                    continue; // provisional pool full — drop the half-open flood
                }
                record = &s.CreateProvisional(datagram->From);
            }
            record->Link->Enqueue(datagram->Bytes);
        }

        // Advance each connection: pump the demuxed inbox, drive resends/keepalive/timeout.
        for (Unique<PeerConnection>& c : s.Connections)
        {
            c->Conn->Update(now);
        }

        // Complete handshakes and process graceful disconnects.
        for (Unique<PeerConnection>& c : s.Connections)
        {
            if (!c->Remove && !c->Closing)
            {
                s.ProcessControl(*c);
            }
        }

        // Flush any accept/deny/close message queued above so it crosses the wire this pump.
        for (Unique<PeerConnection>& c : s.Connections)
        {
            c->Conn->Update(now);
        }

        // Reap timed-out and closing connections, surfacing an event for an established one.
        for (Unique<PeerConnection>& c : s.Connections)
        {
            if (c->Conn->TimedOut() && !c->Remove)
            {
                if (c->Established)
                {
                    s.Events.push_back(NetEvent{.Type = NetEventType::Disconnected,
                                                .Id = c->Id,
                                                .Reason = DisconnectReason::Timeout});
                }
                c->Remove = true;
            }
            if (c->Closing && !c->Remove)
            {
                s.Events.push_back(NetEvent{
                    .Type = NetEventType::Disconnected, .Id = c->Id, .Reason = c->CloseReason});
                c->Remove = true;
            }
        }
        std::erase_if(s.Connections, [](const Unique<PeerConnection>& c) { return c->Remove; });

        // Refresh the established-id view.
        s.EstablishedIds.clear();
        for (const Unique<PeerConnection>& c : s.Connections)
        {
            if (c->Established)
            {
                s.EstablishedIds.push_back(c->Id);
            }
        }
    }

    std::span<const ConnectionId> Server::Connections() const
    {
        return m_State->EstablishedIds;
    }

    Connection& Server::Get(ConnectionId id)
    {
        for (Unique<PeerConnection>& c : m_State->Connections)
        {
            if (c->Established && c->Id == id)
            {
                return *c->Conn;
            }
        }
        VE_ASSERT(false, "Net::Server::Get called with unknown connection id {}", id);
    }

    VoidResult Server::Disconnect(ConnectionId id, DisconnectReason reason)
    {
        for (Unique<PeerConnection>& c : m_State->Connections)
        {
            if (c->Established && c->Id == id && !c->Closing && !c->Remove)
            {
                const vector<u8> bytes = EncodeDisconnect(DisconnectMessage{.Reason = reason});
                (void)c->Conn->Send(Channel::ReliableOrdered, bytes);
                c->Closing = true;
                c->CloseReason = reason;
                return {};
            }
        }
        return std::unexpected(fmt::format("no established connection with id {}", id));
    }

    std::span<const NetEvent> Server::Events() const
    {
        return m_State->Events;
    }

    std::span<const vector<u8>> Server::ReliableAppMessages(ConnectionId id) const
    {
        for (const Unique<PeerConnection>& c : m_State->Connections)
        {
            if (c->Established && c->Id == id)
            {
                return c->AppReliable;
            }
        }
        return {};
    }

    Result<u16> Server::LocalPort() const
    {
        if (m_State->OwnedUdp != nullptr)
        {
            return m_State->OwnedUdp->LocalPort();
        }
        return std::unexpected(
            string("server has no bound UdpTransport (a transport override was supplied)"));
    }
}
