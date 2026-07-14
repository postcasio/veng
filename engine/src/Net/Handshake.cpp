#include "Handshake.h"

#include <Veng/Net/Protocol.h>

namespace Veng::Net
{
    namespace
    {
        constexpr usize TypeByteSize = 1;

        void WriteType(vector<u8>& out, ControlMessageType type)
        {
            out.push_back(static_cast<u8>(type));
        }

        // True if the message is at least `size` bytes and its leading type byte matches.
        [[nodiscard]] bool HasType(std::span<const u8> message, ControlMessageType type, usize size)
        {
            return message.size() >= size && message[0] == static_cast<u8>(type);
        }

        [[nodiscard]] bool IsKnownDenyReason(u8 value)
        {
            return value <= static_cast<u8>(DenyReason::AppRefused);
        }

        [[nodiscard]] bool IsKnownDisconnectReason(u8 value)
        {
            return value <= static_cast<u8>(DisconnectReason::Left);
        }

        [[nodiscard]] bool IsKnownJoinDenyReason(u8 value)
        {
            return value <= static_cast<u8>(JoinDenyReason::NoSuchWorld);
        }

        void WriteJoinType(vector<u8>& out, JoinMessageType type)
        {
            out.push_back(static_cast<u8>(type));
        }

        [[nodiscard]] bool HasJoinType(std::span<const u8> payload, JoinMessageType type,
                                       usize size)
        {
            return payload.size() >= size && payload[0] == static_cast<u8>(type);
        }
    }

    optional<ControlMessageType> PeekControlType(std::span<const u8> message)
    {
        if (message.empty())
        {
            return {};
        }
        switch (static_cast<ControlMessageType>(message[0]))
        {
        case ControlMessageType::ConnectRequest:
        case ControlMessageType::ConnectAccept:
        case ControlMessageType::ConnectDeny:
        case ControlMessageType::Disconnect:
            return static_cast<ControlMessageType>(message[0]);
        }
        return {};
    }

    optional<JoinMessageType> PeekJoinType(std::span<const u8> payload)
    {
        if (payload.empty())
        {
            return {};
        }
        switch (static_cast<JoinMessageType>(payload[0]))
        {
        case JoinMessageType::JoinRequest:
        case JoinMessageType::JoinAccept:
        case JoinMessageType::JoinDeny:
            return static_cast<JoinMessageType>(payload[0]);
        }
        return {};
    }

    vector<u8> EncodeConnectRequest(const ConnectRequestMessage& message)
    {
        vector<u8> out;
        WriteType(out, ControlMessageType::ConnectRequest);
        WriteU32LE(out, message.ProtocolVersion);
        WriteU64LE(out, message.Content.Lo);
        WriteU64LE(out, message.Content.Hi);
        WriteU32LE(out, message.AppVersion);
        return out;
    }

    vector<u8> EncodeConnectAccept(const ConnectAcceptMessage& message)
    {
        vector<u8> out;
        WriteType(out, ControlMessageType::ConnectAccept);
        WriteU32LE(out, message.Id);
        return out;
    }

    vector<u8> EncodeConnectDeny(const ConnectDenyMessage& message)
    {
        vector<u8> out;
        WriteType(out, ControlMessageType::ConnectDeny);
        out.push_back(static_cast<u8>(message.Reason));
        return out;
    }

    vector<u8> EncodeDisconnect(const DisconnectMessage& message)
    {
        vector<u8> out;
        WriteType(out, ControlMessageType::Disconnect);
        out.push_back(static_cast<u8>(message.Reason));
        return out;
    }

    optional<ConnectRequestMessage> DecodeConnectRequest(std::span<const u8> message)
    {
        constexpr usize size = TypeByteSize + 4 + 8 + 8 + 4;
        if (!HasType(message, ControlMessageType::ConnectRequest, size))
        {
            return {};
        }
        return ConnectRequestMessage{
            .ProtocolVersion = ReadU32LE(message, 1),
            .Content = ContentDigest{.Lo = ReadU64LE(message, 5), .Hi = ReadU64LE(message, 13)},
            .AppVersion = ReadU32LE(message, 21),
        };
    }

    optional<ConnectAcceptMessage> DecodeConnectAccept(std::span<const u8> message)
    {
        constexpr usize size = TypeByteSize + 4;
        if (!HasType(message, ControlMessageType::ConnectAccept, size))
        {
            return {};
        }
        return ConnectAcceptMessage{
            .Id = ReadU32LE(message, 1),
        };
    }

    optional<ConnectDenyMessage> DecodeConnectDeny(std::span<const u8> message)
    {
        constexpr usize size = TypeByteSize + 1;
        if (!HasType(message, ControlMessageType::ConnectDeny, size) ||
            !IsKnownDenyReason(message[1]))
        {
            return {};
        }
        return ConnectDenyMessage{.Reason = static_cast<DenyReason>(message[1])};
    }

    optional<DisconnectMessage> DecodeDisconnect(std::span<const u8> message)
    {
        constexpr usize size = TypeByteSize + 1;
        if (!HasType(message, ControlMessageType::Disconnect, size) ||
            !IsKnownDisconnectReason(message[1]))
        {
            return {};
        }
        return DisconnectMessage{.Reason = static_cast<DisconnectReason>(message[1])};
    }

    vector<u8> EncodeJoinRequest(const JoinRequestMessage& message)
    {
        vector<u8> out;
        WriteJoinType(out, JoinMessageType::JoinRequest);
        WriteU64LE(out, message.Key.Lo);
        WriteU64LE(out, message.Key.Hi);
        WriteU32LE(out, message.RequestToken);
        return out;
    }

    vector<u8> EncodeJoinAccept(const JoinAcceptMessage& message)
    {
        vector<u8> out;
        WriteJoinType(out, JoinMessageType::JoinAccept);
        WriteU32LE(out, message.RequestToken);
        WriteU16LE(out, message.Join);
        WriteU64LE(out, message.LevelId);
        WriteU64LE(out, message.WorldDigest.Lo);
        WriteU64LE(out, message.WorldDigest.Hi);
        WriteU32LE(out, message.SeatNetId);
        return out;
    }

    vector<u8> EncodeJoinDeny(const JoinDenyMessage& message)
    {
        vector<u8> out;
        WriteJoinType(out, JoinMessageType::JoinDeny);
        WriteU32LE(out, message.RequestToken);
        out.push_back(static_cast<u8>(message.Reason));
        return out;
    }

    optional<JoinRequestMessage> DecodeJoinRequest(std::span<const u8> payload)
    {
        constexpr usize size = TypeByteSize + 8 + 8 + 4;
        if (!HasJoinType(payload, JoinMessageType::JoinRequest, size))
        {
            return {};
        }
        return JoinRequestMessage{
            .Key = WorldKey{.Lo = ReadU64LE(payload, 1), .Hi = ReadU64LE(payload, 9)},
            .RequestToken = ReadU32LE(payload, 17),
        };
    }

    optional<JoinAcceptMessage> DecodeJoinAccept(std::span<const u8> payload)
    {
        constexpr usize size = TypeByteSize + 4 + 2 + 8 + 8 + 8 + 4;
        if (!HasJoinType(payload, JoinMessageType::JoinAccept, size))
        {
            return {};
        }
        return JoinAcceptMessage{
            .RequestToken = ReadU32LE(payload, 1),
            .Join = ReadU16LE(payload, 5),
            .LevelId = ReadU64LE(payload, 7),
            .WorldDigest =
                ContentDigest{.Lo = ReadU64LE(payload, 15), .Hi = ReadU64LE(payload, 23)},
            .SeatNetId = ReadU32LE(payload, 31),
        };
    }

    optional<JoinDenyMessage> DecodeJoinDeny(std::span<const u8> payload)
    {
        constexpr usize size = TypeByteSize + 4 + 1;
        if (!HasJoinType(payload, JoinMessageType::JoinDeny, size) ||
            !IsKnownJoinDenyReason(payload[5]))
        {
            return {};
        }
        return JoinDenyMessage{
            .RequestToken = ReadU32LE(payload, 1),
            .Reason = static_cast<JoinDenyReason>(payload[5]),
        };
    }
}
