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
// a 32-bit ack bitfield — the standard sliding window). A reliable message larger
// than one packet travels as ordered fragments and is delivered reassembled. Time is injected through
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

    /// @brief Largest datagram a Connection emits, in bytes, header included.
    ///
    /// Below the common 1280-byte IPv6 minimum MTU with room for IP and UDP headers, so a datagram
    /// is never fragmented at the IP layer.
    inline constexpr usize MaxDatagramSize = 1200;

    /// @brief Largest unreliable message payload that fits one datagram.
    inline constexpr usize MaxUnreliableMessageSize = MaxDatagramSize - PacketHeaderSize;

    /// @brief Largest reliable message that travels as one unfragmented packet.
    ///
    /// An unfragmented reliable payload carries a 2-byte message id ahead of the message, so the
    /// budget is two bytes below the unreliable one. This framing is the same in every protocol
    /// version, which is why a message that must be readable by a peer of any version (the connect
    /// request, which carries the protocol version) is bounded by this rather than by
    /// MaxReliableMessageSize.
    inline constexpr usize MaxUnfragmentedReliableMessageSize =
        MaxDatagramSize - PacketHeaderSize - 2;

    /// @brief Largest slice of a fragmented reliable message one packet carries.
    ///
    /// A fragment carries its 2-byte message id plus a 1-byte fragment index and a 1-byte fragment
    /// count ahead of the slice.
    inline constexpr usize MaxReliableFragmentSize = MaxDatagramSize - PacketHeaderSize - 4;

    /// @brief Largest reliable message Send accepts, in bytes; a larger one is refused.
    ///
    /// A message above MaxUnfragmentedReliableMessageSize is split into ordered fragments of up to
    /// MaxReliableFragmentSize, each resent until acked, and reassembled whole before delivery. The
    /// bound is chosen so a message's fragments (MaxReliableFragmentCount) fit one ack window — the
    /// acked sequence plus its 32-bit ack bitfield — so a message sent in one burst is acknowledged
    /// by a single returning header rather than having its early fragments resent. It also bounds
    /// the reassembly buffer a peer can make the receiver hold.
    inline constexpr usize MaxReliableMessageSize = 32 * 1024;

    /// @brief Most fragments one reliable message is split into.
    inline constexpr usize MaxReliableFragmentCount =
        (MaxReliableMessageSize + MaxReliableFragmentSize - 1) / MaxReliableFragmentSize;

    static_assert(MaxReliableFragmentCount <= 33,
                  "a reliable message's fragments must fit one ack window (ack + 32 bits)");

    /// @brief The two delivery disciplines a Connection offers.
    enum class Channel : u8
    {
        /// @brief Latest-wins, never retransmitted: a datagram older than the last
        /// delivered one is dropped. For state that a newer packet supersedes.
        UnreliableSequenced = 0,
        /// @brief Resent until acked, delivered in order exactly once. For events
        /// that must arrive. A message larger than one packet is fragmented and reassembled
        /// transparently, up to MaxReliableMessageSize.
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
        ///         channel's bound (MaxUnreliableMessageSize, or MaxReliableMessageSize for a
        ///         reliable message, which is fragmented when it exceeds one packet).
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
