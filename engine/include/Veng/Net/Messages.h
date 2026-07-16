#pragma once

#include <Veng/Net/Blob.h>
#include <Veng/Net/Connection.h>
#include <Veng/Net/WorldEnvelope.h>
#include <Veng/Veng.h>

// Veng/Net/Messages.h — the game message channel's vocabulary.
//
// Messages are the event complement to replicated world-state: named, reliable-ordered,
// connection-scoped opaque blobs the engine moves and never interprets — an invite that must reach
// a connection before it holds any world join, chat history a latest-wins snapshot legitimately
// skips, a request a host-side service answers. Messages carry no state: state over messages would
// rebuild replication by hand (materialized copies, resync handshakes, dedup), so anything a peer
// who wasn't present needs later belongs in a replicated world, not on a channel.
//
// A message rides the connection's reliable-ordered channel inside the world-multiplexing envelope
// tagged ControlJoinId, framed by a MessageEnvelope (the channel id and payload size) ahead of the
// blob — the same thin-envelope discipline as the JoinId world tag. Transport and Connection are
// untouched and the decode is bounds-checked: a truncated frame drops before any routing. Because
// every channel shares the connection's one reliable stream, ordering is per connection across all
// channels, not per channel.
//
// Delivery semantics: reliable-ordered within a live connection, at-most-once across its lifetime —
// a message accepted at send either arrives in order or the connection has died. There is no engine
// retry and no offline queue (a mailbox is state and belongs in a world or a store). Topology is
// client → server and server → connection(s) only; everything routes through the host. The send and
// receive surfaces live on ServerHost / ClientHost (Veng/Net/Host.h).

namespace Veng::Net
{
    /// @brief The stable name of one game message channel: a minted u64 the consumer generates.
    ///
    /// Minted with `vengc generate-id` (the SystemId id family) and carried on every message in
    /// place of a string, so a channel's identity is one comparison. The engine never interprets
    /// it; both peers register handlers against the same minted value.
    using ChannelId = u64;

    /// @brief The empty ChannelId, distinct from every minted id; never a valid channel.
    inline constexpr ChannelId InvalidChannelId = 0;

    /// @brief The wire header framing one game message: which channel, and how many payload bytes.
    ///
    /// Rides the reliable-ordered channel inside the world-multiplexing envelope tagged
    /// ControlJoinId, after a leading kind byte that distinguishes a game message from the
    /// join-control messages sharing that tier. The blob's reflected type id and its Size payload
    /// bytes follow the header; the decode drops a frame whose bytes do not match Size exactly.
    struct MessageEnvelope
    {
        /// @brief The channel the message is addressed to.
        ChannelId Channel = InvalidChannelId;
        /// @brief The payload byte count following the header (the blob's bytes, not its type id).
        u16 Size = 0;
    };

    /// @brief Wire overhead of one framed message, in bytes, on the reliable channel.
    ///
    /// The world-multiplexing envelope header, the message kind byte, the MessageEnvelope fields
    /// (channel id + size), and the blob's reflected type id.
    inline constexpr usize MessageWireOverhead =
        WorldEnvelopeHeaderSize + 1 + sizeof(ChannelId) + sizeof(u16) + sizeof(TypeId);

    /// @brief Largest message payload (Blob::Bytes) a send accepts, in bytes.
    ///
    /// The reliable channel's per-message bound (MaxReliableMessageSize) minus the message framing
    /// (MessageWireOverhead) — 1163 bytes, ~1.1 KiB. Design blob shapes within it: there is no
    /// fragmentation, so a payload past this bound fails at send rather than being split.
    inline constexpr usize MaxMessagePayloadSize = MaxReliableMessageSize - MessageWireOverhead;

    /// @brief Most messages one connection's outbound queue holds between pumps; the next send fails.
    ///
    /// The outbound cap fails further sends loudly rather than buffering unboundedly — with
    /// MaxOutboundMessageBytes, whichever trips first.
    inline constexpr usize MaxOutboundMessages = 256;

    /// @brief Most framed bytes one connection's outbound queue holds between pumps; the next send fails.
    inline constexpr usize MaxOutboundMessageBytes = 64 * 1024;

    /// @brief Most messages one connection may deliver in a single pump before it is dropped.
    ///
    /// The inbound budget: a connection exceeding it (or MaxInboundMessageBytesPerPump) in one pump
    /// is disconnected with a logged reason, so a hostile or buggy peer cannot starve the shared
    /// stream.
    inline constexpr usize MaxInboundMessagesPerPump = 256;

    /// @brief Most framed bytes one connection may deliver in a single pump before it is dropped.
    inline constexpr usize MaxInboundMessageBytesPerPump = 64 * 1024;
}
