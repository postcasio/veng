// Connection / channel cases: the two delivery disciplines proven over the
// in-process loopback pair and, where the point is loss tolerance, the seeded
// fault-injection wrapper. Time is injected through Update(now) — every case
// scripts its own clock, so all of this is deterministic with no wall clock and no
// device. Covered: unreliable latest-wins stale drop and u16 wraparound; reliable
// exactly-once in-order delivery under seeded drop / reorder / duplicate; resend
// backoff; keepalive emission, suppression under traffic, and timeout; reliable
// fragmentation (a multi-packet message whole and in order under faults, interleaved
// with small ones, and the one-packet framing of an unfragmented message); and the
// per-channel bound's rejection.

#include <doctest/doctest.h>

#include <Veng/Net/Connection.h>
#include <Veng/Net/FaultInjectionTransport.h>
#include <Veng/Net/LoopbackTransport.h>
#include <Veng/Net/Protocol.h>
#include <Veng/Net/Transport.h>

#include <utility>

using namespace Veng;
using namespace Veng::Net;

namespace
{
    vector<u8> IndexPayload(u32 index)
    {
        vector<u8> bytes;
        WriteU32LE(bytes, index);
        return bytes;
    }

    u32 ReadIndex(const vector<u8>& bytes)
    {
        return ReadU32LE(bytes, 0);
    }

    // A message of `size` bytes led by its index, the rest a pattern derived from both, so a
    // reassembly that misplaces, drops or duplicates any slice fails the comparison.
    vector<u8> SizedPayload(u32 index, usize size)
    {
        vector<u8> bytes;
        WriteU32LE(bytes, index);
        while (bytes.size() < size)
        {
            bytes.push_back(static_cast<u8>((bytes.size() * 31u) ^ (index * 7u)));
        }
        return bytes;
    }

    // A pass-through transport recording the channel byte of every datagram it sends.
    class ChannelByteTransport final : public Transport
    {
    public:
        explicit ChannelByteTransport(Transport& inner) : m_Inner(&inner) {}

        VoidResult Send(EndpointId to, std::span<const u8> bytes) override
        {
            if (bytes.size() > PacketHeaderSize)
            {
                ChannelBytes.push_back(bytes[4]);
                Sizes.push_back(bytes.size());
            }
            return m_Inner->Send(to, bytes);
        }

        optional<Datagram> Receive() override { return m_Inner->Receive(); }

        Result<EndpointId> Resolve(string_view host, u16 port) override
        {
            return m_Inner->Resolve(host, port);
        }

        vector<u8> ChannelBytes;
        vector<usize> Sizes;

    private:
        Transport* m_Inner;
    };

    // A pass-through transport that counts the reliable ack-only / keepalive
    // packets (empty-payload reliable datagrams) it sends. Used to prove keepalive
    // suppression under traffic.
    class CountingTransport final : public Transport
    {
    public:
        explicit CountingTransport(Transport& inner) : m_Inner(&inner) {}

        VoidResult Send(EndpointId to, std::span<const u8> bytes) override
        {
            const optional<PacketHeader> header = ReadPacketHeader(bytes);
            if (header.has_value() &&
                header->Channel == static_cast<u8>(Channel::ReliableOrdered) &&
                bytes.size() == PacketHeaderSize)
            {
                m_KeepaliveOrAckOnly += 1;
            }
            return m_Inner->Send(to, bytes);
        }

        optional<Datagram> Receive() override { return m_Inner->Receive(); }

        Result<EndpointId> Resolve(string_view host, u16 port) override
        {
            return m_Inner->Resolve(host, port);
        }

        [[nodiscard]] u32 KeepaliveOrAckOnly() const { return m_KeepaliveOrAckOnly; }

    private:
        Transport* m_Inner;
        u32 m_KeepaliveOrAckOnly = 0;
    };
}

TEST_CASE("reliable delivery arrives exactly once, in order, over a clean link")
{
    auto [la, lb] = LoopbackTransport::CreatePair();
    Connection client(*la, *la->Resolve("", 0));
    Connection server(*lb, EndpointId::None);

    constexpr u32 messageCount = 32;
    for (u32 i = 0; i < messageCount; ++i)
    {
        const vector<u8> payload = IndexPayload(i);
        REQUIRE(client.Send(Channel::ReliableOrdered, payload).has_value());
    }

    vector<u32> delivered;
    f64 now = 0.0;
    for (int tick = 0; tick < 200; ++tick)
    {
        now += 0.05;
        client.Update(now);
        server.Update(now);
        while (const optional<vector<u8>> message = server.Receive(Channel::ReliableOrdered))
        {
            delivered.push_back(ReadIndex(*message));
        }
    }

    REQUIRE(delivered.size() == messageCount);
    for (u32 i = 0; i < messageCount; ++i)
    {
        CHECK(delivered[i] == i);
    }
}

TEST_CASE("reliable delivery is exactly-once and in-order under seeded loss")
{
    const auto run = [](const FaultInjectionConfig& faults)
    {
        auto [la, lb] = LoopbackTransport::CreatePair();
        FaultInjectionTransport clientLink(*la, faults);
        FaultInjectionTransport serverLink(*lb, faults);

        Connection client(clientLink, *la->Resolve("", 0));
        Connection server(serverLink, EndpointId::None);

        constexpr u32 messageCount = 40;
        for (u32 i = 0; i < messageCount; ++i)
        {
            REQUIRE(client.Send(Channel::ReliableOrdered, IndexPayload(i)).has_value());
        }

        vector<u32> delivered;
        f64 now = 0.0;
        for (int tick = 0; tick < 4000; ++tick)
        {
            now += 0.02;
            client.Update(now);
            server.Update(now);
            while (const optional<vector<u8>> message = server.Receive(Channel::ReliableOrdered))
            {
                delivered.push_back(ReadIndex(*message));
            }
        }

        REQUIRE(delivered.size() == messageCount);
        for (u32 i = 0; i < messageCount; ++i)
        {
            CHECK(delivered[i] == i);
        }
    };

    SUBCASE("heavy drop")
    {
        run(FaultInjectionConfig{.DropRate = 0.4f, .Seed = 101});
    }
    SUBCASE("drop, reorder, and duplicate together")
    {
        run(FaultInjectionConfig{
            .DropRate = 0.2f, .DuplicateRate = 0.2f, .ReorderRate = 0.4f, .Seed = 202});
    }
    SUBCASE("severe reordering")
    {
        run(FaultInjectionConfig{.ReorderRate = 0.8f, .Seed = 303});
    }
}

TEST_CASE("unreliable delivery is latest-wins: stale datagrams are dropped")
{
    auto [la, lb] = LoopbackTransport::CreatePair();
    // Reorder the link so out-of-order (stale) datagrams actually occur.
    FaultInjectionTransport serverLink(*lb, FaultInjectionConfig{.ReorderRate = 0.6f, .Seed = 55});

    Connection client(*la, *la->Resolve("", 0));
    Connection server(serverLink, EndpointId::None);

    // Send a batch, then pump once, so the reorder buffer holds several datagrams
    // at a time and actually delivers some out of order.
    constexpr u32 messageCount = 64;
    for (u32 i = 0; i < messageCount; ++i)
    {
        REQUIRE(client.Send(Channel::UnreliableSequenced, IndexPayload(i)).has_value());
    }

    server.Update(0.1);

    vector<u32> delivered;
    while (const optional<vector<u8>> message = server.Receive(Channel::UnreliableSequenced))
    {
        delivered.push_back(ReadIndex(*message));
    }

    // Some datagrams arrived out of order (so fewer than all survive the latest-wins
    // filter), and whatever is handed up is a strictly increasing subsequence — a
    // stale (older-than-delivered) datagram is never delivered.
    REQUIRE(delivered.size() > 0);
    CHECK(delivered.size() < messageCount);
    for (usize i = 1; i < delivered.size(); ++i)
    {
        CHECK(delivered[i] > delivered[i - 1]);
    }
}

TEST_CASE("unreliable sequencing survives the u16 sequence wraparound")
{
    auto [la, lb] = LoopbackTransport::CreatePair();
    Connection client(*la, *la->Resolve("", 0));
    Connection server(*lb, EndpointId::None);

    // More than 65536 datagrams so the u16 packet sequence wraps at least once; on
    // a clean, in-order link every one is newer than the last and is delivered.
    constexpr u32 messageCount = 70000;
    for (u32 i = 0; i < messageCount; ++i)
    {
        REQUIRE(client.Send(Channel::UnreliableSequenced, IndexPayload(i)).has_value());
    }

    server.Update(1.0);

    u32 expected = 0;
    while (const optional<vector<u8>> message = server.Receive(Channel::UnreliableSequenced))
    {
        CHECK(ReadIndex(*message) == expected);
        ++expected;
    }
    CHECK(expected == messageCount);
}

TEST_CASE("a reliable message several packets long arrives whole and in order under faults")
{
    const auto run = [](const FaultInjectionConfig& faults)
    {
        auto [la, lb] = LoopbackTransport::CreatePair();
        FaultInjectionTransport clientLink(*la, faults);
        FaultInjectionTransport serverLink(*lb, faults);

        Connection client(clientLink, *la->Resolve("", 0));
        Connection server(serverLink, EndpointId::None);

        // Two, several and the most fragments a message may take, plus one exactly one byte into
        // a second fragment.
        const vector<usize> sizes{MaxUnfragmentedReliableMessageSize + 1,
                                  MaxReliableFragmentSize * 5 + 17, MaxReliableMessageSize,
                                  MaxReliableFragmentSize * 2};
        for (u32 i = 0; i < sizes.size(); ++i)
        {
            REQUIRE(client.Send(Channel::ReliableOrdered, SizedPayload(i, sizes[i])).has_value());
        }

        vector<vector<u8>> delivered;
        f64 now = 0.0;
        for (int tick = 0; tick < 2000 && delivered.size() < sizes.size(); ++tick)
        {
            now += 0.02;
            client.Update(now);
            server.Update(now);
            while (optional<vector<u8>> message = server.Receive(Channel::ReliableOrdered))
            {
                delivered.push_back(std::move(*message));
            }
        }

        REQUIRE(delivered.size() == sizes.size());
        u32 mismatched = 0;
        for (u32 i = 0; i < sizes.size(); ++i)
        {
            mismatched += delivered[i] == SizedPayload(i, sizes[i]) ? 0 : 1;
        }
        CHECK(mismatched == 0);

        // Exactly once: nothing further surfaces however long the link keeps running.
        for (int tick = 0; tick < 50; ++tick)
        {
            now += 0.02;
            client.Update(now);
            server.Update(now);
        }
        CHECK_FALSE(server.Receive(Channel::ReliableOrdered).has_value());
    };

    SUBCASE("heavy drop")
    {
        run(FaultInjectionConfig{.DropRate = 0.4f, .Seed = 404});
    }
    SUBCASE("drop, reorder, and duplicate together")
    {
        run(FaultInjectionConfig{
            .DropRate = 0.2f, .DuplicateRate = 0.3f, .ReorderRate = 0.5f, .Seed = 505});
    }
}

TEST_CASE("small and large reliable messages interleave in send order")
{
    auto [la, lb] = LoopbackTransport::CreatePair();
    FaultInjectionTransport serverLink(*lb, FaultInjectionConfig{.ReorderRate = 0.5f, .Seed = 606});
    Connection client(*la, *la->Resolve("", 0));
    Connection server(serverLink, EndpointId::None);

    constexpr u32 messageCount = 12;
    const auto sizeOf = [](u32 i) { return i % 3 == 1 ? MaxReliableFragmentSize * 3 + i : 8u + i; };
    for (u32 i = 0; i < messageCount; ++i)
    {
        REQUIRE(client.Send(Channel::ReliableOrdered, SizedPayload(i, sizeOf(i))).has_value());
    }

    vector<vector<u8>> delivered;
    f64 now = 0.0;
    for (int tick = 0; tick < 400 && delivered.size() < messageCount; ++tick)
    {
        now += 0.02;
        client.Update(now);
        server.Update(now);
        while (optional<vector<u8>> message = server.Receive(Channel::ReliableOrdered))
        {
            delivered.push_back(std::move(*message));
        }
    }

    REQUIRE(delivered.size() == messageCount);
    u32 mismatched = 0;
    for (u32 i = 0; i < messageCount; ++i)
    {
        mismatched += delivered[i] == SizedPayload(i, sizeOf(i)) ? 0 : 1;
    }
    CHECK(mismatched == 0);
}

TEST_CASE("a reliable message within one packet keeps the unfragmented framing")
{
    auto [la, lb] = LoopbackTransport::CreatePair();
    ChannelByteTransport recorder(*la);
    Connection client(recorder, *la->Resolve("", 0));

    const u8 plain = static_cast<u8>(Channel::ReliableOrdered);

    // At the unfragmented bound: one datagram, a plain channel byte, and a full-size datagram.
    REQUIRE(
        client.Send(Channel::ReliableOrdered, SizedPayload(0, MaxUnfragmentedReliableMessageSize))
            .has_value());
    client.Update(0.0);
    REQUIRE(recorder.ChannelBytes.size() == 1);
    CHECK(recorder.ChannelBytes[0] == plain);
    CHECK(recorder.Sizes[0] == MaxDatagramSize);

    // One byte past it: two datagrams, both flagged as fragments, neither past the datagram size.
    recorder.ChannelBytes.clear();
    recorder.Sizes.clear();
    REQUIRE(
        client
            .Send(Channel::ReliableOrdered, SizedPayload(1, MaxUnfragmentedReliableMessageSize + 1))
            .has_value());
    client.Update(0.001);
    REQUIRE(recorder.ChannelBytes.size() == 2);
    for (usize i = 0; i < recorder.ChannelBytes.size(); ++i)
    {
        CHECK(recorder.ChannelBytes[i] != plain);
        CHECK((recorder.ChannelBytes[i] & 0x7Fu) == plain);
        CHECK(recorder.Sizes[i] <= MaxDatagramSize);
    }
}

TEST_CASE("a message past its channel's bound is refused with an error")
{
    auto [la, lb] = LoopbackTransport::CreatePair();
    Connection client(*la, *la->Resolve("", 0));

    SUBCASE("reliable")
    {
        const vector<u8> tooBig(MaxReliableMessageSize + 1, 0xAB);
        const VoidResult result = client.Send(Channel::ReliableOrdered, tooBig);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().find("exceeds") != string::npos);

        const vector<u8> atLimit(MaxReliableMessageSize, 0xAB);
        CHECK(client.Send(Channel::ReliableOrdered, atLimit).has_value());
    }

    SUBCASE("unreliable")
    {
        const vector<u8> tooBig(MaxUnreliableMessageSize + 1, 0xCD);
        const VoidResult result = client.Send(Channel::UnreliableSequenced, tooBig);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().find("exceeds") != string::npos);

        const vector<u8> atLimit(MaxUnreliableMessageSize, 0xCD);
        CHECK(client.Send(Channel::UnreliableSequenced, atLimit).has_value());
    }
}

TEST_CASE("an unacked reliable message is resent with exponential backoff")
{
    auto [la, lb] = LoopbackTransport::CreatePair();

    // The peer never runs, so nothing is ever acked: every transmission is a resend.
    // Count how many times the message packet crosses the wire as time advances.
    // A reliable message packet carries a payload, so it is larger than a header.
    struct Counter final : public Transport
    {
        explicit Counter(Transport& inner) : Inner(&inner) {}
        VoidResult Send(EndpointId to, std::span<const u8> bytes) override
        {
            if (bytes.size() > PacketHeaderSize)
            {
                Transmissions.push_back(Now);
            }
            return Inner->Send(to, bytes);
        }
        optional<Datagram> Receive() override { return Inner->Receive(); }
        Result<EndpointId> Resolve(string_view host, u16 port) override
        {
            return Inner->Resolve(host, port);
        }
        Transport* Inner;
        f64 Now = 0.0;
        vector<f64> Transmissions;
    };

    Counter counter(*la);
    Connection sender(counter, *la->Resolve("", 0));

    const ConnectionConfig config; // ResendInterval 0.1, backoff cap 1.0
    REQUIRE(sender.Send(Channel::ReliableOrdered, IndexPayload(0)).has_value());

    f64 now = 0.0;
    for (int tick = 0; tick < 400; ++tick)
    {
        now += 0.01;
        counter.Now = now;
        sender.Update(now);
    }

    // First transmission is immediate; gaps then grow 0.1, 0.2, 0.4, 0.8, then cap
    // at 1.0. Check the first few inter-send gaps are increasing and start near the
    // base interval.
    REQUIRE(counter.Transmissions.size() >= 4);
    const f64 gap1 = counter.Transmissions[1] - counter.Transmissions[0];
    const f64 gap2 = counter.Transmissions[2] - counter.Transmissions[1];
    const f64 gap3 = counter.Transmissions[3] - counter.Transmissions[2];
    CHECK(gap1 == doctest::Approx(config.ResendInterval).epsilon(0.25));
    CHECK(gap2 > gap1);
    CHECK(gap3 > gap2);
}

TEST_CASE("a peer that goes silent trips the timeout flag")
{
    auto [la, lb] = LoopbackTransport::CreatePair();
    const ConnectionConfig config{.TimeoutInterval = 5.0};
    Connection client(*la, *la->Resolve("", 0), config);

    // The peer never responds; drive the clock past the timeout interval.
    f64 now = 0.0;
    CHECK_FALSE(client.TimedOut());
    for (int tick = 0; tick < 40; ++tick)
    {
        now += 0.2;
        client.Update(now);
    }
    CHECK(now > config.TimeoutInterval);
    CHECK(client.TimedOut());
}

TEST_CASE("keepalive keeps an idle connection alive, and traffic suppresses it")
{
    SUBCASE("mutual keepalive prevents timeout on an idle link")
    {
        auto [la, lb] = LoopbackTransport::CreatePair();
        Connection client(*la, *la->Resolve("", 0));
        Connection server(*lb, EndpointId::None);

        f64 now = 0.0;
        for (int tick = 0; tick < 200; ++tick) // 10s, well past the 5s timeout
        {
            now += 0.05;
            client.Update(now);
            server.Update(now);
        }
        CHECK_FALSE(client.TimedOut());
        CHECK_FALSE(server.TimedOut());
    }

    SUBCASE("continuous traffic suppresses keepalive packets")
    {
        const auto keepalivesSent = [](bool withTraffic)
        {
            auto [la, lb] = LoopbackTransport::CreatePair();
            CountingTransport clientLink(*la);
            Connection client(clientLink, *la->Resolve("", 0));
            Connection server(*lb, EndpointId::None);

            f64 now = 0.0;
            for (int tick = 0; tick < 200; ++tick)
            {
                now += 0.05;
                if (withTraffic)
                {
                    // Both ends send so neither falls idle.
                    (void)client.Send(Channel::UnreliableSequenced, IndexPayload(0));
                    (void)server.Send(Channel::UnreliableSequenced, IndexPayload(0));
                }
                client.Update(now);
                server.Update(now);
            }
            return clientLink.KeepaliveOrAckOnly();
        };

        CHECK(keepalivesSent(false) > 0); // idle: keepalives flow
        CHECK(keepalivesSent(true) == 0); // busy: suppressed
    }
}

TEST_CASE("the RTT estimate settles from a clean reliable round-trip")
{
    auto [la, lb] = LoopbackTransport::CreatePair();
    Connection client(*la, *la->Resolve("", 0));
    Connection server(*lb, EndpointId::None);

    REQUIRE(client.Send(Channel::ReliableOrdered, IndexPayload(0)).has_value());

    f64 now = 0.0;
    for (int tick = 0; tick < 20; ++tick)
    {
        now += 0.05;
        client.Update(now);
        server.Update(now);
    }

    // A round trip is ~one tick out and one tick back; the estimate is positive and
    // bounded well under a second.
    CHECK(client.RttEstimate() > 0.0f);
    CHECK(client.RttEstimate() < 0.2f);
}
