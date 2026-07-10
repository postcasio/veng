#include <Veng/Net/Connection.h>

#include <Veng/Log.h>
#include <Veng/Net/Protocol.h>
#include <Veng/Net/Transport.h>

#include <algorithm>
#include <deque>
#include <unordered_map>
#include <utility>

namespace Veng::Net
{
    namespace
    {
        constexpr usize ChannelCount = 2;
        constexpr usize ReliableMessageHeaderSize = 2; // u16 message id

        // Smoothing weight for the round-trip-time estimate (an EWMA).
        constexpr f64 RttSmoothing = 0.1;
    }

    struct Connection::State
    {
        // Per-channel sliding-window ack tracking (what we send back as ack + bits)
        // plus our own outgoing sequence counter.
        struct ChannelState
        {
            u16 LocalSequence = 0;
            AckState Acks;
        };

        // An unacked reliable message awaiting delivery confirmation.
        struct OutgoingMessage
        {
            u16 Id = 0;
            vector<u8> Bytes;
            f64 LastSentTime = 0.0;
            u32 SendCount = 0;
        };

        // A reliable packet we transmitted, so an incoming ack of its sequence can
        // resolve the message it carried (and sample RTT).
        struct SentPacket
        {
            u16 MessageId = 0;
            f64 SendTime = 0.0;
        };

        Transport* Transport = nullptr;
        EndpointId Peer = EndpointId::None;
        ConnectionConfig Config;

        f64 Now = 0.0;
        f64 LastSendTime = 0.0;
        f64 LastReceiveTime = 0.0;
        bool Started = false;
        bool TimedOut = false;
        f64 Rtt = 0.0;

        ChannelState Channels[ChannelCount];

        // Unreliable receive: latest-wins tracking + delivery queue.
        u16 UnreliableDelivered = 0;
        bool HasUnreliableDelivered = false;
        std::deque<vector<u8>> UnreliableInbox;

        // Reliable send.
        u16 NextMessageId = 0;
        std::deque<OutgoingMessage> ReliableOutbox;
        std::unordered_map<u16, SentPacket> ReliableSentPackets;
        bool ReliableAckPending = false;

        // Reliable receive: in-order delivery with a reorder buffer.
        u16 ReliableExpectedId = 0;
        std::unordered_map<u16, vector<u8>> ReliableReorder;
        std::deque<vector<u8>> ReliableInbox;

        vector<u8> SendScratch;

        ChannelState& ChannelFor(Channel channel) { return Channels[static_cast<usize>(channel)]; }

        // Builds a packet header for the channel and transmits header + payload.
        // Returns the sequence the packet was sent under.
        u16 SendPacket(Channel channel, std::span<const u8> payload)
        {
            ChannelState& cs = ChannelFor(channel);

            const PacketHeader header{
                .Magic = ProtocolMagic,
                .Channel = static_cast<u8>(channel),
                .Sequence = cs.LocalSequence,
                .Ack = cs.Acks.HasRemote ? cs.Acks.RemoteSequence : static_cast<u16>(0),
                .AckBits = cs.Acks.AckBits,
            };

            SendScratch.clear();
            WritePacketHeader(SendScratch, header);
            SendScratch.insert(SendScratch.end(), payload.begin(), payload.end());

            const VoidResult sent = Transport->Send(Peer, SendScratch);
            if (!sent.has_value())
            {
                Log::Warn("Net::Connection send failed: {}", sent.error());
            }

            const u16 sequence = cs.LocalSequence;
            cs.LocalSequence = static_cast<u16>(cs.LocalSequence + 1);
            LastSendTime = Now;
            return sequence;
        }

        // Resend interval for a message with `sendCount` prior transmissions:
        // base, then doubling per resend, capped at ResendBackoffMax.
        [[nodiscard]] f64 ResendIntervalFor(u32 sendCount) const
        {
            f64 interval = Config.ResendInterval;
            for (u32 i = 1; i < sendCount; ++i)
            {
                interval *= 2.0;
                if (interval >= Config.ResendBackoffMax)
                {
                    return Config.ResendBackoffMax;
                }
            }
            return std::min(interval, Config.ResendBackoffMax);
        }

        void TransmitReliable(OutgoingMessage& message)
        {
            vector<u8> payload;
            payload.reserve(ReliableMessageHeaderSize + message.Bytes.size());
            WriteU16LE(payload, message.Id);
            payload.insert(payload.end(), message.Bytes.begin(), message.Bytes.end());

            const u16 sequence = SendPacket(Channel::ReliableOrdered, payload);
            ReliableSentPackets[sequence] = SentPacket{.MessageId = message.Id, .SendTime = Now};

            message.LastSentTime = Now;
            message.SendCount += 1;
        }

        // Drops a fully-acked message and every sent-packet record that referenced
        // it (a message may have been resent under several sequences).
        void RetireMessage(u16 messageId)
        {
            std::erase_if(ReliableOutbox,
                          [messageId](const OutgoingMessage& m) { return m.Id == messageId; });
            std::erase_if(ReliableSentPackets, [messageId](const auto& entry)
                          { return entry.second.MessageId == messageId; });
        }

        void AckPacketSequence(u16 sequence)
        {
            const auto it = ReliableSentPackets.find(sequence);
            if (it == ReliableSentPackets.end())
            {
                return;
            }

            const u16 messageId = it->second.MessageId;
            const f64 sample = Now - it->second.SendTime;
            if (sample >= 0.0)
            {
                Rtt = Rtt + RttSmoothing * (sample - Rtt);
            }

            RetireMessage(messageId);
        }

        // Applies the peer's ack + ack bitfield (carried in a reliable packet) to
        // our outstanding reliable packets.
        void ProcessAcks(const PacketHeader& header)
        {
            AckPacketSequence(header.Ack);
            for (u32 i = 0; i < 32; ++i)
            {
                if ((header.AckBits & (1u << i)) != 0)
                {
                    AckPacketSequence(static_cast<u16>(header.Ack - 1 - i));
                }
            }
        }

        void DeliverReliable(u16 id, std::span<const u8> body)
        {
            if (id == ReliableExpectedId)
            {
                ReliableInbox.push_back(vector<u8>(body.begin(), body.end()));
                ReliableExpectedId = static_cast<u16>(ReliableExpectedId + 1);

                // Drain any buffered successors now made contiguous.
                while (true)
                {
                    const auto it = ReliableReorder.find(ReliableExpectedId);
                    if (it == ReliableReorder.end())
                    {
                        break;
                    }
                    ReliableInbox.push_back(std::move(it->second));
                    ReliableReorder.erase(it);
                    ReliableExpectedId = static_cast<u16>(ReliableExpectedId + 1);
                }
            }
            else if (SequenceGreaterThan(id, ReliableExpectedId))
            {
                // A future message: buffer it until the gap ahead fills in. A
                // duplicate of an already-buffered id is ignored.
                if (!ReliableReorder.contains(id))
                {
                    ReliableReorder[id] = vector<u8>(body.begin(), body.end());
                }
            }
            // Otherwise the id is older than expected — already delivered; drop it.
        }

        void HandleDatagram(std::span<const u8> bytes)
        {
            const optional<PacketHeader> header = ReadPacketHeader(bytes);
            if (!header.has_value() || header->Magic != ProtocolMagic)
            {
                return;
            }
            if (header->Channel >= ChannelCount)
            {
                return;
            }

            const auto channel = static_cast<Channel>(header->Channel);
            ChannelState& cs = ChannelFor(channel);
            cs.Acks.Receive(header->Sequence);

            const std::span<const u8> payload = bytes.subspan(PacketHeaderSize);

            if (channel == Channel::UnreliableSequenced)
            {
                if (payload.empty())
                {
                    return;
                }
                if (!HasUnreliableDelivered ||
                    SequenceGreaterThan(header->Sequence, UnreliableDelivered))
                {
                    HasUnreliableDelivered = true;
                    UnreliableDelivered = header->Sequence;
                    UnreliableInbox.push_back(vector<u8>(payload.begin(), payload.end()));
                }
                return;
            }

            // ReliableOrdered: the peer's ack fields resolve our outstanding
            // messages; a non-empty payload carries one message to deliver.
            ProcessAcks(*header);
            if (payload.size() >= ReliableMessageHeaderSize)
            {
                const u16 id = ReadU16LE(payload, 0);
                const std::span<const u8> body = payload.subspan(ReliableMessageHeaderSize);
                DeliverReliable(id, body);
                ReliableAckPending = true;
            }
        }

        void ReceivePump()
        {
            while (true)
            {
                const optional<Datagram> datagram = Transport->Receive();
                if (!datagram.has_value())
                {
                    break;
                }
                if (Peer == EndpointId::None)
                {
                    Peer = datagram->From;
                }
                else if (datagram->From != Peer)
                {
                    // Point-to-point: ignore datagrams from any other endpoint.
                    continue;
                }
                HandleDatagram(datagram->Bytes);
                LastReceiveTime = Now;
            }
        }
    };

    Connection::Connection(Transport& transport, EndpointId peer, const ConnectionConfig& config)
        : m_State(CreateUnique<State>())
    {
        m_State->Transport = &transport;
        m_State->Peer = peer;
        m_State->Config = config;
        m_State->Rtt = config.ResendInterval;
    }

    Connection::~Connection() = default;

    VoidResult Connection::Send(Channel channel, std::span<const u8> message)
    {
        State& s = *m_State;

        if (channel == Channel::UnreliableSequenced)
        {
            if (message.size() > MaxUnreliableMessageSize)
            {
                return std::unexpected(
                    fmt::format("unreliable message of {} bytes exceeds the {}-byte MTU budget",
                                message.size(), MaxUnreliableMessageSize));
            }
            s.SendPacket(Channel::UnreliableSequenced, message);
            return {};
        }

        if (message.size() > MaxReliableMessageSize)
        {
            return std::unexpected(
                fmt::format("reliable message of {} bytes exceeds the {}-byte MTU budget "
                            "(fragmentation is a named follow-on)",
                            message.size(), MaxReliableMessageSize));
        }

        s.ReliableOutbox.push_back(State::OutgoingMessage{
            .Id = s.NextMessageId,
            .Bytes = vector<u8>(message.begin(), message.end()),
            .LastSentTime = 0.0,
            .SendCount = 0,
        });
        s.NextMessageId = static_cast<u16>(s.NextMessageId + 1);
        return {};
    }

    void Connection::Update(f64 now)
    {
        State& s = *m_State;

        if (!s.Started)
        {
            s.Started = true;
            s.LastReceiveTime = now;
            s.LastSendTime = now;
        }
        s.Now = now;

        s.ReceivePump();

        // Transmit never-sent messages and resend those past their backed-off RTO.
        bool sentReliable = false;
        for (State::OutgoingMessage& message : s.ReliableOutbox)
        {
            const bool neverSent = message.SendCount == 0;
            const bool due = (now - message.LastSentTime) >= s.ResendIntervalFor(message.SendCount);
            if (neverSent || due)
            {
                s.TransmitReliable(message);
                sentReliable = true;
            }
        }

        // Flush an owed ack: piggybacked if a message went out, else an ack-only
        // packet. Ack-only packets carry no message, so they never make the peer
        // owe an ack in return — no ping-pong.
        if (s.ReliableAckPending)
        {
            if (!sentReliable)
            {
                s.SendPacket(Channel::ReliableOrdered, {});
            }
            s.ReliableAckPending = false;
        }

        // Keepalive when idle; any real traffic above already refreshed LastSendTime,
        // so traffic suppresses it.
        if ((now - s.LastSendTime) >= s.Config.KeepaliveInterval)
        {
            s.SendPacket(Channel::ReliableOrdered, {});
        }

        if ((now - s.LastReceiveTime) >= s.Config.TimeoutInterval)
        {
            s.TimedOut = true;
        }
    }

    optional<vector<u8>> Connection::Receive(Channel channel)
    {
        State& s = *m_State;
        std::deque<vector<u8>>& inbox =
            channel == Channel::UnreliableSequenced ? s.UnreliableInbox : s.ReliableInbox;
        if (inbox.empty())
        {
            return {};
        }
        vector<u8> message = std::move(inbox.front());
        inbox.pop_front();
        return message;
    }

    bool Connection::TimedOut() const
    {
        return m_State->TimedOut;
    }

    f32 Connection::RttEstimate() const
    {
        return static_cast<f32>(m_State->Rtt);
    }

    EndpointId Connection::Peer() const
    {
        return m_State->Peer;
    }
}
