#include <Veng/Net/UdpTransport.h>

#include <cstring>
#include <utility>

// The single platform translation point for the net layer: every socket header
// and every sockaddr type is confined to this .cpp. The public UdpTransport.h is
// socket-free (pImpl), so include_hygiene stays green. The Windows port inherits
// exactly this one file — the rest of Veng/Net never sees a socket.
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using SocketHandle = SOCKET;
namespace
{
    constexpr SocketHandle InvalidSocket = INVALID_SOCKET;
}
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
namespace
{
    constexpr SocketHandle InvalidSocket = -1;
}
#endif

namespace Veng::Net
{
    namespace
    {
        constexpr usize ReceiveBufferSize = 2048;

        // Winsock needs one-time process init; refcounted so paired Startup/Cleanup
        // nest safely. A no-op everywhere else.
        VoidResult PlatformInit()
        {
#if defined(_WIN32)
            WSADATA data;
            const int rc = WSAStartup(MAKEWORD(2, 2), &data);
            if (rc != 0)
            {
                return std::unexpected(fmt::format("WSAStartup failed with code {}", rc));
            }
#endif
            return {};
        }

        void PlatformShutdown()
        {
#if defined(_WIN32)
            WSACleanup();
#endif
        }

        void CloseSocket(SocketHandle handle)
        {
            if (handle == InvalidSocket)
            {
                return;
            }
#if defined(_WIN32)
            closesocket(handle);
#else
            close(handle);
#endif
        }

        VoidResult SetNonBlocking(SocketHandle handle)
        {
#if defined(_WIN32)
            u_long mode = 1;
            if (ioctlsocket(handle, FIONBIO, &mode) != 0)
            {
                return std::unexpected("failed to set socket non-blocking");
            }
#else
            const int flags = fcntl(handle, F_GETFL, 0);
            if (flags < 0 || fcntl(handle, F_SETFL, flags | O_NONBLOCK) < 0)
            {
                return std::unexpected("failed to set socket non-blocking");
            }
#endif
            return {};
        }
    }

    struct UdpTransport::Impl
    {
        SocketHandle Socket = InvalidSocket;
        vector<sockaddr_storage> Endpoints; // EndpointId indexes this table.
        vector<u8> ReceiveBuffer;

        Impl() { ReceiveBuffer.resize(ReceiveBufferSize); }

        ~Impl()
        {
            CloseSocket(Socket);
            PlatformShutdown();
        }

        // Interns a peer address, returning a stable EndpointId for it.
        EndpointId InternEndpoint(const sockaddr_storage& address, socklen_t length)
        {
            for (usize i = 0; i < Endpoints.size(); ++i)
            {
                if (Endpoints[i].ss_family == address.ss_family &&
                    std::memcmp(&Endpoints[i], &address, length) == 0)
                {
                    return static_cast<EndpointId>(i);
                }
            }
            Endpoints.push_back(address);
            return static_cast<EndpointId>(Endpoints.size() - 1);
        }
    };

    UdpTransport::UdpTransport(Unique<Impl> impl) : m_Impl(std::move(impl)) {}

    UdpTransport::~UdpTransport() = default;

    Result<Unique<UdpTransport>> UdpTransport::Bind(u16 port)
    {
        const VoidResult init = PlatformInit();
        if (!init.has_value())
        {
            return std::unexpected(init.error());
        }

        auto impl = CreateUnique<Impl>();
        impl->Socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (impl->Socket == InvalidSocket)
        {
            return std::unexpected("failed to create UDP socket");
        }

        const VoidResult nonBlocking = SetNonBlocking(impl->Socket);
        if (!nonBlocking.has_value())
        {
            return std::unexpected(nonBlocking.error());
        }

        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        local.sin_port = htons(port);
        if (bind(impl->Socket, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0)
        {
            return std::unexpected(fmt::format("failed to bind UDP socket to port {}", port));
        }

        return Unique<UdpTransport>(new UdpTransport(std::move(impl)));
    }

    Result<Unique<UdpTransport>> UdpTransport::Open()
    {
        // A client binds an ephemeral local port; the OS chooses it.
        return Bind(0);
    }

    VoidResult UdpTransport::Send(EndpointId to, std::span<const u8> bytes)
    {
        const auto index = static_cast<usize>(to);
        if (index >= m_Impl->Endpoints.size())
        {
            return std::unexpected("send to an unknown endpoint");
        }

        const sockaddr_storage& address = m_Impl->Endpoints[index];
        const socklen_t length = static_cast<socklen_t>(
            address.ss_family == AF_INET6 ? sizeof(sockaddr_in6) : sizeof(sockaddr_in));

        const auto sent = sendto(m_Impl->Socket, reinterpret_cast<const char*>(bytes.data()),
                                 static_cast<int>(bytes.size()), 0,
                                 reinterpret_cast<const sockaddr*>(&address), length);
        if (sent < 0)
        {
            return std::unexpected("UDP sendto failed");
        }
        return {};
    }

    optional<Datagram> UdpTransport::Receive()
    {
        sockaddr_storage source{};
        socklen_t sourceLength = sizeof(source);

        const auto received =
            recvfrom(m_Impl->Socket, reinterpret_cast<char*>(m_Impl->ReceiveBuffer.data()),
                     static_cast<int>(m_Impl->ReceiveBuffer.size()), 0,
                     reinterpret_cast<sockaddr*>(&source), &sourceLength);
        if (received < 0)
        {
            // Drained (would-block) or a transient error; either way, nothing to hand back.
            return {};
        }

        const EndpointId from = m_Impl->InternEndpoint(source, sourceLength);
        return Datagram{
            .From = from,
            .Bytes =
                std::span<const u8>(m_Impl->ReceiveBuffer.data(), static_cast<usize>(received)),
        };
    }

    Result<EndpointId> UdpTransport::Resolve(string_view host, u16 port)
    {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_protocol = IPPROTO_UDP;

        const string hostString(host);
        const string portString = fmt::format("{}", port);

        addrinfo* result = nullptr;
        const int rc = getaddrinfo(hostString.c_str(), portString.c_str(), &hints, &result);
        if (rc != 0 || result == nullptr)
        {
            return std::unexpected(fmt::format("failed to resolve host '{}'", hostString));
        }

        sockaddr_storage address{};
        std::memcpy(&address, result->ai_addr, result->ai_addrlen);
        const auto length = static_cast<socklen_t>(result->ai_addrlen);
        freeaddrinfo(result);

        return m_Impl->InternEndpoint(address, length);
    }

    Result<u16> UdpTransport::LocalPort() const
    {
        sockaddr_storage bound{};
        socklen_t length = sizeof(bound);
        if (getsockname(m_Impl->Socket, reinterpret_cast<sockaddr*>(&bound), &length) != 0)
        {
            return std::unexpected("failed to query the socket's local port");
        }
        const auto* inet = reinterpret_cast<const sockaddr_in*>(&bound);
        return ntohs(inet->sin_port);
    }
}
