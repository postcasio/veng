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
        constexpr usize ReliableMessageHeaderSize = 2;  // u16 message id
        constexpr usize ReliableFragmentHeaderSize = 4; // u16 message id, u8 index, u8 count

        // Set on a reliable packet's channel byte when its payload is one fragment of a larger
        // message. Unfragmented packets keep the plain channel byte, so their framing is the same
        // in every protocol version and a peer of another version can still read the handshake.
        constexpr u8 FragmentChannelFlag = 0x80;

        static_assert(MaxReliableFragmentCount <= 255, "a fragment's index and count are one byte");

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

        // An unacked reliable unit awaiting delivery confirmation: a whole message, or one
        // fragment of a larger one. Each unit takes its own id, so fragments are resent, acked and
        // ordered exactly like whole messages.
        struct OutgoingMessage
        {
            u16 Id = 0;
            vector<u8> Bytes;
            f64 LastSentTime = 0.0;
            u32 SendCount = 0;
            // Zero for a whole message; otherwise the fragment count, with Index this unit's slot.
            u8 FragmentCount = 0;
            u8 FragmentIndex = 0;
        };

        // A received reliable unit held until the units ahead of it arrive.
        struct IncomingUnit
        {
            vector<u8> Bytes;
            u8 FragmentCount = 0;
            u8 FragmentIndex = 0;
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

        // Reliable receive: in-order delivery with a reorder buffer, and the fragments of the
        // message being reassembled.
        u16 ReliableExpectedId = 0;
        std::unordered_map<u16, IncomingUnit> ReliableReorder;
        std::deque<vector<u8>> ReliableInbox;
        vector<u8> Reassembly;
        u8 ReassemblyCount = 0;
        u8 ReassemblyNext = 0;

        vector<u8> SendScratch;

        ChannelState& ChannelFor(Channel channel) { return Channels[static_cast<usize>(channel)]; }

        // Builds a packet header for the channel and transmits header + payload.
        // Returns the sequence the packet was sent under.
        u16 SendPacket(Channel channel, std::span<const u8> payload, bool fragment = false)
        {
            ChannelState& cs = ChannelFor(channel);

            const PacketHeader header{
                .Magic = ProtocolMagic,
                .Channel = static_cast<u8>(static_cast<u8>(channel) |
                                           (fragment ? FragmentChannelFlag : u8{0})),
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
            const bool fragment = message.FragmentCount != 0;
            vector<u8> payload;
            payload.reserve(ReliableFragmentHeaderSize + message.Bytes.size());
            WriteU16LE(payload, message.Id);
            if (fragment)
            {
                payload.push_back(message.FragmentIndex);
                payload.push_back(message.FragmentCount);
            }
            payload.insert(payload.end(), message.Bytes.begin(), message.Bytes.end());

            const u16 sequence = SendPacket(Channel::ReliableOrdered, payload, fragment);
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

        void DropReassembly()
        {
            Reassembly.clear();
            ReassemblyCount = 0;
            ReassemblyNext = 0;
        }

        // Takes the next in-order unit: a whole message is delivered as-is, and a fragment joins
        // the message being reassembled, which is delivered once its last fragment lands. Units
        // arrive strictly in id order, so the fragments of one message are contiguous; a unit that
        // breaks that shape can only come from a malformed peer, and discards the partial message.
        void AcceptUnit(IncomingUnit unit)
        {
            if (unit.FragmentCount == 0)
            {
                if (ReassemblyCount != 0)
                {
                    Log::Warn("Net::Connection dropping a partial reliable message: a whole "
                              "message arrived before its last fragment");
                    DropReassembly();
                }
                ReliableInbox.push_back(std::move(unit.Bytes));
                return;
            }

            const bool wellFormed = unit.FragmentIndex < unit.FragmentCount &&
                                    unit.FragmentCount <= MaxReliableFragmentCount;
            const bool starts = wellFormed && unit.FragmentIndex == 0 && ReassemblyCount == 0;
            const bool continues = wellFormed && ReassemblyCount != 0 &&
                                   unit.FragmentCount == ReassemblyCount &&
                                   unit.FragmentIndex == ReassemblyNext;
            if (!starts && !continues)
            {
                Log::Warn("Net::Connection dropping a malformed reliable fragment ({} of {})",
                          unit.FragmentIndex, unit.FragmentCount);
                DropReassembly();
                return;
            }
            if (Reassembly.size() + unit.Bytes.size() > MaxReliableMessageSize)
            {
                Log::Warn("Net::Connection dropping a reliable message reassembling past the {} "
                          "byte bound",
                          MaxReliableMessageSize);
                DropReassembly();
                return;
            }

            ReassemblyCount = unit.FragmentCount;
            Reassembly.insert(Reassembly.end(), unit.Bytes.begin(), unit.Bytes.end());
            ReassemblyNext = static_cast<u8>(unit.FragmentIndex + 1);
            if (ReassemblyNext == ReassemblyCount)
            {
                ReliableInbox.push_back(std::move(Reassembly));
                DropReassembly();
            }
        }

        void DeliverReliable(u16 id, IncomingUnit unit)
        {
            if (id == ReliableExpectedId)
            {
                AcceptUnit(std::move(unit));
                ReliableExpectedId = static_cast<u16>(ReliableExpectedId + 1);

                // Drain any buffered successors now made contiguous.
                while (true)
                {
                    const auto it = ReliableReorder.find(ReliableExpectedId);
                    if (it == ReliableReorder.end())
                    {
                        break;
                    }
                    AcceptUnit(std::move(it->second));
                    ReliableReorder.erase(it);
                    ReliableExpectedId = static_cast<u16>(ReliableExpectedId + 1);
                }
            }
            else if (SequenceGreaterThan(id, ReliableExpectedId))
            {
                // A future unit: buffer it until the gap ahead fills in. A
                // duplicate of an already-buffered id is ignored.
                if (!ReliableReorder.contains(id))
                {
                    ReliableReorder.emplace(id, std::move(unit));
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
            const bool fragment = (header->Channel & FragmentChannelFlag) != 0;
            const u8 channelBits = static_cast<u8>(header->Channel & ~FragmentChannelFlag);
            if (channelBits >= ChannelCount ||
                (fragment && channelBits != static_cast<u8>(Channel::ReliableOrdered)))
            {
                return;
            }

            const auto channel = static_cast<Channel>(channelBits);
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
                    UnreliableInbox.emplace_back(payload.begin(), payload.end());
                }
                return;
            }

            // ReliableOrdered: the peer's ack fields resolve our outstanding
            // messages; a non-empty payload carries one message to deliver.
            ProcessAcks(*header);
            if (fragment)
            {
                if (payload.size() > ReliableFragmentHeaderSize)
                {
                    const u16 id = ReadU16LE(payload, 0);
                    const std::span<const u8> body = payload.subspan(ReliableFragmentHeaderSize);
                    DeliverReliable(id, IncomingUnit{.Bytes = vector<u8>(body.begin(), body.end()),
                                                     .FragmentCount = payload[3],
                                                     .FragmentIndex = payload[2]});
                    ReliableAckPending = true;
                }
            }
            else if (payload.size() >= ReliableMessageHeaderSize)
            {
                const u16 id = ReadU16LE(payload, 0);
                const std::span<const u8> body = payload.subspan(ReliableMessageHeaderSize);
                DeliverReliable(id, IncomingUnit{.Bytes = vector<u8>(body.begin(), body.end())});
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
                fmt::format("reliable message of {} bytes exceeds the {}-byte reliable bound",
                            message.size(), MaxReliableMessageSize));
        }

        if (message.size() <= MaxUnfragmentedReliableMessageSize)
        {
            s.ReliableOutbox.push_back(State::OutgoingMessage{
                .Id = s.NextMessageId,
                .Bytes = vector<u8>(message.begin(), message.end()),
            });
            s.NextMessageId = static_cast<u16>(s.NextMessageId + 1);
            return {};
        }

        const usize count =
            (message.size() + MaxReliableFragmentSize - 1) / MaxReliableFragmentSize;
        for (usize index = 0; index < count; ++index)
        {
            const std::span<const u8> slice =
                message.subspan(index * MaxReliableFragmentSize,
                                std::min(MaxReliableFragmentSize,
                                         message.size() - (index * MaxReliableFragmentSize)));
            s.ReliableOutbox.push_back(State::OutgoingMessage{
                .Id = s.NextMessageId,
                .Bytes = vector<u8>(slice.begin(), slice.end()),
                .FragmentCount = static_cast<u8>(count),
                .FragmentIndex = static_cast<u8>(index),
            });
            s.NextMessageId = static_cast<u16>(s.NextMessageId + 1);
        }
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
