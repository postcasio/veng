#pragma once

#include <Veng/Net/Transport.h>
#include <Veng/Result.h>
#include <Veng/Veng.h>

#include <span>

// Veng/Net/UdpTransport.h — the platform-socket transport.
//
// The only Transport that touches the OS. Every socket type it uses — sockaddr,
// the file descriptor, the BSD-sockets / Winsock split — lives behind a pImpl in
// UdpTransport.cpp, so this header stays socket-free and passes include_hygiene.
// EndpointIds index an internal address table; peer addresses never surface here.

namespace Veng::Net
{
    /// @brief Non-blocking UDP transport over platform sockets.
    ///
    /// A server Binds a known port and discovers peers from received datagrams; a
    /// client Opens an ephemeral local port and Resolves a host/port to a peer
    /// handle. Sends and receives never block. All socket state is hidden behind a
    /// pImpl, and all fallible operations return a Result.
    class VE_API UdpTransport final : public Transport
    {
    public:
        /// @brief Opens a socket bound to a fixed local port (the server role).
        /// @param port  Local UDP port to bind; 0 requests an ephemeral port.
        /// @return The transport, or an error string on socket/bind failure.
        static Result<Unique<UdpTransport>> Bind(u16 port);

        /// @brief Opens a socket on an ephemeral local port (the client role).
        /// @return The transport, or an error string on socket failure.
        static Result<Unique<UdpTransport>> Open();

        ~UdpTransport() override;

        UdpTransport(const UdpTransport&) = delete;
        UdpTransport& operator=(const UdpTransport&) = delete;

        VoidResult Send(EndpointId to, std::span<const u8> bytes) override;
        optional<Datagram> Receive() override;
        Result<EndpointId> Resolve(string_view host, u16 port) override;

        /// @brief Returns the bound local port (useful after Bind(0) or Open()).
        /// @return The local port, or an error string if it cannot be queried.
        [[nodiscard]] Result<u16> LocalPort() const;

    private:
        struct Impl;

        explicit UdpTransport(Unique<Impl> impl);

        Unique<Impl> m_Impl;
    };
}
