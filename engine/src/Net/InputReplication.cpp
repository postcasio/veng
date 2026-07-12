#include <Veng/Net/Replication.h>

#include <Veng/Net/BitStream.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeRegistry.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace Veng
{
    namespace
    {
        // Framing is written field-by-field little-endian (never a memcpy of a padded struct); the
        // ActionState payloads between the framing are the reflection serializer's WriteFields bytes.

        void AppendU32(vector<u8>& out, u32 value)
        {
            for (u32 i = 0; i < 4; ++i)
            {
                out.push_back(static_cast<u8>(value >> (8 * i)));
            }
        }

        void AppendU64(vector<u8>& out, u64 value)
        {
            for (u32 i = 0; i < 8; ++i)
            {
                out.push_back(static_cast<u8>(value >> (8 * i)));
            }
        }

        Result<u32> ReadU32(std::span<const u8> in, usize& cursor)
        {
            if (cursor + sizeof(u32) > in.size())
            {
                return std::unexpected("input packet: truncated u32");
            }
            u32 value = 0;
            for (u32 i = 0; i < 4; ++i)
            {
                value |= static_cast<u32>(in[cursor + i]) << (8 * i);
            }
            cursor += sizeof(u32);
            return value;
        }

        Result<u64> ReadU64(std::span<const u8> in, usize& cursor)
        {
            if (cursor + sizeof(u64) > in.size())
            {
                return std::unexpected("input packet: truncated u64");
            }
            u64 value = 0;
            for (u32 i = 0; i < 8; ++i)
            {
                value |= static_cast<u64>(in[cursor + i]) << (8 * i);
            }
            cursor += sizeof(u64);
            return value;
        }

        const TypeInfo& ActionStateInfo(const TypeRegistry& registry)
        {
            return registry.Info(TypeIdOf<ActionState>());
        }
    }

    namespace
    {
        // An axis component in [-1, 1] to 8 bits and back — the packed-input value quantization.
        u32 QuantizeAxis8(f32 value)
        {
            const f32 normalized = std::clamp((value + 1.0f) * 0.5f, 0.0f, 1.0f);
            return static_cast<u32>(std::lround(normalized * 255.0f));
        }

        f32 DequantizeAxis8(u32 code)
        {
            return static_cast<f32>(code) / 255.0f * 2.0f - 1.0f;
        }
    }

    u64 HashContextStack(std::span<const AssetId> contexts)
    {
        // FNV-1a over the context ids in order — order-sensitive so a reordered stack differs.
        u64 hash = 1469598103934665603ULL;
        const auto mix = [&hash](u64 value)
        {
            for (u32 i = 0; i < 8; ++i)
            {
                hash ^= (value >> (8 * i)) & 0xFFu;
                hash *= 1099511628211ULL;
            }
        };
        for (const AssetId id : contexts)
        {
            mix(id.Value);
        }
        return hash;
    }

    vector<u8> EncodePackedActionState(const ActionState& state,
                                       std::span<const PackedInputAction> schema)
    {
        Net::BitWriter bw;
        for (const PackedInputAction& action : schema)
        {
            const ActionSample* sample = nullptr;
            for (const ActionSample& candidate : state.Actions)
            {
                if (candidate.Id == action.Id)
                {
                    sample = &candidate;
                    break;
                }
            }
            bw.WriteBit(sample != nullptr);
            if (sample == nullptr)
            {
                continue;
            }
            bw.WriteBits(static_cast<u32>(sample->Phase), 2);
            switch (action.Kind)
            {
            case ActionKind::Button:
                bw.WriteBit(sample->Value.x != 0.0f);
                break;
            case ActionKind::Axis1D:
                bw.WriteBits(QuantizeAxis8(sample->Value.x), 8);
                break;
            case ActionKind::Axis2D:
                bw.WriteBits(QuantizeAxis8(sample->Value.x), 8);
                bw.WriteBits(QuantizeAxis8(sample->Value.y), 8);
                break;
            }
        }
        return bw.Take();
    }

    ActionState DecodePackedActionState(std::span<const u8> bytes,
                                        std::span<const PackedInputAction> schema)
    {
        ActionState state;
        Net::BitReader br(bytes);
        for (const PackedInputAction& action : schema)
        {
            if (!br.ReadBit())
            {
                continue;
            }
            ActionSample sample;
            sample.Id = action.Id;
            sample.Phase = static_cast<ActionPhase>(br.ReadBits(2));
            switch (action.Kind)
            {
            case ActionKind::Button:
                sample.Value = vec2(br.ReadBit() ? 1.0f : 0.0f, 0.0f);
                break;
            case ActionKind::Axis1D:
                sample.Value = vec2(DequantizeAxis8(br.ReadBits(8)), 0.0f);
                break;
            case ActionKind::Axis2D:
                sample.Value.x = DequantizeAxis8(br.ReadBits(8));
                sample.Value.y = DequantizeAxis8(br.ReadBits(8));
                break;
            }
            state.Actions.push_back(sample);
        }
        return state;
    }

    vector<u8> EncodePackedInputPacket(u64 ackedServerTick, u64 contextHash, u64 firstClientTick,
                                       std::span<const ActionState> records,
                                       std::span<const PackedInputAction> schema)
    {
        vector<u8> out;
        AppendU64(out, ackedServerTick);
        AppendU64(out, contextHash);
        AppendU64(out, firstClientTick);
        AppendU32(out, static_cast<u32>(records.size()));
        for (const ActionState& state : records)
        {
            const vector<u8> packed = EncodePackedActionState(state, schema);
            AppendU32(out, static_cast<u32>(packed.size()));
            out.insert(out.end(), packed.begin(), packed.end());
        }
        return out;
    }

    Result<InputPacket> DecodePackedInputPacket(std::span<const u8> packet, u64 expectedHash,
                                                std::span<const PackedInputAction> schema)
    {
        usize cursor = 0;
        const Result<u64> ackedServerTick = ReadU64(packet, cursor);
        const Result<u64> contextHash = ReadU64(packet, cursor);
        const Result<u64> firstClientTick = ReadU64(packet, cursor);
        const Result<u32> count = ReadU32(packet, cursor);
        if (!ackedServerTick || !contextHash || !firstClientTick || !count)
        {
            return std::unexpected("packed input packet: truncated header");
        }
        if (*contextHash != expectedHash)
        {
            return std::unexpected("packed input packet: context hash mismatch");
        }

        InputPacket result;
        result.AckedServerTick = *ackedServerTick;
        for (u32 i = 0; i < *count; ++i)
        {
            const Result<u32> byteLength = ReadU32(packet, cursor);
            if (!byteLength || cursor + *byteLength > packet.size())
            {
                break; // truncated trailing record
            }
            const std::span<const u8> payload = packet.subspan(cursor, *byteLength);
            cursor += *byteLength;
            result.Inputs.push_back(TickedInput{.ClientTick = *firstClientTick + i,
                                                .State = DecodePackedActionState(payload, schema)});
        }
        return result;
    }

    ActionState DecayInputPhases(const ActionState& state)
    {
        ActionState decayed = state;
        for (ActionSample& sample : decayed.Actions)
        {
            if (sample.Phase == ActionPhase::Started)
            {
                sample.Phase = ActionPhase::Ongoing;
            }
            else if (sample.Phase == ActionPhase::Completed)
            {
                sample.Phase = ActionPhase::None;
            }
        }
        return decayed;
    }

    vector<u8> EncodeInputPacket(u64 ackedServerTick, u64 firstClientTick,
                                 std::span<const ActionState> records, const TypeRegistry& registry)
    {
        const TypeInfo& info = ActionStateInfo(registry);

        vector<u8> out;
        AppendU64(out, ackedServerTick);
        AppendU64(out, firstClientTick);
        AppendU32(out, static_cast<u32>(records.size()));

        for (const ActionState& state : records)
        {
            vector<u8> payload;
            WriteFields(payload, &state, info, registry);
            AppendU32(out, static_cast<u32>(payload.size()));
            out.insert(out.end(), payload.begin(), payload.end());
        }

        return out;
    }

    Result<InputPacket> DecodeInputPacket(std::span<const u8> packet, const TypeRegistry& registry)
    {
        usize cursor = 0;
        const Result<u64> ackedServerTick = ReadU64(packet, cursor);
        if (!ackedServerTick)
        {
            return std::unexpected("input packet: truncated header");
        }
        const Result<u64> firstClientTick = ReadU64(packet, cursor);
        if (!firstClientTick)
        {
            return std::unexpected("input packet: truncated header");
        }
        const Result<u32> count = ReadU32(packet, cursor);
        if (!count)
        {
            return std::unexpected("input packet: truncated header");
        }

        InputPacket result;
        result.AckedServerTick = *ackedServerTick;

        const TypeInfo& info = ActionStateInfo(registry);
        for (u32 i = 0; i < *count; ++i)
        {
            const Result<u32> byteLength = ReadU32(packet, cursor);
            if (!byteLength)
            {
                break; // truncated trailing record
            }
            if (cursor + *byteLength > packet.size())
            {
                break; // record claims more bytes than the packet holds
            }
            const std::span<const u8> payload = packet.subspan(cursor, *byteLength);
            cursor += *byteLength;

            ActionState state;
            if (VoidResult read = ReadFields(payload, &state, info, registry); !read)
            {
                continue; // malformed record drops; its tick is skipped
            }
            result.Inputs.push_back(
                TickedInput{.ClientTick = *firstClientTick + i, .State = std::move(state)});
        }

        return result;
    }

    void InputSendBuffer::Stamp(u64 clientTick, const ActionState& state)
    {
        m_Window.push_back(TickedInput{.ClientTick = clientTick, .State = state});
        if (m_Window.size() > m_Settings.Redundancy)
        {
            m_Window.erase(m_Window.begin(),
                           m_Window.begin() + static_cast<vector<TickedInput>::difference_type>(
                                                  m_Window.size() - m_Settings.Redundancy));
        }
    }

    vector<u8> InputSendBuffer::Encode(u64 ackedServerTick, const TypeRegistry& registry) const
    {
        if (m_Window.empty())
        {
            return EncodeInputPacket(ackedServerTick, 0, {}, registry);
        }

        vector<ActionState> states;
        states.reserve(m_Window.size());
        for (const TickedInput& input : m_Window)
        {
            states.push_back(input.State);
        }
        return EncodeInputPacket(ackedServerTick, m_Window.front().ClientTick, states, registry);
    }

    void InputJitterBuffer::Ingest(const InputPacket& packet)
    {
        for (const TickedInput& input : packet.Inputs)
        {
            if (m_Started && input.ClientTick <= m_LastConsumedTick)
            {
                continue; // already consumed (or dropped) past this tick
            }
            m_Buffer[input.ClientTick] = input.State; // redundant duplicates collapse latest-wins
        }
    }

    optional<ActionState> InputJitterBuffer::Consume()
    {
        ++m_ConsumeCount;

        // Overrun: drop the oldest buffered ticks so at most TargetDepth remain after this consume,
        // bounding the latency the buffer holds. A dropped tick advances the consumed front, so a
        // later redundant copy of it is rejected on Ingest.
        while (m_Buffer.size() > static_cast<usize>(m_Settings.TargetDepth) + 1)
        {
            m_LastConsumedTick = m_Buffer.begin()->first;
            m_Started = true;
            m_Buffer.erase(m_Buffer.begin());
        }

        if (!m_Buffer.empty())
        {
            auto oldest = m_Buffer.extract(m_Buffer.begin());
            m_LastConsumedTick = oldest.key();
            m_Last = oldest.mapped();
            m_Started = true;
            return std::move(oldest.mapped());
        }

        // Underrun: coast on the last input with edge phases decayed (a held action persists, an edge
        // never repeats). nullopt only before the first input has ever arrived.
        if (m_Started)
        {
            ++m_UnderrunCount;
            m_Last = DecayInputPhases(*m_Last);
            return m_Last;
        }
        return std::nullopt;
    }

    optional<ActionState> InputJitterBuffer::ConsumeForTick(u64 tick)
    {
        ++m_ConsumeCount;

        // Drop any buffered tick older than the scheduled one: the server has advanced past it, so it
        // can never be consumed (a later redundant copy is rejected on Ingest).
        while (!m_Buffer.empty() && m_Buffer.begin()->first < tick)
        {
            m_LastConsumedTick = m_Buffer.begin()->first;
            m_Started = true;
            m_Buffer.erase(m_Buffer.begin());
        }

        if (const auto it = m_Buffer.find(tick); it != m_Buffer.end())
        {
            auto node = m_Buffer.extract(it);
            m_LastConsumedTick = tick;
            m_Last = node.mapped();
            m_Started = true;
            return std::move(node.mapped());
        }

        // Underrun: the input for this tick has not arrived (the client is not far enough ahead, or the
        // packet was lost). Coast on the last input with edge phases decayed, exactly as Consume does.
        if (m_Started)
        {
            ++m_UnderrunCount;
            m_LastConsumedTick = tick;
            m_Last = DecayInputPhases(*m_Last);
            return m_Last;
        }
        return std::nullopt;
    }
}
