// Transport-layer cases: the in-process loopback pair, the seeded fault-injection
// wrapper, and the packet-header codec / sequence arithmetic in Net/Protocol.h.
// Everything here is pure and device-free — no socket, no wall clock. The
// UdpTransport path is exercised only for construction (a bind + resolve), since
// the reliability guarantees are all proven over loopback + fault injection.

#include <doctest/doctest.h>

#include <Veng/Net/Connection.h>
#include <Veng/Net/FaultInjectionTransport.h>
#include <Veng/Net/LoopbackTransport.h>
#include <Veng/Net/Protocol.h>
#include <Veng/Net/Transport.h>
#include <Veng/Net/UdpTransport.h>

#include <algorithm>
#include <array>
#include <utility>

using namespace Veng;
using namespace Veng::Net;

namespace
{
    vector<u8> Bytes(std::initializer_list<u8> values)
    {
        return vector<u8>(values);
    }

    // Drains a transport into a flat list of {From, payload-copy} for assertions.
    vector<std::pair<EndpointId, vector<u8>>> DrainAll(Transport& transport)
    {
        vector<std::pair<EndpointId, vector<u8>>> out;
        while (true)
        {
            const optional<Datagram> datagram = transport.Receive();
            if (!datagram.has_value())
            {
                break;
            }
            out.emplace_back(datagram->From,
                             vector<u8>(datagram->Bytes.begin(), datagram->Bytes.end()));
        }
        return out;
    }
}

TEST_CASE("loopback delivers datagrams to the peer with the sender's handle")
{
    auto [a, b] = LoopbackTransport::CreatePair();

    const Result<EndpointId> peerFromA = a->Resolve("ignored", 0);
    REQUIRE(peerFromA.has_value());
    CHECK(*peerFromA == b->Local());

    const vector<u8> payload = Bytes({1, 2, 3, 4});
    REQUIRE(a->Send(*peerFromA, payload).has_value());

    // Nothing loops back to the sender.
    CHECK(DrainAll(*a).empty());

    const auto received = DrainAll(*b);
    REQUIRE(received.size() == 1);
    CHECK(received[0].first == a->Local());
    CHECK(received[0].second == payload);
}

TEST_CASE("loopback preserves FIFO order and drains to empty")
{
    auto [a, b] = LoopbackTransport::CreatePair();
    const EndpointId peer = *a->Resolve("", 0);

    for (u8 i = 0; i < 5; ++i)
    {
        REQUIRE(a->Send(peer, Bytes({i})).has_value());
    }

    const auto received = DrainAll(*b);
    REQUIRE(received.size() == 5);
    for (u8 i = 0; i < 5; ++i)
    {
        CHECK(received[i].second == Bytes({i}));
    }
    CHECK(DrainAll(*b).empty());
}

TEST_CASE("fault injection with zero rates is a transparent passthrough")
{
    auto [a, b] = LoopbackTransport::CreatePair();
    FaultInjectionTransport lossy(*b, FaultInjectionConfig{.Seed = 1});

    const EndpointId peer = *a->Resolve("", 0);
    for (u8 i = 0; i < 8; ++i)
    {
        REQUIRE(a->Send(peer, Bytes({i})).has_value());
    }

    const auto received = DrainAll(lossy);
    REQUIRE(received.size() == 8);
    for (u8 i = 0; i < 8; ++i)
    {
        CHECK(received[i].second == Bytes({i}));
    }
}

TEST_CASE("fault injection drops a deterministic, seed-stable subset")
{
    const auto run = [](u64 seed)
    {
        auto [a, b] = LoopbackTransport::CreatePair();
        FaultInjectionTransport lossy(*b, FaultInjectionConfig{.DropRate = 0.5f, .Seed = seed});
        const EndpointId peer = *a->Resolve("", 0);
        for (u8 i = 0; i < 100; ++i)
        {
            REQUIRE(a->Send(peer, Bytes({i})).has_value());
        }
        return DrainAll(lossy);
    };

    const auto first = run(1234);
    const auto second = run(1234);

    // Some dropped, some survived — and identical across identical seeds.
    CHECK(first.size() < 100);
    CHECK(first.size() > 0);
    CHECK(first.size() == second.size());
    for (usize i = 0; i < first.size(); ++i)
    {
        CHECK(first[i].second == second[i].second);
    }

    // A different seed drops a different subset (compare which payloads survived —
    // robust even if the two counts happen to coincide).
    const auto other = run(9999);
    vector<u8> firstSurvivors;
    for (const auto& entry : first)
    {
        firstSurvivors.push_back(entry.second[0]);
    }
    vector<u8> otherSurvivors;
    for (const auto& entry : other)
    {
        otherSurvivors.push_back(entry.second[0]);
    }
    CHECK(firstSurvivors != otherSurvivors);
}

TEST_CASE("fault injection duplicates deliver payloads more than once")
{
    auto [a, b] = LoopbackTransport::CreatePair();
    FaultInjectionTransport lossy(*b, FaultInjectionConfig{.DuplicateRate = 1.0f, .Seed = 7});
    const EndpointId peer = *a->Resolve("", 0);

    for (u8 i = 0; i < 10; ++i)
    {
        REQUIRE(a->Send(peer, Bytes({i})).has_value());
    }

    // Every datagram duplicated: 20 out for 10 in.
    const auto received = DrainAll(lossy);
    CHECK(received.size() == 20);
}

TEST_CASE("fault injection reorders while preserving the multiset of payloads")
{
    auto [a, b] = LoopbackTransport::CreatePair();
    FaultInjectionTransport lossy(*b, FaultInjectionConfig{.ReorderRate = 0.5f, .Seed = 42});
    const EndpointId peer = *a->Resolve("", 0);

    for (u8 i = 0; i < 30; ++i)
    {
        REQUIRE(a->Send(peer, Bytes({i})).has_value());
    }

    const auto received = DrainAll(lossy);
    REQUIRE(received.size() == 30);

    vector<u8> order;
    for (const auto& entry : received)
    {
        order.push_back(entry.second[0]);
    }

    // No datagram lost or duplicated, but the sequence is not the identity.
    vector<u8> sorted = order;
    std::ranges::sort(sorted);
    for (u8 i = 0; i < 30; ++i)
    {
        CHECK(sorted[i] == i);
    }
    CHECK(order != sorted);
}

TEST_CASE("SequenceGreaterThan orders across the u16 wraparound")
{
    CHECK(SequenceGreaterThan(1, 0));
    CHECK_FALSE(SequenceGreaterThan(0, 1));
    CHECK_FALSE(SequenceGreaterThan(5, 5));

    // Just past the wrap: 0 is newer than 65535.
    CHECK(SequenceGreaterThan(0, 65535));
    CHECK_FALSE(SequenceGreaterThan(65535, 0));
    CHECK(SequenceGreaterThan(10, 65530));
    CHECK_FALSE(SequenceGreaterThan(65530, 10));
}

TEST_CASE("AckState tracks the sliding window of received sequences")
{
    AckState acks;

    SUBCASE("in-order arrivals accumulate the bitfield")
    {
        for (u16 seq = 0; seq <= 5; ++seq)
        {
            acks.Receive(seq);
        }
        CHECK(acks.RemoteSequence == 5);
        for (u16 seq = 0; seq <= 5; ++seq)
        {
            CHECK(acks.IsAcked(seq));
        }
        CHECK_FALSE(acks.IsAcked(6));
    }

    SUBCASE("an out-of-order older arrival sets its bit without moving the head")
    {
        acks.Receive(10);
        CHECK_FALSE(acks.IsAcked(7));
        acks.Receive(7);
        CHECK(acks.RemoteSequence == 10);
        CHECK(acks.IsAcked(7));
        CHECK(acks.IsAcked(10));
        CHECK_FALSE(acks.IsAcked(8));
    }

    SUBCASE("a large jump shifts old acks out of the 32-entry window")
    {
        acks.Receive(0);
        acks.Receive(100);
        CHECK(acks.RemoteSequence == 100);
        CHECK(acks.IsAcked(100));
        CHECK_FALSE(acks.IsAcked(0));
    }

    SUBCASE("the window tracks correctly across the wraparound")
    {
        acks.Receive(65534);
        acks.Receive(65535);
        acks.Receive(0);
        acks.Receive(1);
        CHECK(acks.RemoteSequence == 1);
        CHECK(acks.IsAcked(1));
        CHECK(acks.IsAcked(0));
        CHECK(acks.IsAcked(65535));
        CHECK(acks.IsAcked(65534));
    }
}

TEST_CASE("packet header round-trips field-by-field, little-endian")
{
    const PacketHeader original{
        .Magic = ProtocolMagic,
        .Channel = 1,
        .Sequence = 0xABCD,
        .Ack = 0x1234,
        .AckBits = 0xDEADBEEF,
    };

    vector<u8> buffer;
    WritePacketHeader(buffer, original);
    REQUIRE(buffer.size() == PacketHeaderSize);

    // Spot-check the byte order is little-endian at the sequence field (offset 5).
    CHECK(buffer[5] == 0xCD);
    CHECK(buffer[6] == 0xAB);

    const optional<PacketHeader> parsed = ReadPacketHeader(buffer);
    REQUIRE(parsed.has_value());
    CHECK(parsed->Magic == original.Magic);
    CHECK(parsed->Channel == original.Channel);
    CHECK(parsed->Sequence == original.Sequence);
    CHECK(parsed->Ack == original.Ack);
    CHECK(parsed->AckBits == original.AckBits);
}

TEST_CASE("a datagram too short for a header parses to nullopt")
{
    const vector<u8> tooShort(PacketHeaderSize - 1, 0);
    CHECK_FALSE(ReadPacketHeader(tooShort).has_value());
}

TEST_CASE("UdpTransport binds a local port and resolves a host")
{
    // Socket construction is deterministic on a normal host; the reliability
    // guarantees are covered over loopback, so this only proves the platform seam
    // constructs and resolves without touching the wire timing.
    const Result<Unique<UdpTransport>> server = UdpTransport::Bind(0);
    REQUIRE(server.has_value());

    const Result<u16> port = (*server)->LocalPort();
    REQUIRE(port.has_value());
    CHECK(*port != 0);

    const Result<Unique<UdpTransport>> client = UdpTransport::Open();
    REQUIRE(client.has_value());

    const Result<EndpointId> peer = (*client)->Resolve("127.0.0.1", *port);
    REQUIRE(peer.has_value());
    CHECK(*peer != EndpointId::None);
}
