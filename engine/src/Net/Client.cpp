#include <Veng/Net/Client.h>

#include <Veng/Log.h>
#include "Handshake.h"
#include <Veng/Net/UdpTransport.h>

#include <utility>

namespace Veng::Net
{
    struct Client::Impl
    {
        Unique<Transport> OwnedInner; // the opened socket when a NetSim wrapper owns the transport
        Unique<Transport>
            OwnedTransport; // non-null when Connect opened its own socket (or its NetSim wrapper)
        FaultInjectionTransport* Sim = nullptr; // the --netsim wrapper, advanced each Pump
        Transport* Transport = nullptr;         // owned or overridden
        Unique<Connection> Conn;
        ClientState State = ClientState::Connecting;
        ConnectionId AssignedId = ServerConnectionId;
        AccountId Account; // the account presented at the handshake (minted when unconfigured)
        optional<DenyReason> Deny;
        vector<vector<u8>> AppReliable; // non-handshake reliable messages from the most recent Pump
    };

    Client::Client(Unique<Impl> impl) : m_Impl(std::move(impl)) {}

    Client::~Client() = default;

    Result<Unique<Client>> Client::Connect(const ClientInfo& info)
    {
        auto impl = CreateUnique<Impl>();

        if (info.TransportOverride != nullptr)
        {
            impl->Transport = info.TransportOverride;
        }
        else
        {
            Result<Unique<UdpTransport>> udp = UdpTransport::Open();
            if (!udp.has_value())
            {
                return std::unexpected(udp.error());
            }
            if (info.NetSim.has_value())
            {
                // Wrap the opened socket in the network simulation; the socket outlives the wrapper.
                impl->OwnedInner = std::move(*udp);
                auto sim = CreateUnique<FaultInjectionTransport>(*impl->OwnedInner, *info.NetSim);
                impl->Sim = sim.get();
                impl->OwnedTransport = std::move(sim);
            }
            else
            {
                impl->OwnedTransport = std::move(*udp);
            }
            impl->Transport = impl->OwnedTransport.get();
        }

        Result<EndpointId> peer = impl->Transport->Resolve(info.Host, info.Port);
        if (!peer.has_value())
        {
            return std::unexpected(peer.error());
        }

        impl->Conn = CreateUnique<Connection>(*impl->Transport, *peer, info.Connection);

        // An unconfigured account mints the ephemeral per-process default, so the presented id is
        // always valid (see ClientInfo::Account).
        impl->Account = info.Account.IsValid() ? info.Account : GenerateAccountId();

        // The connect request is one unfragmented reliable message, so an over-budget profile has
        // nowhere to go: refuse here rather than send a request the host would deny anyway, and
        // surface the same reason the host would have.
        if (info.Profile.Bytes.size() > MaxProfileBytes)
        {
            Log::Warn("Net::Client refusing to connect: account profile is {} bytes, past the {} "
                      "byte budget",
                      info.Profile.Bytes.size(), MaxProfileBytes);
            impl->Deny = DenyReason::ProfileTooLarge;
            impl->State = ClientState::Denied;
            return Unique<Client>(new Client(std::move(impl)));
        }

        const vector<u8> request = EncodeConnectRequest(ConnectRequestMessage{
            .ProtocolVersion = info.ProtocolVersion,
            .Content = info.Content,
            .AppVersion = info.AppVersion,
            .Account = impl->Account,
            .Profile = info.Profile,
        });
        (void)impl->Conn->Send(Channel::ReliableOrdered, request);

        return Unique<Client>(new Client(std::move(impl)));
    }

    void Client::Pump(f64 now)
    {
        Impl& s = *m_Impl;
        if (s.Sim != nullptr)
        {
            s.Sim->SetTime(now);
        }
        s.AppReliable.clear();
        s.Conn->Update(now);

        while (const optional<vector<u8>> message = s.Conn->Receive(Channel::ReliableOrdered))
        {
            const optional<ControlMessageType> type = PeekControlType(*message);
            if (!type.has_value())
            {
                // A non-handshake reliable message (the replication spawn/despawn stream): surface it
                // to the app rather than dropping it.
                s.AppReliable.push_back(*message);
                continue;
            }
            switch (*type)
            {
            case ControlMessageType::ConnectAccept:
                if (const optional<ConnectAcceptMessage> accept = DecodeConnectAccept(*message);
                    accept.has_value() && s.State == ClientState::Connecting)
                {
                    s.AssignedId = accept->Id;
                    s.State = ClientState::Connected;
                    Log::Info("Net::Client connected as {}", accept->Id);
                }
                break;
            case ControlMessageType::ConnectDeny:
                if (const optional<ConnectDenyMessage> deny = DecodeConnectDeny(*message);
                    deny.has_value() && s.State == ClientState::Connecting)
                {
                    s.Deny = deny->Reason;
                    s.State = ClientState::Denied;
                    Log::Warn("Net::Client handshake denied: reason {}",
                              static_cast<u32>(deny->Reason));
                }
                break;
            case ControlMessageType::Disconnect:
                if (DecodeDisconnect(*message).has_value())
                {
                    s.State = ClientState::Lost;
                    Log::Info("Net::Client disconnected by server");
                }
                break;
            case ControlMessageType::ConnectRequest:
                break; // client→server only; ignore inbound
            }
        }

        if (s.Conn->TimedOut() &&
            (s.State == ClientState::Connecting || s.State == ClientState::Connected))
        {
            s.State = ClientState::Lost;
        }
    }

    ClientState Client::State() const
    {
        return m_Impl->State;
    }

    ConnectionId Client::AssignedId() const
    {
        return m_Impl->AssignedId;
    }

    AccountId Client::GetAccount() const
    {
        return m_Impl->Account;
    }

    optional<DenyReason> Client::GetDenyReason() const
    {
        return m_Impl->Deny;
    }

    Connection& Client::Server()
    {
        return *m_Impl->Conn;
    }

    std::span<const vector<u8>> Client::ReliableAppMessages() const
    {
        return m_Impl->AppReliable;
    }

    void Client::Disconnect()
    {
        Impl& s = *m_Impl;
        const vector<u8> bytes =
            EncodeDisconnect(DisconnectMessage{.Reason = DisconnectReason::Left});
        (void)s.Conn->Send(Channel::ReliableOrdered, bytes);
        s.State = ClientState::Lost;
    }
}
