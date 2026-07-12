#pragma once

#include <Veng/Net/Connection.h>
#include <Veng/Net/FaultInjectionTransport.h>
#include <Veng/Net/NetEvents.h>
#include <Veng/Net/Transport.h>
#include <Veng/Result.h>
#include <Veng/Veng.h>

#include <span>

// Veng/Net/Server.h — the listen/accept side of the connection lifecycle.
//
// Above Plan 01's per-peer Connection layer, a Server owns one transport (a bound
// UDP socket, or a caller-supplied override), demultiplexes datagrams to a
// Connection per peer, and runs the handshake: a provisional connection is created
// on first contact and either accepted (assigned a ConnectionId, promoted) or
// denied and discarded. Sockets pump non-blocking on the caller's thread at frame
// boundaries through Pump(now) — there is no net thread. Socket-free: every socket
// type stays behind the Transport seam and the .cpp.

namespace Veng::Net
{
    /// @brief The peer identity presented to an app connect-policy hook.
    struct ConnectRequestInfo
    {
        /// @brief The consumer-supplied application version the client advertised.
        u32 AppVersion = 0;
        /// @brief The content digest the client advertised (already parity-checked).
        ContentDigest Content;
    };

    /// @brief The world-glue extras a server folds into an accepted connection's ConnectAccept.
    ///
    /// Returned by ServerInfo::OnAccept the moment a connection is accepted (its id is assigned but
    /// the accept is not yet on the wire), so the join payload the client needs — which level to load
    /// and which replicated seat is its own — rides the acceptance itself. A default (both zero)
    /// leaves the accept a bare id, the standalone/no-world-glue behavior.
    struct AcceptPayload
    {
        /// @brief The AssetId value of the level the accepted client loads, or 0 for none.
        u64 LevelId = 0;
        /// @brief The wire id of the client's own seat entity the server spawned, or 0 for none.
        u32 SeatNetId = 0;
    };

    /// @brief Configuration for a Server.
    struct ServerInfo
    {
        /// @brief UDP port to listen on when no transport override is supplied.
        u16 Port = 27750;
        /// @brief Maximum simultaneously accepted connections; a further request is denied ServerFull.
        u32 MaxConnections = 16;
        /// @brief The protocol version this server requires; a mismatch is denied ProtocolMismatch.
        u32 ProtocolVersion = Net::ProtocolVersion;
        /// @brief The active content digest a client must match; a mismatch is denied ContentMismatch.
        ContentDigest Content;
        /// @brief Optional app connect-policy hook; returning false denies the request AppRefused.
        function<bool(const ConnectRequestInfo&)> OnConnectRequest;
        /// @brief Optional world-glue hook; fills the join payload folded into the ConnectAccept.
        ///
        /// Called once per accepted connection, synchronously, with the freshly assigned id, before
        /// the accept is encoded — the seat-spawning glue populates the client's seat and returns its
        /// wire id here. Unset leaves the accept a bare id (the standalone path).
        function<AcceptPayload(ConnectionId)> OnAccept;
        /// @brief Optional transport to listen on instead of a bound UdpTransport (the loopback/test seam).
        Transport* TransportOverride = nullptr;
        /// @brief Optional network simulation wrapping the bound transport (the launcher's --netsim).
        ///
        /// When set (and no TransportOverride), the bound UdpTransport is wrapped in a
        /// SimulatedTransport with these seeded faults + latency; the server advances its clock each
        /// Pump. Ships in every build as a dev/QA tool; inert when unset.
        optional<FaultInjectionConfig> NetSim;
        /// @brief Timing configuration threaded into each per-peer Connection.
        ConnectionConfig Connection;
    };

    /// @brief Owns the listen transport and every accepted connection's lifecycle.
    ///
    /// Create binds (or adopts) a transport; Pump(now) drives one non-blocking frame of
    /// receive → handshake/lifecycle → timeout reaping and drains typed events; Connections()
    /// and Get() reach the live set; Disconnect() closes one. Every fallible construction path
    /// returns a Result — nothing here throws.
    class VE_API Server
    {
    public:
        /// @brief Creates a server listening on the configured transport.
        /// @param info  Server configuration.
        /// @return The server, or an error string if the transport could not be opened.
        static Result<Unique<Server>> Create(const ServerInfo& info);

        ~Server();

        Server(const Server&) = delete;
        Server& operator=(const Server&) = delete;

        /// @brief Pumps one non-blocking frame: receive, handshake, lifecycle, timeout reaping.
        ///
        /// Drains the transport, advances each connection, completes pending handshakes, reaps
        /// timed-out and closing connections, and refreshes the event queue Events() exposes.
        /// @param now  Monotonic time in seconds (injected — never a wall clock).
        void Pump(f64 now);

        /// @brief The ids of every currently established connection.
        /// @return A view valid until the next Pump or Disconnect.
        [[nodiscard]] std::span<const ConnectionId> Connections() const;

        /// @brief Returns the Connection for an established id.
        /// @param id  An established connection id (from Connections() or a Connected event).
        /// @return The connection; asserting on an unknown id (API misuse).
        [[nodiscard]] Connection& Get(ConnectionId id);

        /// @brief Closes an established connection, surfacing a Disconnected event on the next Pump.
        ///
        /// Queues a best-effort Disconnect message to the peer and marks the connection closing;
        /// the next Pump flushes the message, removes the connection, and emits Disconnected.
        /// @param id      The connection to close.
        /// @param reason  The reason carried in the event (typically Kicked).
        /// @return Empty on success, or an error string if the id is unknown.
        VoidResult Disconnect(ConnectionId id, DisconnectReason reason);

        /// @brief The lifecycle events produced by the most recent Pump.
        /// @return A view valid until the next Pump.
        [[nodiscard]] std::span<const NetEvent> Events() const;

        /// @brief The non-handshake reliable messages an established connection delivered this Pump.
        ///
        /// The handshake owns the reliable channel's control messages (connect/accept/deny/disconnect);
        /// every other reliable message a client sends — the join layer's ClientReady, say — is
        /// surfaced here rather than dropped, so the world glue drains them per connection. Refilled
        /// every Pump; the view is valid until the next one.
        /// @param id  An established connection id.
        /// @return This Pump's reliable app messages for the connection, or an empty view for an
        ///         unknown id.
        [[nodiscard]] std::span<const vector<u8>> ReliableAppMessages(ConnectionId id) const;

        /// @brief The bound local port, useful after Create with Port 0 and no override.
        /// @return The port, or an error string when the transport cannot report one.
        [[nodiscard]] Result<u16> LocalPort() const;

    private:
        struct State;

        explicit Server(Unique<State> state);

        Unique<State> m_State;
    };
}
