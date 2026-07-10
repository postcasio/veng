#pragma once

#include <Veng/Result.h>
#include <Veng/Veng.h>

#include <span>

// Veng/Net/Transport.h — the datagram transport seam.
//
// This is the socket-free boundary the whole net layer sits above: an interface
// that moves opaque byte datagrams to and from opaque, transport-scoped peer
// handles (EndpointId). No platform socket type appears here — UdpTransport hides
// its sockaddr table behind a pImpl, and LoopbackTransport carries no platform
// surface at all — so this header (and everything above it) compiles against
// public dependencies only, guarded by the include_hygiene sweep.

namespace Veng::Net
{
    /// @brief Transport-scoped handle for a peer address.
    ///
    /// Opaque outside the transport that minted it: UdpTransport maps it to an
    /// internal sockaddr entry, LoopbackTransport to an in-process queue. A client
    /// obtains one from Transport::Resolve; a server learns one from the From field
    /// of a received Datagram. Never serialized onto the wire.
    enum class EndpointId : u32
    {
        None = 0xFFFFFFFFu
    };

    /// @brief One received datagram: its source peer and a view over its bytes.
    ///
    /// Bytes is a view over transport-owned storage that stays valid only until the
    /// next Transport::Receive call on the same transport; a consumer that needs to
    /// retain the payload copies it before pumping again.
    struct Datagram
    {
        /// @brief The peer the datagram arrived from.
        EndpointId From = EndpointId::None;
        /// @brief View over the datagram payload, valid until the next Receive.
        std::span<const u8> Bytes;
    };

    /// @brief Abstract non-blocking datagram transport.
    ///
    /// The single seam every net implementation shares: unreliable, unordered,
    /// bounded-size datagrams to and from EndpointIds. Reliability, ordering, and
    /// sequencing are layered above by Connection — the transport only moves bytes.
    /// Every fallible operation returns a Result; nothing here throws.
    class Transport
    {
    public:
        virtual ~Transport() = default;

        /// @brief Sends one datagram to a peer.
        /// @param to     Destination peer handle.
        /// @param bytes  Payload to send; copied out before the call returns.
        /// @return Empty on success, or an error string describing the failure.
        virtual VoidResult Send(EndpointId to, std::span<const u8> bytes) = 0;

        /// @brief Dequeues the next received datagram, if any.
        ///
        /// Non-blocking: returns nullopt once the inbound queue is drained. The
        /// returned Datagram's Bytes view is valid only until the next Receive.
        /// @return The next datagram, or nullopt when none is pending.
        virtual optional<Datagram> Receive() = 0;

        /// @brief Resolves a host and port to a peer handle to send to.
        /// @param host  Host string (an address literal for UdpTransport; ignored by
        ///              LoopbackTransport, which has exactly one peer).
        /// @param port  Destination port.
        /// @return The peer handle, or an error string on failure.
        virtual Result<EndpointId> Resolve(string_view host, u16 port) = 0;
    };
}
