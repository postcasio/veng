#pragma once

#include <Veng/Net/Transport.h>
#include <Veng/Result.h>
#include <Veng/Veng.h>

#include <span>

// Veng/Net/LoopbackTransport.h — an in-process transport pair.
//
// Two LoopbackTransports share one in-memory medium: a datagram Sent on one
// surfaces from Receive on the other, with no socket, no thread, and no wall
// clock. It is the device-free test fixture for the entire net layer, and — with
// no code change — the listen-server path (a server and a client in one process
// talk over exactly the same Connection code they would over UDP).

namespace Veng::Net
{
    /// @brief In-process transport: one of a connected pair sharing a memory medium.
    ///
    /// Created only through CreatePair, which returns two transports bound to one
    /// shared medium. Each is the other's sole peer: Resolve on either returns the
    /// peer handle regardless of host/port, and a datagram Sent to that handle
    /// surfaces from the peer's Receive tagged with this transport's own handle as
    /// From.
    class VE_API LoopbackTransport final : public Transport
    {
    public:
        /// @brief Creates a connected pair sharing one in-process medium.
        /// @return The two endpoints; either may act as server or client.
        static std::pair<Unique<LoopbackTransport>, Unique<LoopbackTransport>> CreatePair();

        ~LoopbackTransport() override;

        LoopbackTransport(const LoopbackTransport&) = delete;
        LoopbackTransport& operator=(const LoopbackTransport&) = delete;

        VoidResult Send(EndpointId to, std::span<const u8> bytes) override;
        optional<Datagram> Receive() override;
        Result<EndpointId> Resolve(string_view host, u16 port) override;

        /// @brief This transport's own handle (the From other endpoints observe).
        [[nodiscard]] EndpointId Local() const { return m_Self; }
        /// @brief The peer transport's handle (what Resolve returns).
        [[nodiscard]] EndpointId Peer() const { return m_Peer; }

    private:
        struct Medium;

        LoopbackTransport(Ref<Medium> medium, EndpointId self, EndpointId peer);

        Ref<Medium> m_Medium;
        EndpointId m_Self;
        EndpointId m_Peer;
        vector<u8> m_ReceiveScratch;
    };
}
