#pragma once

#include <Veng/Net/NetEvents.h>
#include <Veng/Veng.h>

#include <span>

// Net/Handshake.h — the connection-lifecycle control-message codec.
//
// Internal (engine/src) header. The reliable channel of a Connection is framed with
// a leading ControlMessageType byte; this codec encodes and decodes the four
// lifecycle messages that ride it — connect request/accept/deny and disconnect.
// Every field is packed little-endian one at a time (the Protocol.h discipline),
// never a padded-struct copy, and every decode is bounds-checked and recoverable
// (a truncated or unknown message yields nullopt, never a read past the end).

namespace Veng::Net
{
    /// @brief The type tag prefixing every control message on the reliable channel.
    ///
    /// Zero is left unused so a zeroed byte is never a valid control type. Later plans extend
    /// this set with their own reliable message kinds behind the same leading-byte frame.
    enum class ControlMessageType : u8
    {
        ConnectRequest = 1,
        ConnectAccept = 2,
        ConnectDeny = 3,
        Disconnect = 4,
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

    /// @brief The server's acceptance, carrying the assigned connection id.
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

    /// @brief Reads the leading control-type byte of a reliable message.
    /// @param message  The message bytes (with the leading type byte).
    /// @return The type, or nullopt if the message is empty or the byte is not a known type.
    [[nodiscard]] optional<ControlMessageType> PeekControlType(std::span<const u8> message);

    /// @brief Encodes a connect request (type byte + fields).
    [[nodiscard]] vector<u8> EncodeConnectRequest(const ConnectRequestMessage& message);
    /// @brief Encodes a connect accept (type byte + fields).
    [[nodiscard]] vector<u8> EncodeConnectAccept(const ConnectAcceptMessage& message);
    /// @brief Encodes a connect deny (type byte + fields).
    [[nodiscard]] vector<u8> EncodeConnectDeny(const ConnectDenyMessage& message);
    /// @brief Encodes a disconnect (type byte + fields).
    [[nodiscard]] vector<u8> EncodeDisconnect(const DisconnectMessage& message);

    /// @brief Decodes a connect request; nullopt if truncated or mistyped.
    [[nodiscard]] optional<ConnectRequestMessage> DecodeConnectRequest(std::span<const u8> message);
    /// @brief Decodes a connect accept; nullopt if truncated or mistyped.
    [[nodiscard]] optional<ConnectAcceptMessage> DecodeConnectAccept(std::span<const u8> message);
    /// @brief Decodes a connect deny; nullopt if truncated, mistyped, or an unknown reason.
    [[nodiscard]] optional<ConnectDenyMessage> DecodeConnectDeny(std::span<const u8> message);
    /// @brief Decodes a disconnect; nullopt if truncated, mistyped, or an unknown reason.
    [[nodiscard]] optional<DisconnectMessage> DecodeDisconnect(std::span<const u8> message);
}
