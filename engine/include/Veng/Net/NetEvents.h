#pragma once

#include <Veng/Net/AccountId.h>
#include <Veng/Net/Connection.h>
#include <Veng/Veng.h>

// Veng/Net/NetEvents.h — the shared connection-lifecycle vocabulary.
//
// The types Server and Client agree on: the server-assigned connection id, the
// content-identity handshake payload, the deny/disconnect reason enums, the client
// state, and the typed lifecycle events the server surfaces to the app layer. Pure
// value types — no socket, no transport — so it compiles under include_hygiene and
// both lifecycle headers include it.

namespace Veng::Net
{
    /// @brief The wire protocol version, checked at handshake the way the module ABI is at load.
    ///
    /// One engine constant covering the wire format (header shape, message ids, snapshot
    /// encoding). Any wire-visible change bumps it; there is no in-protocol negotiation — a peer
    /// advertising a different version is rejected loudly. It is the default local version an app
    /// advertises (overridable per Server/Client to force a stricter app-level parity check).
    ///
    /// Version 2 introduced the per-connection world-multiplexing envelope (a JoinId tag ahead of
    /// each world-tagged payload) and the two-tier connection/join handshake, so the connection
    /// accept no longer bakes in a single level or seat. Version 3 threaded the opaque travel payload
    /// into the join request and its reply, added the travel-request / directed-travel / leave-notice
    /// join-tier control messages (the world-directory travel and adopt-in-place primitives), and grew
    /// the reliable spawn record an optional anchor field (the stable-anchor binding). Version 4 added
    /// the presented AccountId to the connect request (the account-identity handshake) and the
    /// account-tier deny reasons. Version 5 grew the connect request an opaque account profile blob
    /// (the admission profile) and the over-budget deny reason.
    inline constexpr u32 ProtocolVersion = 5;

    /// @brief Wire overhead of a connect request, in bytes, ahead of its account profile blob.
    ///
    /// The leading control-type byte, the protocol version, the 128-bit content digest, the app
    /// version, the 128-bit account id, and the profile blob's own header (its reflected type id
    /// plus its byte count).
    inline constexpr usize ConnectRequestOverhead = 1 + 4 + 16 + 4 + 16 + sizeof(u64) + 4;

    /// @brief Largest account profile (Blob::Bytes) the connect handshake carries, in bytes.
    ///
    /// The reliable channel's per-message bound (MaxReliableMessageSize) minus the connect
    /// request's own framing (ConnectRequestOverhead). The connect request is a single reliable
    /// message with no fragmentation, so a profile past this bound cannot be split: the connect is
    /// refused with DenyReason::ProfileTooLarge rather than the payload being truncated.
    inline constexpr usize MaxProfileBytes = MaxReliableMessageSize - ConnectRequestOverhead;

    /// @brief A server-assigned connection identifier: a per-session u32, never reused.
    ///
    /// Assigned monotonically as connections are accepted, never recycled within a session, and
    /// the value written into an entity's Authority::Owner. Zero is reserved for "server/none",
    /// matching that field's default — a real connection is always nonzero.
    using ConnectionId = u32;

    /// @brief The reserved connection id meaning "the server itself, or no connection".
    inline constexpr ConnectionId ServerConnectionId = 0;

    /// @brief A per-connection wire tag naming one joined world, framed ahead of every world message.
    ///
    /// Server-assigned when a connection joins a world, monotonic per connection and never reused
    /// within it (so a stale datagram for a departed join is dropped, not misrouted to a recycled
    /// tag). Its per-connection scope means it can never name another connection's worlds — the id
    /// carried on the hot path, distinct from the process-local WorldInstanceId (never on the wire)
    /// and the opaque WorldKey (only on the join request).
    using JoinId = u16;

    /// @brief The reserved JoinId meaning "connection/join control, not a world-tagged payload".
    ///
    /// The world-multiplexing envelope reserves zero: a payload tagged with it is a join-tier
    /// control message (a join request/reply/deny), not world data. Real joins are assigned from one.
    inline constexpr JoinId ControlJoinId = 0;

    /// @brief Why a server refused a world join at the join tier — a per-request, logged rejection.
    ///
    /// Distinct from DenyReason (which refuses the whole connection at the door): a join deny leaves
    /// the connection live and only refuses the one requested world.
    enum class JoinDenyReason : u8
    {
        /// @brief The authorization hook refused this connection's join/create of the key.
        NotAuthorized = 0,
        /// @brief The connection is already at its MaxJoinedWorldsPerConnection.
        PerConnectionCapReached = 1,
        /// @brief Opening a new world for the key would exceed the server-wide MaxHostedWorlds.
        HostedWorldsCapReached = 2,
        /// @brief The key missed the shared map and no factory materialized a world for it.
        NoSuchWorld = 3,
    };

    /// @brief The content identity a client and server must share to interoperate.
    ///
    /// A 128-bit digest of the active cooked-pack set (its table-of-contents hash) — "the same
    /// game data" on both ends. The wire carries only asset ids, never assets, so a content
    /// mismatch would silently desync the spawn stream; the handshake rejects it instead. Zero is
    /// the default "no content" digest a bare test or content-free peer advertises.
    struct ContentDigest
    {
        /// @brief Low 64 bits of the digest.
        u64 Lo = 0;
        /// @brief High 64 bits of the digest.
        u64 Hi = 0;

        /// @brief Equality over both halves.
        [[nodiscard]] bool operator==(const ContentDigest&) const = default;
    };

    /// @brief Why a server refused a connection at the handshake — a terminal, logged rejection.
    enum class DenyReason : u8
    {
        /// @brief The client's ProtocolVersion did not match the server's.
        ProtocolMismatch = 0,
        /// @brief The client's ContentDigest did not match the server's active pack set.
        ContentMismatch = 1,
        /// @brief The server is already at its MaxConnections.
        ServerFull = 2,
        /// @brief The app's connect policy hook rejected the request.
        AppRefused = 3,
        /// @brief The presented account was refused (the AdmitAccount hook, or an invalid id).
        AccountRefused = 4,
        /// @brief The presented account is already bound to a live connection.
        ///
        /// Exactly one live connection may hold an account. The refusal is **transient**: after a
        /// client crash or silent drop the stale binding stays live until the transport's
        /// dead-connection detection fires (Connection::TimeoutInterval), so a fast reconnect is
        /// refused with this reason precisely when reattach matters most. A consumer's reconnect
        /// flow treats it as retryable — retry with backoff until the timeout clears the binding.
        AccountAlreadyConnected = 5,
        /// @brief The presented account profile exceeded Net::MaxProfileBytes.
        ///
        /// The connect request is one unfragmented reliable message, so an over-budget profile
        /// cannot be split. It is refused rather than truncated: a silently shortened opaque
        /// payload is a corruption the consumer that authored it cannot detect. A client whose own
        /// presented profile is over budget refuses locally with this reason and sends nothing.
        ProfileTooLarge = 6,
    };

    /// @brief Why an established connection ended.
    enum class DisconnectReason : u8
    {
        /// @brief The peer went silent past the timeout interval.
        Timeout = 0,
        /// @brief The server closed the connection (Server::Disconnect).
        Kicked = 1,
        /// @brief The peer closed the connection gracefully (a Disconnect message).
        Left = 2,
    };

    /// @brief A client's view of its own handshake and connection state.
    enum class ClientState : u8
    {
        /// @brief The connect request is outstanding; no reply yet.
        Connecting = 0,
        /// @brief The server accepted; the connection is live.
        Connected = 1,
        /// @brief The server refused the handshake (see GetDenyReason).
        Denied = 2,
        /// @brief An established (or connecting) link was lost — timeout or a server disconnect.
        Lost = 3,
    };

    /// @brief The kind of a lifecycle event a Server surfaces.
    enum class NetEventType : u8
    {
        /// @brief A connection completed its handshake and was accepted.
        Connected = 0,
        /// @brief A connection ended (see NetEvent::Reason).
        Disconnected = 1,
        /// @brief A connection left one joined world (its seat torn down), the connection staying live.
        ///
        /// Distinct from Disconnected: the connection remains, only the named join (NetEvent::Join) is
        /// gone. Surfaced when a client sends a leave notice; game policy reaps the join's server-side
        /// pawn the way a disconnect reaps every join's.
        WorldLeft = 2,
    };

    /// @brief One typed connection-lifecycle event, drained from a Server per pump.
    ///
    /// Connected/Disconnected surface to the app layer the way input events do — policy (seat
    /// spawn/teardown) lives above. Reason is meaningful only when Type is Disconnected.
    struct NetEvent
    {
        /// @brief Which lifecycle transition this event reports.
        NetEventType Type = NetEventType::Connected;
        /// @brief The connection the event concerns.
        ConnectionId Id = ServerConnectionId;
        /// @brief Why the connection ended; ignored for a Connected event.
        DisconnectReason Reason = DisconnectReason::Timeout;
        /// @brief The joined world this event concerns; meaningful only for a WorldLeft event.
        JoinId Join = ControlJoinId;
        /// @brief The admitted account bound to the connection; meaningful for a Connected event.
        AccountId Account;
    };
}
