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
            return value <= static_cast<u8>(DenyReason::AccountAlreadyConnected);
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

        // The fixed header of a wire-serialized travel payload: the reflected type id (u64) plus the
        // byte-blob length (u32). The blob follows inline.
        constexpr usize PayloadHeaderSize = 8 + 4;

        void WriteTravelPayload(vector<u8>& out, const TravelPayload& payload)
        {
            WriteU64LE(out, payload.Type);
            WriteU32LE(out, static_cast<u32>(payload.Bytes.size()));
            out.insert(out.end(), payload.Bytes.begin(), payload.Bytes.end());
        }

        // Reads a travel payload at @p offset, bounds-checked. On success fills @p out and advances
        // @p offset past the blob; returns false (leaving both untouched) if the message is truncated.
        [[nodiscard]] bool ReadTravelPayload(std::span<const u8> message, usize& offset,
                                             TravelPayload& out)
        {
            if (message.size() < offset + PayloadHeaderSize)
            {
                return false;
            }
            const TypeId type = ReadU64LE(message, offset);
            const u32 length = ReadU32LE(message, offset + 8);
            const usize start = offset + PayloadHeaderSize;
            if (message.size() < start + length)
            {
                return false;
            }
            out.Type = type;
            out.Bytes.assign(message.begin() + static_cast<std::ptrdiff_t>(start),
                             message.begin() + static_cast<std::ptrdiff_t>(start + length));
            offset = start + length;
            return true;
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
        case JoinMessageType::TravelRequest:
        case JoinMessageType::DirectedTravel:
        case JoinMessageType::LeaveNotice:
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
        WriteU64LE(out, message.Account.Lo);
        WriteU64LE(out, message.Account.Hi);
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
        constexpr usize size = TypeByteSize + 4 + 8 + 8 + 4 + 16;
        if (!HasType(message, ControlMessageType::ConnectRequest, size))
        {
            return {};
        }
        return ConnectRequestMessage{
            .ProtocolVersion = ReadU32LE(message, 1),
            .Content = ContentDigest{.Lo = ReadU64LE(message, 5), .Hi = ReadU64LE(message, 13)},
            .AppVersion = ReadU32LE(message, 21),
            .Account = AccountId{.Lo = ReadU64LE(message, 25), .Hi = ReadU64LE(message, 33)},
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
        WriteTravelPayload(out, message.Payload);
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
        WriteTravelPayload(out, message.Payload);
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
        JoinRequestMessage message{
            .Key = WorldKey{.Lo = ReadU64LE(payload, 1), .Hi = ReadU64LE(payload, 9)},
            .RequestToken = ReadU32LE(payload, 17),
        };
        usize offset = 21;
        if (!ReadTravelPayload(payload, offset, message.Payload))
        {
            return {};
        }
        return message;
    }

    optional<JoinAcceptMessage> DecodeJoinAccept(std::span<const u8> payload)
    {
        constexpr usize size = TypeByteSize + 4 + 2 + 8 + 8 + 8 + 4;
        if (!HasJoinType(payload, JoinMessageType::JoinAccept, size))
        {
            return {};
        }
        JoinAcceptMessage message{
            .RequestToken = ReadU32LE(payload, 1),
            .Join = ReadU16LE(payload, 5),
            .LevelId = ReadU64LE(payload, 7),
            .WorldDigest =
                ContentDigest{.Lo = ReadU64LE(payload, 15), .Hi = ReadU64LE(payload, 23)},
            .SeatNetId = ReadU32LE(payload, 31),
        };
        usize offset = 35;
        if (!ReadTravelPayload(payload, offset, message.Payload))
        {
            return {};
        }
        return message;
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

    vector<u8> EncodeTravelRequest(const TravelRequestMessage& message)
    {
        vector<u8> out;
        WriteJoinType(out, JoinMessageType::TravelRequest);
        WriteU64LE(out, message.Key.Lo);
        WriteU64LE(out, message.Key.Hi);
        WriteTravelPayload(out, message.Payload);
        return out;
    }

    vector<u8> EncodeDirectedTravel(const DirectedTravelMessage& message)
    {
        vector<u8> out;
        WriteJoinType(out, JoinMessageType::DirectedTravel);
        WriteU16LE(out, message.Leave);
        WriteU64LE(out, message.Join.Lo);
        WriteU64LE(out, message.Join.Hi);
        WriteTravelPayload(out, message.Payload);
        return out;
    }

    vector<u8> EncodeLeaveNotice(const LeaveNoticeMessage& message)
    {
        vector<u8> out;
        WriteJoinType(out, JoinMessageType::LeaveNotice);
        WriteU16LE(out, message.Join);
        return out;
    }

    optional<TravelRequestMessage> DecodeTravelRequest(std::span<const u8> payload)
    {
        constexpr usize size = TypeByteSize + 8 + 8;
        if (!HasJoinType(payload, JoinMessageType::TravelRequest, size))
        {
            return {};
        }
        TravelRequestMessage message{
            .Key = WorldKey{.Lo = ReadU64LE(payload, 1), .Hi = ReadU64LE(payload, 9)},
        };
        usize offset = 17;
        if (!ReadTravelPayload(payload, offset, message.Payload))
        {
            return {};
        }
        return message;
    }

    optional<DirectedTravelMessage> DecodeDirectedTravel(std::span<const u8> payload)
    {
        constexpr usize size = TypeByteSize + 2 + 8 + 8;
        if (!HasJoinType(payload, JoinMessageType::DirectedTravel, size))
        {
            return {};
        }
        DirectedTravelMessage message{
            .Leave = ReadU16LE(payload, 1),
            .Join = WorldKey{.Lo = ReadU64LE(payload, 3), .Hi = ReadU64LE(payload, 11)},
        };
        usize offset = 19;
        if (!ReadTravelPayload(payload, offset, message.Payload))
        {
            return {};
        }
        return message;
    }

    optional<LeaveNoticeMessage> DecodeLeaveNotice(std::span<const u8> payload)
    {
        constexpr usize size = TypeByteSize + 2;
        if (!HasJoinType(payload, JoinMessageType::LeaveNotice, size))
        {
            return {};
        }
        return LeaveNoticeMessage{.Join = ReadU16LE(payload, 1)};
    }
}
