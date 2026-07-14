#pragma once

#include <Veng/Net/NetEvents.h>
#include <Veng/Net/Protocol.h>
#include <Veng/Veng.h>

#include <span>
#include <utility>

// Veng/Net/WorldEnvelope.h — the per-connection world-multiplexing frame.
//
// Above the Connection's two reliability channels, every world-tagged and join-control message rides
// an envelope framing a JoinId ahead of the payload, so N worlds share one connection. The framing
// is a thin layer above the datagram seam — Transport / UdpTransport / LoopbackTransport are
// untouched, no socket surface is added — and its decode is bounds-checked and recoverable:
// a datagram shorter than the header returns nullopt and is dropped before any routing, so a short
// or garbage frame can never mis-index the demux.
//
// The leading byte is a fixed zero marker. ControlMessageType reserves zero (a zeroed byte is never
// a valid control type), so an enveloped frame can never be mistaken for a connection-tier control
// message on the shared reliable channel — the connection handshake rides unenveloped, this rides
// behind the marker. JoinId zero (ControlJoinId) inside the envelope is the join-control tier (a
// join request/reply/deny); a nonzero JoinId tags world data for that joined world.

namespace Veng::Net
{
    /// @brief The leading byte marking a world-multiplexing envelope on a reliability channel.
    ///
    /// Zero, which ControlMessageType never uses, so an envelope is distinguishable from a
    /// connection-tier control message sharing the reliable channel.
    inline constexpr u8 WorldEnvelopeMarker = 0x00;

    /// @brief Serialized size of the envelope header: the marker byte plus the u16 JoinId.
    inline constexpr usize WorldEnvelopeHeaderSize = 1 + sizeof(JoinId);

    /// @brief Largest snapshot payload that still fits one datagram once the envelope is prepended.
    ///
    /// The snapshot packer targets this rather than the raw channel budget, so a max-sized snapshot
    /// plus its envelope header stays within the Connection's unreliable MTU.
    inline constexpr usize MaxEnvelopedUnreliablePayload =
        MaxUnreliableMessageSize - WorldEnvelopeHeaderSize;

    /// @brief Frames a payload behind the envelope header (marker + JoinId), appended to a buffer.
    /// @param out      Destination buffer; the framed message is appended.
    /// @param join     The JoinId to tag the payload with (ControlJoinId for the join-control tier).
    /// @param payload  The payload bytes to frame.
    inline void EncodeWorldEnvelope(vector<u8>& out, JoinId join, std::span<const u8> payload)
    {
        out.push_back(WorldEnvelopeMarker);
        WriteU16LE(out, join);
        out.insert(out.end(), payload.begin(), payload.end());
    }

    /// @brief Frames a payload behind the envelope header, returning the framed bytes.
    /// @param join     The JoinId to tag the payload with.
    /// @param payload  The payload bytes to frame.
    /// @return The framed message bytes.
    [[nodiscard]] inline vector<u8> EncodeWorldEnvelope(JoinId join, std::span<const u8> payload)
    {
        vector<u8> out;
        out.reserve(WorldEnvelopeHeaderSize + payload.size());
        EncodeWorldEnvelope(out, join, payload);
        return out;
    }

    /// @brief The JoinId and payload view a decoded envelope carries.
    struct WorldEnvelope
    {
        /// @brief The JoinId the payload was tagged with (ControlJoinId for the join-control tier).
        JoinId Join = ControlJoinId;
        /// @brief The payload following the header, a view into the decoded message.
        std::span<const u8> Payload;
    };

    /// @brief Decodes an envelope, bounds-checked: nullopt for a short frame or a wrong marker.
    ///
    /// A message shorter than the header, or one whose leading byte is not the envelope marker, is
    /// not a world-tagged frame and returns nullopt — dropped before any routing.
    /// @param message  The full message bytes (with the envelope header).
    /// @return The JoinId and the payload view, or nullopt if the frame is too short or unmarked.
    [[nodiscard]] inline optional<WorldEnvelope> DecodeWorldEnvelope(std::span<const u8> message)
    {
        if (message.size() < WorldEnvelopeHeaderSize || message[0] != WorldEnvelopeMarker)
        {
            return {};
        }
        return WorldEnvelope{.Join = ReadU16LE(message, 1),
                             .Payload = message.subspan(WorldEnvelopeHeaderSize)};
    }
}
