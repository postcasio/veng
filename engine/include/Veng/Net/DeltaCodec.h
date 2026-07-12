#pragma once

#include <Veng/Net/Quantize.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Result.h>
#include <Veng/Veng.h>

#include <span>

// Veng/Net/DeltaCodec.h — the per-component wire body codec behind the snapshot record framing.
//
// A snapshot record is TypeId:u64 ByteLength:u32 <body>; this codec produces and consumes the body.
// The body opens with a one-byte encoding tag, so a connection that shares a baseline with the
// server (via ack) sends only what changed, while a never-acked / re-baselined / fallback record
// carries the whole self-describing value — three encodings, one posture: every compressed form is
// an opt-down from a canonical full form, decodable against information both ends provably share (an
// ack). The Transform type additionally quantizes its spatial leaves. Pure reflection + quantization
// helpers: no Scene, no connection state (the baselines live in the replication layer above).

namespace Veng
{
    class TypeRegistry;

    namespace Net
    {
        /// @brief The one-byte encoding tag opening a component wire body.
        enum class ComponentEncoding : u8
        {
            /// @brief The whole value as self-describing WriteFields bytes — spawn, keyframe, fallback.
            ReflectFull = 0,
            /// @brief A field-presence bitmask + the changed fields' values, patched over a baseline.
            ReflectDelta = 1,
            /// @brief The Transform's quantized spatial leaves with per-leaf presence (full or delta).
            TransformQuant = 2,
        };

        /// @brief Encodes a component wire body, deltaing against a baseline when one is shared.
        ///
        /// Appends a leading ComponentEncoding tag then the payload. With @p baselineBytes empty (a
        /// never-acked entity, a post-respawn re-baseline) or @p forceFull set (the keyframe cadence),
        /// the full self-describing form is written; otherwise only the leaves that differ from the
        /// baseline. The @p transformType id selects the quantized spatial encoding for that one type.
        /// @param out            Destination buffer; the tag + payload are appended.
        /// @param type           The component's reflected type id.
        /// @param currentBytes   The current value as WriteFields bytes (references already remapped).
        /// @param baselineBytes  The baseline value as WriteFields bytes, or empty for the full form.
        /// @param forceFull      When true, the full form is written regardless of the baseline.
        /// @param transformType  The Transform type id (its leaves quantize); pass InvalidTypeId to disable.
        /// @param registry       Registry resolving the type's fields.
        /// @param quant          The spatial quantization settings.
        VE_API void EncodeComponentBody(vector<u8>& out, TypeId type,
                                        std::span<const u8> currentBytes,
                                        std::span<const u8> baselineBytes, bool forceFull,
                                        TypeId transformType, const TypeRegistry& registry,
                                        const QuantizationSettings& quant);

        /// @brief Decodes a component wire body against a baseline into full WriteFields bytes.
        ///
        /// A full body reconstructs the whole value; a delta body patches the changed leaves over
        /// @p baselineBytes (which must be the baseline the encoder deltaed against — empty is a
        /// decode error for a delta body, never a crash). On success @p outBytes receives the decoded
        /// value as WriteFields bytes (still in wire/NetId reference space) — both what the caller
        /// applies to the live component (after reference remap) and its new stored baseline.
        /// @param body           The component body (leading tag + payload).
        /// @param baselineBytes  The baseline WriteFields bytes a delta patches over; empty for full.
        /// @param type           The component's reflected type id.
        /// @param transformType  The Transform type id; pass InvalidTypeId to disable quantization.
        /// @param registry       Registry resolving the type's fields.
        /// @param quant          The spatial quantization settings (must match the encoder's).
        /// @param outBytes       Receives the decoded value's WriteFields bytes (the new baseline).
        /// @return Empty on success; an error string on a malformed body or a delta with no baseline.
        VE_API VoidResult DecodeComponentBody(std::span<const u8> body,
                                              std::span<const u8> baselineBytes, TypeId type,
                                              TypeId transformType, const TypeRegistry& registry,
                                              const QuantizationSettings& quant,
                                              vector<u8>& outBytes);
    }
}
