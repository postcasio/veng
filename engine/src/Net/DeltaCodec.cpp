#include <Veng/Net/DeltaCodec.h>

#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeRegistry.h>

#include <bit>
#include <cstring>
#include <new>
#include <string_view>
#include <unordered_map>

namespace Veng::Net
{
    namespace
    {
        void AppendU32(vector<u8>& out, u32 value)
        {
            for (u32 i = 0; i < 4; ++i)
            {
                out.push_back(static_cast<u8>(value >> (8 * i)));
            }
        }

        bool ReadU32(std::span<const u8> in, usize& cursor, u32& out)
        {
            if (cursor + sizeof(u32) > in.size())
            {
                return false;
            }
            out = 0;
            for (u32 i = 0; i < 4; ++i)
            {
                out |= static_cast<u32>(in[cursor + i]) << (8 * i);
            }
            cursor += sizeof(u32);
            return true;
        }

        // Aligned scratch for one type-erased value (the Replication.cpp idiom), so a value is
        // decoded out of line and never touches live state until the caller applies it.
        struct ScratchValue
        {
            const TypeInfo& Info;
            void* Ptr;

            explicit ScratchValue(const TypeInfo& info)
                : Info(info), Ptr(::operator new(info.Size, std::align_val_t{info.Align}))
            {
                Info.DefaultConstruct(Ptr);
            }

            ~ScratchValue()
            {
                Info.Destruct(Ptr);
                ::operator delete(Ptr, std::align_val_t{Info.Align});
            }

            ScratchValue(const ScratchValue&) = delete;
            ScratchValue& operator=(const ScratchValue&) = delete;
        };

        // One field record parsed out of a WriteFields byte stream, as name-keyed value spans in the
        // stream's (descriptor) order. WriteFields writes RecordCount then per field NameLen/Name/
        // ValueLen/Value, so a delta can diff and reassemble value runs without decoding to a value.
        struct ParsedFields
        {
            std::unordered_map<std::string_view, std::span<const u8>> ByName;
            bool Ok = false;
        };

        ParsedFields ParseFields(std::span<const u8> bytes)
        {
            ParsedFields out;
            usize cursor = 0;
            u32 count = 0;
            if (!ReadU32(bytes, cursor, count))
            {
                return out;
            }
            for (u32 i = 0; i < count; ++i)
            {
                u32 nameLen = 0;
                if (!ReadU32(bytes, cursor, nameLen) || cursor + nameLen > bytes.size())
                {
                    return out;
                }
                const std::string_view name(reinterpret_cast<const char*>(bytes.data() + cursor),
                                            nameLen);
                cursor += nameLen;
                u32 valueLen = 0;
                if (!ReadU32(bytes, cursor, valueLen) || cursor + valueLen > bytes.size())
                {
                    return out;
                }
                out.ByName.emplace(name, bytes.subspan(cursor, valueLen));
                cursor += valueLen;
            }
            out.Ok = true;
            return out;
        }

        // Appends one field record (NameLen/Name/ValueLen/Value) — the WriteFields per-field framing.
        void AppendFieldRecord(vector<u8>& out, std::string_view name, std::span<const u8> value)
        {
            AppendU32(out, static_cast<u32>(name.size()));
            out.insert(out.end(), name.begin(), name.end());
            AppendU32(out, static_cast<u32>(value.size()));
            out.insert(out.end(), value.begin(), value.end());
        }

        // ---- Transform quantized encoding -------------------------------------------------------

        struct TransformLeaves
        {
            const FieldDescriptor* Position = nullptr;
            const FieldDescriptor* Rotation = nullptr;
            const FieldDescriptor* Scale = nullptr;
        };

        TransformLeaves LocateTransformLeaves(const TypeInfo& info)
        {
            TransformLeaves leaves;
            for (const FieldDescriptor& field : info.Fields)
            {
                if (field.Name == "Position")
                {
                    leaves.Position = &field;
                }
                else if (field.Name == "Rotation")
                {
                    leaves.Rotation = &field;
                }
                else if (field.Name == "Scale")
                {
                    leaves.Scale = &field;
                }
            }
            return leaves;
        }

        vec3 ReadVec3(const void* base, const FieldDescriptor& field)
        {
            vec3 value{0.0f};
            std::memcpy(&value, static_cast<const u8*>(base) + field.Offset, sizeof(vec3));
            return value;
        }

        quat ReadQuat(const void* base, const FieldDescriptor& field)
        {
            quat value{1.0f, 0.0f, 0.0f, 0.0f};
            std::memcpy(&value, static_cast<const u8*>(base) + field.Offset, sizeof(quat));
            return value;
        }

        void WriteVec3(void* base, const FieldDescriptor& field, const vec3& value)
        {
            std::memcpy(static_cast<u8*>(base) + field.Offset, &value, sizeof(vec3));
        }

        void WriteQuat(void* base, const FieldDescriptor& field, const quat& value)
        {
            std::memcpy(static_cast<u8*>(base) + field.Offset, &value, sizeof(quat));
        }

        vector<u8> EncodedPositionBytes(const vec3& value, const QuantizationSettings& quant)
        {
            BitWriter bw;
            EncodePosition(bw, value, quant);
            return bw.Take();
        }

        vector<u8> EncodedRotationBytes(const quat& value, const QuantizationSettings& quant)
        {
            BitWriter bw;
            EncodeRotation(bw, value, quant);
            return bw.Take();
        }

        void EncodeTransformBody(vector<u8>& out, std::span<const u8> currentBytes,
                                 std::span<const u8> baselineBytes, bool full, const TypeInfo& info,
                                 const TypeRegistry& registry, const QuantizationSettings& quant)
        {
            const TransformLeaves leaves = LocateTransformLeaves(info);

            ScratchValue current(info);
            (void)ReadFields(currentBytes, current.Ptr, info, registry);

            const vec3 curPos =
                leaves.Position ? ReadVec3(current.Ptr, *leaves.Position) : vec3(0.0f);
            const quat curRot =
                leaves.Rotation ? ReadQuat(current.Ptr, *leaves.Rotation) : quat(1, 0, 0, 0);
            const vec3 curScale = leaves.Scale ? ReadVec3(current.Ptr, *leaves.Scale) : vec3(1.0f);

            bool posPresent = true;
            bool rotPresent = true;
            bool scalePresent = true;
            if (!full && !baselineBytes.empty())
            {
                ScratchValue base(info);
                (void)ReadFields(baselineBytes, base.Ptr, info, registry);
                const vec3 basePos =
                    leaves.Position ? ReadVec3(base.Ptr, *leaves.Position) : vec3(0.0f);
                const quat baseRot =
                    leaves.Rotation ? ReadQuat(base.Ptr, *leaves.Rotation) : quat(1, 0, 0, 0);
                const vec3 baseScale =
                    leaves.Scale ? ReadVec3(base.Ptr, *leaves.Scale) : vec3(1.0f);

                posPresent =
                    EncodedPositionBytes(curPos, quant) != EncodedPositionBytes(basePos, quant);
                rotPresent =
                    EncodedRotationBytes(curRot, quant) != EncodedRotationBytes(baseRot, quant);
                scalePresent = curScale != baseScale;
            }

            out.push_back(static_cast<u8>(ComponentEncoding::TransformQuant));

            BitWriter bw;
            bw.WriteBit(posPresent);
            bw.WriteBit(rotPresent);
            bw.WriteBit(scalePresent);
            if (posPresent)
            {
                EncodePosition(bw, curPos, quant);
            }
            if (rotPresent)
            {
                EncodeRotation(bw, curRot, quant);
            }
            if (scalePresent)
            {
                for (int axis = 0; axis < 3; ++axis)
                {
                    bw.WriteBits(std::bit_cast<u32>(curScale[axis]), 32);
                }
            }
            const vector<u8> body = bw.Take();
            out.insert(out.end(), body.begin(), body.end());
        }

        VoidResult DecodeTransformBody(std::span<const u8> payload,
                                       std::span<const u8> baselineBytes, const TypeInfo& info,
                                       const TypeRegistry& registry,
                                       const QuantizationSettings& quant, vector<u8>& outBytes)
        {
            const TransformLeaves leaves = LocateTransformLeaves(info);

            ScratchValue result(info);
            if (!baselineBytes.empty())
            {
                (void)ReadFields(baselineBytes, result.Ptr, info, registry);
            }

            BitReader br(payload);
            const bool posPresent = br.ReadBit();
            const bool rotPresent = br.ReadBit();
            const bool scalePresent = br.ReadBit();

            if (posPresent && leaves.Position)
            {
                WriteVec3(result.Ptr, *leaves.Position, DecodePosition(br, quant));
            }
            if (rotPresent && leaves.Rotation)
            {
                WriteQuat(result.Ptr, *leaves.Rotation, DecodeRotation(br, quant));
            }
            if (scalePresent && leaves.Scale)
            {
                vec3 scale{1.0f};
                for (int axis = 0; axis < 3; ++axis)
                {
                    scale[axis] = std::bit_cast<f32>(br.ReadBits(32));
                }
                WriteVec3(result.Ptr, *leaves.Scale, scale);
            }

            outBytes.clear();
            WriteFields(outBytes, result.Ptr, info, registry);
            return {};
        }
    }

    void EncodeComponentBody(vector<u8>& out, TypeId type, std::span<const u8> currentBytes,
                             std::span<const u8> baselineBytes, bool forceFull,
                             TypeId transformType, const TypeRegistry& registry,
                             const QuantizationSettings& quant)
    {
        const TypeInfo& info = registry.Info(type);
        const bool full = forceFull || baselineBytes.empty();

        if (transformType != InvalidTypeId && type == transformType)
        {
            EncodeTransformBody(out, currentBytes, baselineBytes, full, info, registry, quant);
            return;
        }

        if (full)
        {
            out.push_back(static_cast<u8>(ComponentEncoding::ReflectFull));
            out.insert(out.end(), currentBytes.begin(), currentBytes.end());
            return;
        }

        const ParsedFields current = ParseFields(currentBytes);
        const ParsedFields baseline = ParseFields(baselineBytes);
        if (!current.Ok || !baseline.Ok)
        {
            // A baseline we cannot parse is no baseline: fall back to the full self-describing form.
            out.push_back(static_cast<u8>(ComponentEncoding::ReflectFull));
            out.insert(out.end(), currentBytes.begin(), currentBytes.end());
            return;
        }

        const usize fieldCount = info.Fields.size();
        const usize maskBytes = (fieldCount + 7) / 8;

        vector<u8> mask(maskBytes, 0);
        vector<u8> values;
        for (usize i = 0; i < fieldCount; ++i)
        {
            const std::string_view name = info.Fields[i].Name;
            const auto curIt = current.ByName.find(name);
            const auto baseIt = baseline.ByName.find(name);
            const bool changed =
                curIt == current.ByName.end() || baseIt == baseline.ByName.end() ||
                curIt->second.size() != baseIt->second.size() ||
                std::memcmp(curIt->second.data(), baseIt->second.data(), curIt->second.size()) != 0;
            if (changed && curIt != current.ByName.end())
            {
                mask[i / 8] |= static_cast<u8>(1u << (i % 8));
                AppendU32(values, static_cast<u32>(curIt->second.size()));
                values.insert(values.end(), curIt->second.begin(), curIt->second.end());
            }
        }

        out.push_back(static_cast<u8>(ComponentEncoding::ReflectDelta));
        AppendU32(out, static_cast<u32>(fieldCount));
        out.insert(out.end(), mask.begin(), mask.end());
        out.insert(out.end(), values.begin(), values.end());
    }

    VoidResult DecodeComponentBody(std::span<const u8> body, std::span<const u8> baselineBytes,
                                   TypeId type, TypeId transformType, const TypeRegistry& registry,
                                   const QuantizationSettings& quant, vector<u8>& outBytes)
    {
        if (body.empty())
        {
            return std::unexpected("delta: empty component body");
        }
        const auto encoding = static_cast<ComponentEncoding>(body[0]);
        const std::span<const u8> payload = body.subspan(1);
        const TypeInfo& info = registry.Info(type);

        switch (encoding)
        {
        case ComponentEncoding::ReflectFull:
            outBytes.assign(payload.begin(), payload.end());
            return {};

        case ComponentEncoding::TransformQuant:
            if (transformType == InvalidTypeId || type != transformType)
            {
                return std::unexpected("delta: transform body for a non-transform type");
            }
            return DecodeTransformBody(payload, baselineBytes, info, registry, quant, outBytes);

        case ComponentEncoding::ReflectDelta:
        {
            const ParsedFields baseline = ParseFields(baselineBytes);
            if (!baseline.Ok)
            {
                return std::unexpected("delta: no baseline to patch against");
            }
            usize cursor = 0;
            u32 fieldCount = 0;
            if (!ReadU32(payload, cursor, fieldCount))
            {
                return std::unexpected("delta: truncated field count");
            }
            const usize maskBytes = (fieldCount + 7) / 8;
            if (cursor + maskBytes > payload.size())
            {
                return std::unexpected("delta: truncated field mask");
            }
            const std::span<const u8> mask = payload.subspan(cursor, maskBytes);
            cursor += maskBytes;

            // Reassemble the full WriteFields record: each field takes its delta value when present,
            // else its baseline value. Field order is the descriptor order both ends share.
            outBytes.clear();
            AppendU32(outBytes, static_cast<u32>(info.Fields.size()));
            for (usize i = 0; i < info.Fields.size(); ++i)
            {
                const std::string_view name = info.Fields[i].Name;
                const bool present =
                    i < fieldCount && (mask[i / 8] & static_cast<u8>(1u << (i % 8))) != 0;
                if (present)
                {
                    u32 valueLen = 0;
                    if (!ReadU32(payload, cursor, valueLen) || cursor + valueLen > payload.size())
                    {
                        return std::unexpected("delta: truncated field value");
                    }
                    AppendFieldRecord(outBytes, name, payload.subspan(cursor, valueLen));
                    cursor += valueLen;
                }
                else
                {
                    const auto it = baseline.ByName.find(name);
                    if (it == baseline.ByName.end())
                    {
                        return std::unexpected("delta: field absent from baseline");
                    }
                    AppendFieldRecord(outBytes, name, it->second);
                }
            }
            return {};
        }
        }

        return std::unexpected("delta: unknown component encoding");
    }
}
