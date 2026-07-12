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

            // The delay queue: latency + seeded uniform jitter, held until the injected clock reaches
            // the release time. Only draws the Rng when jitter is active, so a zero-jitter config
            // leaves the seeded fault sequence (and thus every existing scenario) byte-identical.
            f64 delaySeconds = static_cast<f64>(m_Config.LatencyMs) / 1000.0;
            if (m_Config.JitterMs > 0.0f)
            {
                const f32 jitter = (m_Rng.NextFloat() * 2.0f - 1.0f) * m_Config.JitterMs;
                delaySeconds += static_cast<f64>(jitter) / 1000.0;
            }
            if (delaySeconds < 0.0)
            {
                delaySeconds = 0.0;
            }
            const f64 release = m_Now + delaySeconds;

            m_Pending.push_back(
                Held{.From = incoming->From, .Bytes = bytes, .ReleaseTime = release});
            if (duplicate)
            {
                m_Pending.push_back(
                    Held{.From = incoming->From, .Bytes = bytes, .ReleaseTime = release});
            }
        }
    }

    optional<Datagram> FaultInjectionTransport::Receive()
    {
        Pump();

        // Find the first datagram whose delay has elapsed (all of them when latency/jitter are zero).
        usize first = m_Pending.size();
        for (usize i = 0; i < m_Pending.size(); ++i)
        {
            if (m_Pending[i].ReleaseTime <= m_Now)
            {
                first = i;
                break;
            }
        }
        if (first == m_Pending.size())
        {
            return {}; // nothing ready to surface yet
        }

        // Reorder by occasionally delivering the next ready datagram ahead of the first — a
        // deterministic adjacent swap among the ready set.
        usize index = first;
        usize second = m_Pending.size();
        for (usize i = first + 1; i < m_Pending.size(); ++i)
        {
            if (m_Pending[i].ReleaseTime <= m_Now)
            {
                second = i;
                break;
            }
        }
        if (second != m_Pending.size() && m_Rng.NextFloat() < m_Config.ReorderRate)
        {
            index = second;
        }

        Held held = std::move(m_Pending[static_cast<std::ptrdiff_t>(index)]);
        m_Pending.erase(m_Pending.begin() + static_cast<std::ptrdiff_t>(index));

        m_ReceiveScratch = std::move(held.Bytes);
        return Datagram{.From = held.From, .Bytes = std::span<const u8>(m_ReceiveScratch)};
    }
}
