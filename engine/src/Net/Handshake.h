#pragma once

#include <Veng/Net/NetEvents.h>
#include <Veng/Net/TravelPayload.h>
#include <Veng/Net/WorldKey.h>
#include <Veng/Veng.h>

#include <span>

// Net/Handshake.h — the connection- and join-lifecycle control-message codec.
//
// Internal (engine/src) header. The handshake is two-tier: a connection tier that establishes the
// process↔process link (connect request/accept/deny, disconnect) and a per-world join tier that
// joins one world by an opaque WorldKey (join request/accept/deny). The connection-tier messages
// ride the reliable channel raw, framed with a leading ControlMessageType byte; the join-tier
// messages ride the world-multiplexing envelope (WorldEnvelope.h) with the reserved ControlJoinId,
// framed with a leading JoinMessageType byte. Every field is packed little-endian one at a time (the
// Protocol.h discipline), never a padded-struct copy, and every decode is bounds-checked and
// recoverable (a truncated or unknown message yields nullopt, never a read past the end).

namespace Veng::Net
{
    /// @brief The type tag prefixing every connection-tier control message on the reliable channel.
    ///
    /// Zero is left unused so a zeroed byte is never a valid control type — the same zero the
    /// world-multiplexing envelope's marker uses, so an enveloped world/join frame is never mistaken
    /// for a connection-tier control message on the shared reliable channel.
    enum class ControlMessageType : u8
    {
        ConnectRequest = 1,
        ConnectAccept = 2,
        ConnectDeny = 3,
        Disconnect = 4,
    };

    /// @brief The type tag prefixing every join-tier message inside the world-multiplexing envelope.
    ///
    /// A join-tier message rides the envelope tagged ControlJoinId (the join-control tier), then this
    /// byte selects which. Distinct from ControlMessageType — a different frame at a different tier.
    enum class JoinMessageType : u8
    {
        JoinRequest = 1,
        JoinAccept = 2,
        JoinDeny = 3,
        TravelRequest = 4,
        DirectedTravel = 5,
        LeaveNotice = 6,
    };

    /// @brief A client's connect request: the parity payload the server checks at the door.
    struct ConnectRequestMessage
    {
        /// @brief The protocol version the client advertises.
        u32 ProtocolVersion = 0;
        /// @brief The content digest the client advertises.
        ContentDigest Content;
        /// @brief The consumer-supplied application version.
        u32 AppVersion = 0;
    };

    /// @brief The server's acceptance of the connection: the assigned id, and nothing world-specific.
    ///
    /// The connection tier establishes the link only; which world the client loads and which seat is
    /// its own now ride the per-world join reply (JoinAcceptMessage), not the connection accept.
    struct ConnectAcceptMessage
    {
        /// @brief The server-assigned connection id.
        ConnectionId Id = ServerConnectionId;
    };

    /// @brief The server's refusal, carrying the reason.
    struct ConnectDenyMessage
    {
        /// @brief Why the connection was refused.
        DenyReason Reason = DenyReason::AppRefused;
    };

    /// @brief A graceful close from either end, carrying the reason.
    struct DisconnectMessage
    {
        /// @brief Why the connection is being closed.
        DisconnectReason Reason = DisconnectReason::Left;
    };

    /// @brief A client's request to join a world, naming it by an opaque WorldKey.
    ///
    /// Rides the world-multiplexing envelope tagged ControlJoinId. The token correlates the reply to
    /// this request (a connection may have several joins in flight for distinct keys), and the server
    /// resolves the key through its authorization hook, caps, and get-or-create factory.
    struct JoinRequestMessage
    {
        /// @brief The opaque world the client asks to join (the engine never interprets it).
        WorldKey Key;
        /// @brief A client-assigned token echoed in the reply so the client can match it to this request.
        u32 RequestToken = 0;
        /// @brief The opaque travel payload threaded into the server's resolution; empty is common.
        TravelPayload Payload;
    };

    /// @brief The server's acceptance of a join: the assigned JoinId plus the world-construction payload.
    ///
    /// Carries what the client needs to build and validate its local view of the resolved world: the
    /// level to load, a content digest of the resolved world the client checks its own reconstruction
    /// against (rejecting the join loudly on mismatch), and the wire id of the client's own seat. The
    /// JoinId tags every subsequent message for this world.
    struct JoinAcceptMessage
    {
        /// @brief The request token this reply answers (echoed from the JoinRequest).
        u32 RequestToken = 0;
        /// @brief The per-connection JoinId assigned to this joined world.
        JoinId Join = ControlJoinId;
        /// @brief The AssetId value of the level the client loads, or 0 when the world is not level-based.
        u64 LevelId = 0;
        /// @brief A content digest of the resolved world the client validates its reconstruction against.
        ContentDigest WorldDigest;
        /// @brief The wire id of the client's own replicated seat entity in this world, or 0 when none.
        u32 SeatNetId = 0;
        /// @brief The travel payload echoed back so the client's factory-parameterized reconstruction has its inputs.
        TravelPayload Payload;
    };

    /// @brief The server's refusal of a join, carrying the request token and the reason.
    struct JoinDenyMessage
    {
        /// @brief The request token this reply answers (echoed from the JoinRequest).
        u32 RequestToken = 0;
        /// @brief Why the join was refused (the connection stays live; only this world is refused).
        JoinDenyReason Reason = JoinDenyReason::NotAuthorized;
    };

    /// @brief A client's request to travel to a world, letting the server direct the resulting join.
    ///
    /// Rides the world-multiplexing envelope tagged ControlJoinId. Unlike a bare join, a travel goes
    /// through the server so it may resolve a parameterized key and reply DirectedTravel — the client
    /// never self-resolves a payload-parameterized key. The payload rides into the server's resolution.
    struct TravelRequestMessage
    {
        /// @brief The opaque world the client asks to travel to.
        WorldKey Key;
        /// @brief The opaque travel payload the server resolves the key with; empty is common.
        TravelPayload Payload;
    };

    /// @brief The server's directive to a client: join this world and, once ready, leave that one.
    ///
    /// The reply to a travel request (and available to the server unprompted). The client joins Join by
    /// the ordinary join flow (digest validation included, the payload supplying reconstruction params)
    /// and, once that join is ready, leaves Leave — make-before-break, so a denied join leaves the
    /// client where it was. A Leave of ControlJoinId means nothing to leave (a fresh travel).
    struct DirectedTravelMessage
    {
        /// @brief The client join to leave once the new join is ready, or ControlJoinId for none.
        JoinId Leave = ControlJoinId;
        /// @brief The opaque world the client must join.
        WorldKey Join;
        /// @brief The opaque travel payload the client carries into the join.
        TravelPayload Payload;
    };

    /// @brief A client's notice that it is leaving a joined world, so the server tears down its seat.
    ///
    /// Rides the world-multiplexing envelope tagged ControlJoinId. Names the JoinId the client is
    /// leaving (per-connection scope, so it can never name another connection's world); the server
    /// releases that join's seat, drops the directory presence, and surfaces the leave. Idempotent — a
    /// notice for a join the server already reaped is dropped.
    struct LeaveNoticeMessage
    {
        /// @brief The client's JoinId being left.
        JoinId Join = ControlJoinId;
    };

    /// @brief Reads the leading control-type byte of a reliable message.
    /// @param message  The message bytes (with the leading type byte).
    /// @return The type, or nullopt if the message is empty or the byte is not a known type.
    [[nodiscard]] optional<ControlMessageType> PeekControlType(std::span<const u8> message);

    /// @brief Reads the leading join-message-type byte of an enveloped join-tier payload.
    /// @param payload  The join-tier payload bytes (with the leading type byte).
    /// @return The type, or nullopt if the payload is empty or the byte is not a known type.
    [[nodiscard]] optional<JoinMessageType> PeekJoinType(std::span<const u8> payload);

    /// @brief Encodes a connect request (type byte + fields).
    [[nodiscard]] vector<u8> EncodeConnectRequest(const ConnectRequestMessage& message);
    /// @brief Encodes a connect accept (type byte + fields).
    [[nodiscard]] vector<u8> EncodeConnectAccept(const ConnectAcceptMessage& message);
    /// @brief Encodes a connect deny (type byte + fields).
    [[nodiscard]] vector<u8> EncodeConnectDeny(const ConnectDenyMessage& message);
    /// @brief Encodes a disconnect (type byte + fields).
    [[nodiscard]] vector<u8> EncodeDisconnect(const DisconnectMessage& message);
    /// @brief Encodes a join request (type byte + fields) as a join-tier payload.
    [[nodiscard]] vector<u8> EncodeJoinRequest(const JoinRequestMessage& message);
    /// @brief Encodes a join accept (type byte + fields) as a join-tier payload.
    [[nodiscard]] vector<u8> EncodeJoinAccept(const JoinAcceptMessage& message);
    /// @brief Encodes a join deny (type byte + fields) as a join-tier payload.
    [[nodiscard]] vector<u8> EncodeJoinDeny(const JoinDenyMessage& message);
    /// @brief Encodes a travel request (type byte + fields) as a join-tier payload.
    [[nodiscard]] vector<u8> EncodeTravelRequest(const TravelRequestMessage& message);
    /// @brief Encodes a directed travel (type byte + fields) as a join-tier payload.
    [[nodiscard]] vector<u8> EncodeDirectedTravel(const DirectedTravelMessage& message);
    /// @brief Encodes a leave notice (type byte + fields) as a join-tier payload.
    [[nodiscard]] vector<u8> EncodeLeaveNotice(const LeaveNoticeMessage& message);

    /// @brief Decodes a connect request; nullopt if truncated or mistyped.
    [[nodiscard]] optional<ConnectRequestMessage> DecodeConnectRequest(std::span<const u8> message);
    /// @brief Decodes a connect accept; nullopt if truncated or mistyped.
    [[nodiscard]] optional<ConnectAcceptMessage> DecodeConnectAccept(std::span<const u8> message);
    /// @brief Decodes a connect deny; nullopt if truncated, mistyped, or an unknown reason.
    [[nodiscard]] optional<ConnectDenyMessage> DecodeConnectDeny(std::span<const u8> message);
    /// @brief Decodes a disconnect; nullopt if truncated, mistyped, or an unknown reason.
    [[nodiscard]] optional<DisconnectMessage> DecodeDisconnect(std::span<const u8> message);
    /// @brief Decodes a join request; nullopt if truncated or mistyped.
    [[nodiscard]] optional<JoinRequestMessage> DecodeJoinRequest(std::span<const u8> payload);
    /// @brief Decodes a join accept; nullopt if truncated or mistyped.
    [[nodiscard]] optional<JoinAcceptMessage> DecodeJoinAccept(std::span<const u8> payload);
    /// @brief Decodes a join deny; nullopt if truncated, mistyped, or an unknown reason.
    [[nodiscard]] optional<JoinDenyMessage> DecodeJoinDeny(std::span<const u8> payload);
    /// @brief Decodes a travel request; nullopt if truncated or mistyped.
    [[nodiscard]] optional<TravelRequestMessage> DecodeTravelRequest(std::span<const u8> payload);
    /// @brief Decodes a directed travel; nullopt if truncated or mistyped.
    [[nodiscard]] optional<DirectedTravelMessage> DecodeDirectedTravel(std::span<const u8> payload);
    /// @brief Decodes a leave notice; nullopt if truncated or mistyped.
    [[nodiscard]] optional<LeaveNoticeMessage> DecodeLeaveNotice(std::span<const u8> payload);
}
