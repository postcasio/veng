#pragma once

#include <Veng/Net/Transport.h>
#include <Veng/Result.h>
#include <Veng/Veng.h>

#include <span>

// Veng/Net/Connection.h — the per-peer sequencing / reliability layer.
//
// Above a Transport, a Connection turns opaque datagrams into two message
// channels with defined delivery disciplines. Each datagram carries one packet
// header (protocol magic, channel, a u16 sequence, and a remote-sequence ack plus
// a 32-bit ack bitfield — the standard sliding window). Time is injected through
// Update(now): the layer holds no wall clock, so it is fully deterministic under
// test. Socket-free — the whole file compiles under include_hygiene.

namespace Veng::Net
{
    /// @brief Wire protocol magic prefixing every packet header.
    inline constexpr u32 ProtocolMagic = 0x564E4731u;

    /// @brief Serialized size of a packet header, in bytes.
    ///
    /// magic(4) + channel(1) + sequence(2) + ack(2) + ackBits(4). Fields are packed
    /// little-endian one at a time, never as a padded struct copy.
    inline constexpr usize PacketHeaderSize = 13;

    /// @brief Largest unreliable message payload that fits one datagram.
    inline constexpr usize MaxUnreliableMessageSize = 1200 - PacketHeaderSize;

    /// @brief Largest reliable message payload that fits one datagram.
    ///
    /// A reliable payload also carries a 2-byte message id, so the budget is two
    /// bytes below the unreliable one. Fragmentation of larger messages is a named
    /// follow-on; a message over this size is rejected loudly by Send.
    inline constexpr usize MaxReliableMessageSize = 1200 - PacketHeaderSize - 2;

    /// @brief The two delivery disciplines a Connection offers.
    enum class Channel : u8
    {
        /// @brief Latest-wins, never retransmitted: a datagram older than the last
        /// delivered one is dropped. For state that a newer packet supersedes.
        UnreliableSequenced = 0,
        /// @brief Resent until acked, delivered in order exactly once. For events
        /// that must arrive. Messages are MTU-capped (see MaxReliableMessageSize).
        ReliableOrdered = 1,
    };

    /// @brief Timing knobs for a Connection, all in seconds of injected time.
    struct ConnectionConfig
    {
        /// @brief Base interval before an unacked reliable message is resent.
        f64 ResendInterval = 0.1;
        /// @brief Cap on the exponentially backed-off resend interval.
        f64 ResendBackoffMax = 1.0;
        /// @brief Idle interval after which a keepalive is sent.
        f64 KeepaliveInterval = 1.0;
        /// @brief Silence interval after which the peer is considered timed out.
        f64 TimeoutInterval = 5.0;
    };

    /// @brief Per-peer connection state over a Transport.
    ///
    /// Owns the sequencing, ack, resend, keepalive, and timeout machinery for one
    /// peer. Send queues or emits a message on a channel; Update(now) pumps received
    /// datagrams, drives resends and keepalive off the injected time, and updates
    /// the timeout flag; Receive hands the app messages already ordered per the
    /// channel's discipline. The Transport is borrowed, not owned, and must outlive
    /// the Connection.
    class VE_API Connection
    {
    public:
        /// @brief Constructs a connection to a peer over a transport.
        /// @param transport  Borrowed transport; must outlive this connection.
        /// @param peer        The peer handle; EndpointId::None adopts the first
        ///                    peer a datagram is received from (the server role).
        /// @param config      Timing configuration.
        Connection(Transport& transport, EndpointId peer, const ConnectionConfig& config = {});

        ~Connection();

        Connection(const Connection&) = delete;
        Connection& operator=(const Connection&) = delete;

        /// @brief Queues (reliable) or emits (unreliable) a message on a channel.
        /// @param channel  The delivery discipline to use.
        /// @param message  The message bytes; copied out.
        /// @return Empty on success, or an error string if the message exceeds the
        ///         channel's MTU budget (see MaxUnreliableMessageSize /
        ///         MaxReliableMessageSize).
        VoidResult Send(Channel channel, std::span<const u8> message);

        /// @brief Pumps received datagrams and advances time-driven state.
        /// @param now  Monotonic time in seconds (injected — never a wall clock).
        void Update(f64 now);

        /// @brief Dequeues the next delivered message on a channel.
        /// @param channel  The channel to read.
        /// @return The next message per the channel's discipline, or nullopt.
        optional<vector<u8>> Receive(Channel channel);

        /// @brief True once the peer has been silent past the timeout interval.
        [[nodiscard]] bool TimedOut() const;

        /// @brief Smoothed round-trip-time estimate in seconds.
        [[nodiscard]] f32 RttEstimate() const;

        /// @brief The peer handle (resolved, or adopted from the first datagram).
        [[nodiscard]] EndpointId Peer() const;

    private:
        struct State;

        Unique<State> m_State;
    };
}
