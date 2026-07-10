#include <Veng/Net/LoopbackTransport.h>

#include <deque>
#include <unordered_map>
#include <utility>

namespace Veng::Net
{
    // The shared in-process medium: one inbound queue per endpoint handle, keyed by
    // the raw handle value. Send appends to the destination's queue tagged with the
    // sender; Receive pops from the caller's own queue. Both endpoints of a pair
    // hold a Ref to the same Medium, so it lives until both are destroyed.
    struct LoopbackTransport::Medium
    {
        struct Packet
        {
            EndpointId From;
            vector<u8> Bytes;
        };

        std::unordered_map<u32, std::deque<Packet>> Queues;
    };

    LoopbackTransport::LoopbackTransport(Ref<Medium> medium, EndpointId self, EndpointId peer)
        : m_Medium(std::move(medium)), m_Self(self), m_Peer(peer)
    {
    }

    LoopbackTransport::~LoopbackTransport() = default;

    std::pair<Unique<LoopbackTransport>, Unique<LoopbackTransport>> LoopbackTransport::CreatePair()
    {
        const auto medium = CreateRef<Medium>();
        constexpr auto endpointA = static_cast<EndpointId>(1);
        constexpr auto endpointB = static_cast<EndpointId>(2);

        // Not std::make_unique: the constructor is private, so construct explicitly.
        auto a = Unique<LoopbackTransport>(new LoopbackTransport(medium, endpointA, endpointB));
        auto b = Unique<LoopbackTransport>(new LoopbackTransport(medium, endpointB, endpointA));
        return {std::move(a), std::move(b)};
    }

    VoidResult LoopbackTransport::Send(EndpointId to, std::span<const u8> bytes)
    {
        auto& queue = m_Medium->Queues[static_cast<u32>(to)];
        queue.push_back(
            Medium::Packet{.From = m_Self, .Bytes = vector<u8>(bytes.begin(), bytes.end())});
        return {};
    }

    optional<Datagram> LoopbackTransport::Receive()
    {
        const auto it = m_Medium->Queues.find(static_cast<u32>(m_Self));
        if (it == m_Medium->Queues.end() || it->second.empty())
        {
            return {};
        }

        Medium::Packet packet = std::move(it->second.front());
        it->second.pop_front();

        m_ReceiveScratch = std::move(packet.Bytes);
        return Datagram{.From = packet.From, .Bytes = std::span<const u8>(m_ReceiveScratch)};
    }

    Result<EndpointId> LoopbackTransport::Resolve(string_view /*host*/, u16 /*port*/)
    {
        // A loopback transport has exactly one peer, fixed at CreatePair.
        return m_Peer;
    }
}
