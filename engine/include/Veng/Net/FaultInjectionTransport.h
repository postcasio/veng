#pragma once

#include <Veng/Math/Random.h>
#include <Veng/Net/Transport.h>
#include <Veng/Result.h>
#include <Veng/Veng.h>

#include <deque>
#include <span>

// Veng/Net/FaultInjectionTransport.h — a lossy-link wrapper over any Transport.
//
// Wraps an inner Transport and, on the receive path, applies seeded, deterministic
// drop / duplicate / reorder faults so the reliability layer above can be tested
// against an adversarial link with no real network and no wall clock — the same
// inputs always produce the same faults. Wrapping both ends of a pair yields a
// bidirectionally lossy channel. The knobs also serve as a net-sim harness later.

namespace Veng::Net
{
    /// @brief Seeded fault rates for a FaultInjectionTransport.
    ///
    /// Each rate is an independent per-datagram probability in [0, 1]. Faults are
    /// applied on the receive path: a dropped datagram never surfaces, a duplicated
    /// one surfaces twice, a reordered one is delivered after the datagram behind it.
    struct FaultInjectionConfig
    {
        /// @brief Probability a received datagram is dropped.
        f32 DropRate = 0.0f;
        /// @brief Probability a received datagram is delivered twice.
        f32 DuplicateRate = 0.0f;
        /// @brief Probability a received datagram is swapped past the next one.
        f32 ReorderRate = 0.0f;
        /// @brief Seed for the deterministic fault stream.
        u64 Seed = 0;
    };

    /// @brief Transport wrapper that injects seeded, deterministic receive-path faults.
    ///
    /// Forwards Send and Resolve straight through to the inner transport; on Receive
    /// it drains the inner transport, applies the configured faults through a seeded
    /// Rng, and hands out the surviving datagrams. Does not own the inner transport —
    /// the caller keeps it alive for the wrapper's lifetime.
    class VE_API FaultInjectionTransport final : public Transport
    {
    public:
        /// @brief Wraps an inner transport with a fault configuration.
        /// @param inner   The transport to wrap; must outlive this wrapper.
        /// @param config  Fault rates and seed.
        FaultInjectionTransport(Transport& inner, const FaultInjectionConfig& config);

        VoidResult Send(EndpointId to, std::span<const u8> bytes) override;
        optional<Datagram> Receive() override;
        Result<EndpointId> Resolve(string_view host, u16 port) override;

    private:
        struct Held
        {
            EndpointId From;
            vector<u8> Bytes;
        };

        void Pump();

        Transport* m_Inner;
        FaultInjectionConfig m_Config;
        Rng m_Rng;
        std::deque<Held> m_Pending;
        vector<u8> m_ReceiveScratch;
    };
}
