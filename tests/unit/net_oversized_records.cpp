// A replicated component too large for one snapshot packet: it leaves the snapshot path for the
// reliable channel, one full record in flight at a time, newest value wins. The direct cases drive
// ReplicationServer and ReplicationClient over two bare scenes with the ack round trip made by hand;
// the loss case runs the same pair over a Connection on a loopback link that drops one fragment.

#include <doctest/doctest.h>

#include <Veng/Net/Connection.h>
#include <Veng/Net/LoopbackTransport.h>
#include <Veng/Net/Replication.h>
#include <Veng/Net/Transport.h>
#include <Veng/Net/WorldEnvelope.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

#include "support/TestComponents.h"

#include <algorithm>
#include <utility>

using namespace Veng;
using namespace Veng::Net;

namespace
{
    // A dependency-free spawn never touches the manager, so a never-dereferenced reference is safe
    // (the net_join_flow.cpp precedent).
    AssetManager& FakeAssets()
    {
        alignas(16) static unsigned char bytes[64]{};
        return *reinterpret_cast<AssetManager*>(bytes);
    }

    constexpr ConnectionId PeerConnection = 1;

    // The leading byte of a reliable component-state message.
    constexpr u8 ComponentStateMessageId = 18;

    // Comfortably past one unreliable packet, and well inside one reliable message.
    const string LargeA(3000, 'a');
    const string LargeB(3000, 'b');
    const string LargeC(3000, 'c');

    bool IsComponentState(const ReplicationMessage& message)
    {
        return message.Channel == Channel::ReliableOrdered && !message.Bytes.empty() &&
               message.Bytes[0] == ComponentStateMessageId;
    }

    u32 CountComponentStates(const vector<ReplicationMessage>& messages)
    {
        return static_cast<u32>(std::ranges::count_if(messages, IsComponentState));
    }

    // A server scene and its replication server, and the client scene its stream applies into. One
    // entity carries a small replicated score and a replicated text the cases grow past a packet.
    struct Pair
    {
        TypeRegistry ServerTypes;
        TypeRegistry ClientTypes;
        Unique<Scene> Server;
        Unique<Scene> Client;
        NetIdAllocator Allocator;
        ReplicationServer ReplServer{ReplicationServer::Settings{.SnapshotInterval = 1}};
        ReplicationClient ReplClient{[](AssetId) -> Ref<Prefab> { return nullptr; }};
        Entity Subject = Entity::Null;
        // The largest snapshot packet generated so far.
        usize LargestSnapshot = 0;

        Pair()
        {
            RegisterBuiltinTypes(ServerTypes);
            ServerTypes.Register<VengTest::TestScore>();
            ServerTypes.Register<VengTest::TestText>();
            RegisterBuiltinTypes(ClientTypes);
            ClientTypes.Register<VengTest::TestScore>();
            ClientTypes.Register<VengTest::TestText>();
            Server = Scene::Create(ServerTypes);
            Client = Scene::Create(ClientTypes);
            ReplServer.AddConnection(PeerConnection);

            Server->SetChangeTick(1);
            Subject = Server->CreateEntity();
            Server->Add<VengTest::TestScore>(Subject, VengTest::TestScore{.Value = 1});
            Server->Add<VengTest::TestText>(Subject, VengTest::TestText{.Value = "short"});
            AssignServerNetIds(*Server, Allocator);
        }

        // Stamps the tick's writes at @p tick, as a system running inside it would.
        void BeginTick(const u64 tick) { Server->SetChangeTick(tick); }

        vector<ReplicationMessage> Generate(const u64 tick)
        {
            vector<ReplicationMessage> messages =
                ReplServer.Generate(PeerConnection, *Server, tick);
            for (const ReplicationMessage& message : messages)
            {
                if (message.Channel == Channel::UnreliableSequenced)
                {
                    LargestSnapshot = std::max(LargestSnapshot, message.Bytes.size());
                }
            }
            return messages;
        }

        // Applies a stream and returns the tick of the last snapshot it carried, or zero.
        u64 Apply(const vector<ReplicationMessage>& messages)
        {
            u64 snapshotTick = 0;
            for (const ReplicationMessage& message : messages)
            {
                if (message.Channel == Channel::ReliableOrdered)
                {
                    ReplClient.ApplyReliable(message.Bytes, *Client, FakeAssets());
                }
                else
                {
                    snapshotTick = ReplClient.ApplySnapshot(message.Bytes, *Client).ServerTick;
                }
            }
            return snapshotTick;
        }

        // The client's acknowledgements of everything it has applied, as its input packet carries.
        void Ack(const u64 snapshotTick)
        {
            ReplServer.Acknowledge(PeerConnection, snapshotTick);
            ReplServer.AcknowledgeComponentState(PeerConnection,
                                                 ReplClient.GetAppliedStateSequence());
        }

        // Generates, applies and acknowledges one tick, returning what was generated.
        vector<ReplicationMessage> RoundTrip(const u64 tick)
        {
            vector<ReplicationMessage> messages = Generate(tick);
            Ack(Apply(messages));
            return messages;
        }

        [[nodiscard]] Entity Mirror() const
        {
            return ReplClient.Map().Lookup(std::as_const(*Server).TryGet<NetIdentity>(Subject)->Id);
        }

        [[nodiscard]] const string& ClientText() const
        {
            return std::as_const(*Client).TryGet<VengTest::TestText>(Mirror())->Value;
        }

        [[nodiscard]] i32 ClientScore() const
        {
            return std::as_const(*Client).TryGet<VengTest::TestScore>(Mirror())->Value;
        }

        void SetText(const string& value)
        {
            Server->Get<VengTest::TestText>(Subject).Value = value;
        }
    };
}

TEST_CASE("A component too large for one snapshot packet reaches the client with its value")
{
    Pair fx;
    fx.RoundTrip(1);
    REQUIRE_FALSE(fx.Mirror().IsNull());
    REQUIRE(fx.ClientText() == "short");

    fx.BeginTick(2);
    fx.SetText(LargeA);
    const vector<ReplicationMessage> messages = fx.RoundTrip(2);

    CHECK(CountComponentStates(messages) == 1);
    CHECK(fx.LargestSnapshot <= MaxEnvelopedUnreliablePayload);
    CHECK(fx.ClientText() == LargeA);
}

TEST_CASE("Changes made while a component-state record is outstanding send the newest value once")
{
    Pair fx;
    fx.RoundTrip(1);

    fx.BeginTick(2);
    fx.SetText(LargeA);
    vector<ReplicationMessage> messages = fx.Generate(2);
    REQUIRE(CountComponentStates(messages) == 1);
    // Applied, but the acknowledgement has not reached the server yet.
    fx.Apply(messages);

    u32 sentWhileOutstanding = 0;
    for (const auto& [tick, value] : {std::pair{3, &LargeB}, std::pair{4, &LargeC}})
    {
        fx.BeginTick(tick);
        fx.SetText(*value);
        sentWhileOutstanding += CountComponentStates(fx.Generate(tick));
    }
    CHECK(sentWhileOutstanding == 0);
    CHECK(fx.ClientText() == LargeA);

    fx.Ack(0);
    fx.BeginTick(5);
    messages = fx.RoundTrip(5);
    CHECK(CountComponentStates(messages) == 1);
    CHECK(fx.ClientText() == LargeC);
}

TEST_CASE("An unchanged oversized component is not sent again, keyframes included")
{
    Pair fx;
    fx.RoundTrip(1);
    fx.BeginTick(2);
    fx.SetText(LargeA);
    REQUIRE(CountComponentStates(fx.RoundTrip(2)) == 1);

    // Past a keyframe, with the entity's small component changing each tick so every tick
    // snapshots the entity.
    u32 sent = 0;
    for (u64 tick = 3; tick < 24; ++tick)
    {
        fx.BeginTick(tick);
        fx.Server->Get<VengTest::TestScore>(fx.Subject).Value = static_cast<i32>(tick);
        sent += CountComponentStates(fx.RoundTrip(tick));
    }
    CHECK(sent == 0);
    CHECK(fx.LargestSnapshot <= MaxEnvelopedUnreliablePayload);
    CHECK(fx.ClientScore() == 23);
    CHECK(fx.ClientText() == LargeA);
}

TEST_CASE("A snapshot sent before a component went reliable cannot overwrite its newer record")
{
    Pair fx;
    fx.RoundTrip(1);

    // A small change rides a snapshot, which the link holds back...
    fx.BeginTick(2);
    fx.SetText("medium");
    const vector<ReplicationMessage> early = fx.Generate(2);
    REQUIRE(CountComponentStates(early) == 0);

    // ...until after the component has grown past a packet and its reliable record has landed.
    fx.BeginTick(3);
    fx.SetText(LargeA);
    fx.Apply(fx.Generate(3));
    REQUIRE(fx.ClientText() == LargeA);

    fx.Apply(early);
    CHECK(fx.ClientText() == LargeA);
}

// ---- Over a lossy link -------------------------------------------------------------------------

namespace
{
    // A pass-through transport that, once armed, drops the next full-size datagram it sends: the
    // first fragment of a reliable message larger than one packet.
    class DropFirstFragmentTransport final : public Transport
    {
    public:
        explicit DropFirstFragmentTransport(Transport& inner) : m_Inner(&inner) {}

        VoidResult Send(EndpointId to, std::span<const u8> bytes) override
        {
            if (Armed && bytes.size() == MaxDatagramSize)
            {
                Armed = false;
                Dropped = true;
                return {};
            }
            return m_Inner->Send(to, bytes);
        }

        optional<Datagram> Receive() override { return m_Inner->Receive(); }

        Result<EndpointId> Resolve(string_view host, u16 port) override
        {
            return m_Inner->Resolve(host, port);
        }

        bool Armed = false;
        bool Dropped = false;

    private:
        Transport* m_Inner;
    };
}

TEST_CASE("A dropped fragment behind a later snapshot still converges on the newest value")
{
    Pair fx;
    auto [serverLink, clientLink] = LoopbackTransport::CreatePair();
    DropFirstFragmentTransport lossy(*serverLink);
    Connection serverSide(lossy, *serverLink->Resolve("", 0));
    Connection clientSide(*clientLink, EndpointId::None);

    f64 now = 0.0;
    u32 componentStatesSent = 0;
    const auto step = [&](const u64 tick)
    {
        now += 1.0 / 60.0;
        fx.ReplServer.Generate(PeerConnection, *fx.Server, tick, nullptr,
                               [&](const ReplicationMessage& message) -> VoidResult
                               {
                                   componentStatesSent += IsComponentState(message) ? 1 : 0;
                                   return serverSide.Send(message.Channel, message.Bytes);
                               });
        serverSide.Update(now);
        clientSide.Update(now);

        u64 snapshotTick = 0;
        while (const optional<vector<u8>> message = clientSide.Receive(Channel::ReliableOrdered))
        {
            fx.ReplClient.ApplyReliable(*message, *fx.Client, FakeAssets());
        }
        while (const optional<vector<u8>> packet = clientSide.Receive(Channel::UnreliableSequenced))
        {
            snapshotTick = fx.ReplClient.ApplySnapshot(*packet, *fx.Client).ServerTick;
        }
        fx.Ack(snapshotTick);
    };

    u64 tick = 1;
    for (; tick < 10; ++tick)
    {
        step(tick);
    }
    REQUIRE_FALSE(fx.Mirror().IsNull());

    // The grown text goes out reliably with its first fragment lost; the next tick's snapshot of
    // the same entity overtakes it, and a further change waits behind the outstanding record.
    lossy.Armed = true;
    fx.BeginTick(tick);
    fx.SetText(LargeA);
    step(tick++);
    REQUIRE(lossy.Dropped);

    fx.BeginTick(tick);
    fx.Server->Get<VengTest::TestScore>(fx.Subject).Value = 7;
    fx.SetText(LargeB);
    step(tick++);
    REQUIRE(fx.ClientScore() == 7);
    REQUIRE(fx.ClientText() == "short");

    for (const u64 end = tick + 60; tick < end; ++tick)
    {
        step(tick);
    }
    CHECK(fx.ClientText() == LargeB);
    CHECK(fx.ClientScore() == 7);
    CHECK(componentStatesSent == 2);
}
