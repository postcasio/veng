#pragma once

#include <Veng/Reflection/Reflect.h>
#include <Veng/Reflection/ReflectionTypes.h>
#include <Veng/Veng.h>

// Veng/Net/Blob.h — the opaque reflected byte blob the engine carries but never interprets.
//
// A Blob is the one shape for every consumer value the engine moves without reading: travel
// params on a world request, game messages on a channel. A consumer serialises its own value into
// the bytes (the reflection-binary walker is the shared codec) and deserialises it on the other
// side. Type names the reflected type the bytes encode (or InvalidTypeId for none) so a consumer's
// placement policy, digest fold, or message handler can tell one shape from another without the
// engine interpreting the bytes; the engine only ever moves the pair. Empty is valid — many
// requests carry no payload. Pure value type — compiles under include_hygiene.

namespace Veng::Net
{
    /// @brief Opaque bytes a consumer attaches to a request or message; the engine carries but never reads them.
    ///
    /// A game serialises its own value into the bytes and deserialises it where the blob lands. The
    /// engine treats it as an uninterpreted blob with value semantics; an empty blob (no bytes) is
    /// the common no-data case. Type carries the reflected type id the bytes encode so a consumer
    /// can discriminate blob shapes (a proximity-match placement comparing drop positions, a digest
    /// fold, a channel handler decoding a message) without the engine decoding anything.
    struct Blob
    {
        /// @brief The reflected type the bytes encode, or InvalidTypeId when the blob carries none.
        TypeId Type = InvalidTypeId;
        /// @brief The opaque blob bytes; empty when the value carries no data.
        vector<u8> Bytes;

        /// @brief Returns whether the blob carries no bytes.
        [[nodiscard]] bool IsEmpty() const { return Bytes.empty(); }

        /// @brief Value equality over the type id and the byte contents.
        [[nodiscard]] bool operator==(const Blob&) const = default;
    };

    /// @brief Alias for Blob — the name the travel/join surfaces spell the payload with.
    using TravelPayload = Blob;
}

// Reflected as an opaque (type id, bytes) pair so it sits inside reflected values (a session
// record's params and pose) as ordinary data; the bytes stay uninterpreted by every consumer of
// the schema.
VE_REFLECT(::Veng::Net::Blob, 0x686E1ADA700DAE37ULL)
VE_FIELD(Type, .DisplayName = "Type")
VE_ARRAY_FIELD(Bytes, .DisplayName = "Bytes")
VE_REFLECT_END();
