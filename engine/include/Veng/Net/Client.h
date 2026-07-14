#pragma once

#include <Veng/Asset/AssetId.h>
#include <Veng/Net/Connection.h>
#include <Veng/Net/FaultInjectionTransport.h>
#include <Veng/Net/NetEvents.h>
#include <Veng/Net/Transport.h>
#include <Veng/Result.h>
#include <Veng/Veng.h>

#include <span>

// Veng/Net/Client.h — the connect/handshake side of the connection lifecycle.
//
// A Client opens a transport to one server, sends a ConnectRequest carrying its
// protocol version and content identity, and drives the reply: accepted (Connected,
// with a server-assigned id), denied (Denied, with a reason), or lost (a timeout).
// Pump(now) runs the same non-blocking, thread-free frame discipline the Server
// uses. Socket-free — sockets stay behind the Transport seam and the .cpp.

namespace Veng::Net
{
    /// @brief Configuration for a Client.
    struct ClientInfo
    {
        /// @brief Server host to resolve (an address literal for UDP; ignored by loopback).
        string Host;
        /// @brief Server port.
        u16 Port = 27750;
        /// @brief The protocol version to advertise; the server denies a mismatch.
        u32 ProtocolVersion = Net::ProtocolVersion;
        /// @brief The active content digest to advertise; the server denies a mismatch.
        ContentDigest Content;
        /// @brief Consumer-supplied application version, surfaced to the server's connect hook.
        u32 AppVersion = 0;
        /// @brief Optional transport to connect over instead of an opened UdpTransport (the test seam).
        Transport* TransportOverride = nullptr;
        /// @brief Optional network simulation wrapping the opened transport (the launcher's --netsim).
        ///
        /// When set (and no TransportOverride), the opened UdpTransport is wrapped in a
        /// SimulatedTransport with these seeded faults + latency; the client advances its clock each
        /// Pump. Inert when unset.
        optional<FaultInjectionConfig> NetSim;
        /// @brief Timing configuration for the underlying Connection.
        ConnectionConfig Connection;
    };

    /// @brief Owns the transport and the single connection to a server.
    ///
    /// Connect opens (or adopts) a transport and queues the handshake; Pump(now) advances it and
    /// resolves the reply into State(). Server() reaches the underlying Connection for the app's
    /// own traffic once Connected. Every fallible construction path returns a Result.
    class VE_API Client
    {
    public:
        /// @brief Opens a connection to a server and begins the handshake.
        /// @param info  Client configuration.
        /// @return The client (State() == Connecting), or an error string if the transport could
        ///         not be opened or the host could not be resolved.
        static Result<Unique<Client>> Connect(const ClientInfo& info);

        ~Client();

        Client(const Client&) = delete;
        Client& operator=(const Client&) = delete;

        /// @brief Pumps one non-blocking frame: advance the connection and resolve the handshake reply.
        /// @param now  Monotonic time in seconds (injected — never a wall clock).
        void Pump(f64 now);

        /// @brief The current connection state.
        [[nodiscard]] ClientState State() const;

        /// @brief The server-assigned id once Connected, else ServerConnectionId.
        [[nodiscard]] ConnectionId AssignedId() const;

        /// @brief The deny reason when State() is Denied, else nullopt.
        [[nodiscard]] optional<DenyReason> GetDenyReason() const;

        /// @brief The underlying connection to the server (valid once Connected).
        [[nodiscard]] Connection& Server();

        /// @brief The app (non-handshake) reliable messages received during the most recent Pump.
        ///
        /// The handshake owns the reliable channel's control messages; every other reliable message —
        /// the replication layer's spawn/despawn stream — is surfaced here rather than dropped, so the
        /// app drains them after each Pump. Refilled every Pump, so the view is valid until the next one.
        /// @return A view of this Pump's app reliable messages, valid until the next Pump.
        [[nodiscard]] std::span<const vector<u8>> ReliableAppMessages() const;

        /// @brief Closes the connection gracefully, sending a best-effort Disconnect to the server.
        ///
        /// Transitions State() to Lost; the queued message flushes on subsequent Pumps so the
        /// server surfaces a Left event rather than waiting for a timeout.
        void Disconnect();

    private:
        struct Impl;

        explicit Client(Unique<Impl> impl);

        Unique<Impl> m_Impl;
    };
}
