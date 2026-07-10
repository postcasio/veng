#include <Veng/Net/FaultInjectionTransport.h>

#include <utility>

namespace Veng::Net
{
    FaultInjectionTransport::FaultInjectionTransport(Transport& inner,
                                                     const FaultInjectionConfig& config)
        : m_Inner(&inner), m_Config(config), m_Rng(config.Seed)
    {
    }

    VoidResult FaultInjectionTransport::Send(EndpointId to, std::span<const u8> bytes)
    {
        // Faults are injected on the receive path; sends pass straight through.
        return m_Inner->Send(to, bytes);
    }

    Result<EndpointId> FaultInjectionTransport::Resolve(string_view host, u16 port)
    {
        return m_Inner->Resolve(host, port);
    }

    void FaultInjectionTransport::Pump()
    {
        // Drain the inner transport into the pending queue, applying drop and
        // duplicate faults. Bytes are copied out immediately because the inner
        // transport's span is only valid until its next Receive.
        while (true)
        {
            const optional<Datagram> incoming = m_Inner->Receive();
            if (!incoming.has_value())
            {
                break;
            }

            const bool drop = m_Rng.NextFloat() < m_Config.DropRate;
            if (drop)
            {
                continue;
            }

            const vector<u8> bytes(incoming->Bytes.begin(), incoming->Bytes.end());
            const bool duplicate = m_Rng.NextFloat() < m_Config.DuplicateRate;

            m_Pending.push_back(Held{.From = incoming->From, .Bytes = bytes});
            if (duplicate)
            {
                m_Pending.push_back(Held{.From = incoming->From, .Bytes = bytes});
            }
        }
    }

    optional<Datagram> FaultInjectionTransport::Receive()
    {
        Pump();
        if (m_Pending.empty())
        {
            return {};
        }

        // Reorder by occasionally delivering the second pending datagram ahead of
        // the first — a deterministic adjacent swap.
        usize index = 0;
        if (m_Pending.size() >= 2 && m_Rng.NextFloat() < m_Config.ReorderRate)
        {
            index = 1;
        }

        Held held = std::move(m_Pending[static_cast<std::ptrdiff_t>(index)]);
        m_Pending.erase(m_Pending.begin() + static_cast<std::ptrdiff_t>(index));

        m_ReceiveScratch = std::move(held.Bytes);
        return Datagram{.From = held.From, .Bytes = std::span<const u8>(m_ReceiveScratch)};
    }
}
