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
        WriteU64LE(out, message.LevelId);
        WriteU32LE(out, message.SeatNetId);
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
        constexpr usize size = TypeByteSize + 4 + 8 + 4;
        if (!HasType(message, ControlMessageType::ConnectAccept, size))
        {
            return {};
        }
        return ConnectAcceptMessage{
            .Id = ReadU32LE(message, 1),
            .LevelId = ReadU64LE(message, 5),
            .SeatNetId = ReadU32LE(message, 13),
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
}
