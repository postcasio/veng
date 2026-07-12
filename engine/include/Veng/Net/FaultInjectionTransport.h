#pragma once

#include <Veng/Math/Random.h>
#include <Veng/Net/Transport.h>
#include <Veng/Result.h>
#include <Veng/Veng.h>

#include <deque>
#include <span>

// Veng/Net/FaultInjectionTransport.h — the network-simulation wrapper over any Transport.
//
// Wraps an inner Transport and, on the receive path, applies seeded, deterministic
// drop / duplicate / reorder faults plus a latency + jitter delay queue, so the
// reliability and prediction layers above can be tested — and a human can play —
// against an adversarial link with no real network: the same inputs and injected
// time always produce the same faults. Wrapping both ends of a pair yields a
// bidirectionally lossy, laggy channel. It is the one adversity tool for tests and
// for the launcher's --netsim flag; SimulatedTransport is its consumer-facing name.

namespace Veng::Net
{
    /// @brief Seeded fault rates and delay for a FaultInjectionTransport.
    ///
    /// Each rate is an independent per-datagram probability in [0, 1]; the latency/jitter delay each
    /// received datagram before it surfaces. Faults apply on the receive path: a dropped datagram
    /// never surfaces, a duplicated one surfaces twice, a reordered one is delivered after the
    /// datagram behind it, and a delayed one is held until its release time (injected via SetTime).
    struct FaultInjectionConfig
    {
        /// @brief Probability a received datagram is dropped.
        f32 DropRate = 0.0f;
        /// @brief Probability a received datagram is delivered twice.
        f32 DuplicateRate = 0.0f;
        /// @brief Probability a received datagram is swapped past the next one.
        f32 ReorderRate = 0.0f;
        /// @brief Base one-way delay in milliseconds applied to every received datagram.
        f32 LatencyMs = 0.0f;
        /// @brief Uniform +/- jitter in milliseconds added to the base latency, seeded.
        f32 JitterMs = 0.0f;
        /// @brief Seed for the deterministic fault/jitter stream.
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

        /// @brief Sets the injected wall clock (seconds) the delay queue releases against.
        ///
        /// The harness (or Server/Client::Pump) advances this each frame; a datagram surfaces once
        /// the clock reaches its release time. With zero latency and jitter it is inert (every
        /// datagram is ready immediately), so a caller that never sets it keeps the v1 behavior.
        /// @param nowSeconds  The current injected time in seconds.
        void SetTime(f64 nowSeconds) { m_Now = nowSeconds; }

    private:
        struct Held
        {
            EndpointId From;
            vector<u8> Bytes;
            /// @brief The injected time at which this datagram may surface (arrival + latency +/- jitter).
            f64 ReleaseTime = 0.0;
        };

        void Pump();

        Transport* m_Inner;
        FaultInjectionConfig m_Config;
        Rng m_Rng;
        std::deque<Held> m_Pending;
        vector<u8> m_ReceiveScratch;
        f64 m_Now = 0.0;
    };

    /// @brief The consumer-facing name for the network-simulation transport (tests and --netsim).
    using SimulatedTransport = FaultInjectionTransport;
}
