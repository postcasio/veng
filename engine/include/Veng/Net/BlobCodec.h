#pragma once

#include <Veng/Net/Blob.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Veng.h>

#include <cstring>
#include <type_traits>

// Veng/Net/BlobCodec.h — the two ways a consumer value becomes Net::Blob bytes.
//
// A Blob is opaque to the engine, so every consumer that puts a payload into one supplies its own
// codec. Two forms cover the field: a fixed-layout form that memcpy's a trivially-copyable struct
// in and out under an explicit tag, and a record form that runs a reflected value through the
// field walkers. Both live here rather than in Blob.h so Blob stays a pure value type with no
// reflection-walker dependency.
//
// The tag is a parameter on the fixed-layout form because a blob's tag is a discriminator, not a
// description of the byte layout: a payload is frequently tagged with a type other than the one it
// carries so a receiver can tell two payloads apart on a shared channel, and a payload meant to
// read as absent carries no tag at all. Hard-wiring TypeIdOf<T>() would exclude every wire struct
// that is not itself reflected and silently rewrite the discriminator wherever a consumer
// cross-tags.

namespace Veng::Net
{
    /// @brief Packs a trivially-copyable value into a blob's bytes under an explicit tag.
    ///
    /// The blob carries exactly sizeof(T) bytes — T's object representation, byte for byte — and
    /// the tag the caller names. The tag is a discriminator the receiver matches on, not a
    /// description of the layout: pass TypeIdOf\<T\>() for the plain case, another type's id to
    /// discriminate two payloads on a shared channel, or InvalidTypeId for a payload a permissive
    /// receiver reads as carrying nothing.
    ///
    /// The fixed-layout form is a same-build convenience, not a versioned wire contract: it
    /// encodes T's in-memory layout, which differs across compilers, architectures, and any edit
    /// to T. A payload that crosses a build boundary, or whose fields will evolve, belongs on
    /// EncodeBlobRecord instead.
    /// @tparam T     The payload type; must be trivially copyable.
    /// @param value  The value to pack.
    /// @param tag    The type id the blob is tagged with; the receiver's DecodeBlob must expect it.
    /// @return A blob carrying sizeof(T) bytes under @p tag.
    template <typename T>
    [[nodiscard]] Blob EncodeBlob(const T& value, const TypeId tag)
    {
        static_assert(std::is_trivially_copyable_v<T>,
                      "EncodeBlob packs T's object representation; T must be trivially copyable");

        Blob blob{.Type = tag};
        blob.Bytes.resize(sizeof(T));
        std::memcpy(blob.Bytes.data(), &value, sizeof(T));
        return blob;
    }

    /// @brief Unpacks a trivially-copyable value from a blob, or nullopt when the blob does not fit T.
    ///
    /// Returns a value only when the blob's tag equals @p expected and its byte count is at least
    /// sizeof(T); a mismatched tag or a short payload yields nullopt. Blobs arrive as untrusted
    /// input, so neither is an assert. The size test is `>=`, not `==`, so a longer payload sharing
    /// the tag decodes its leading sizeof(T) bytes and a shorter one reads as absent — a caller
    /// wanting exactness compares the blob's own byte count.
    ///
    /// @warning A successful decode guarantees only that the tag matched and the byte count
    /// sufficed. Nothing validates the bytes: an adversarial payload of the right size memcpy'd
    /// into a T yields out-of-range enums, bools that are neither 0 nor 1, NaN floats, and indices
    /// past the end of whatever they index. The caller must validate every field whose domain is
    /// narrower than its representation. A payload from a peer that has not been admitted belongs
    /// on DecodeBlobRecord, which goes through the field walkers instead of a raw copy.
    /// @tparam T        The payload type; must be trivially copyable.
    /// @param blob      The blob to read.
    /// @param expected  The tag the payload must carry.
    /// @return The decoded value, or nullopt on a tag mismatch or a payload shorter than T.
    template <typename T>
    [[nodiscard]] optional<T> DecodeBlob(const Blob& blob, const TypeId expected)
    {
        static_assert(std::is_trivially_copyable_v<T>,
                      "DecodeBlob copies T's object representation; T must be trivially copyable");

        if (blob.Type != expected || blob.Bytes.size() < sizeof(T))
        {
            return {};
        }

        T value;
        std::memcpy(&value, blob.Bytes.data(), sizeof(T));
        return value;
    }

    /// @brief Encodes a reflected value into a blob as its WriteFields record, tagged with its own type id.
    ///
    /// The record form is the schema-tolerant one: fields are name-keyed and length-prefixed, so a
    /// decoder skips a field it does not know and defaults one the data omits. That is what a
    /// payload whose fields will evolve, or one crossing a build boundary, needs — the fixed-layout
    /// EncodeBlob encodes T's in-memory layout and tolerates neither.
    /// @tparam T        The payload type; must be reflected.
    /// @param value     The value to encode.
    /// @param registry  Registry holding T's descriptors and its nested field types.
    /// @pre T is registered in @p registry.
    /// @return A blob carrying T's field record, tagged TypeIdOf\<T\>().
    template <typename T>
    [[nodiscard]] Blob EncodeBlobRecord(const T& value, const TypeRegistry& registry)
    {
        Blob blob{.Type = TypeIdOf<T>()};
        WriteFields(blob.Bytes, &value, registry.Info(TypeIdOf<T>()), registry);
        return blob;
    }

    /// @brief Decodes a reflected value from a blob's field record, or nullopt when the blob is not one.
    ///
    /// Returns a value only when the blob is tagged TypeIdOf\<T\>() and its bytes decode as a field
    /// record; a foreign tag, an empty blob, or a truncated record yields nullopt rather than an
    /// assert, since blobs arrive as untrusted input. Decoding is schema-tolerant in both
    /// directions: a record naming a field T no longer has is skipped, and a field of T the record
    /// omits keeps its default.
    /// @tparam T        The payload type; must be reflected.
    /// @param blob      The blob to read.
    /// @param registry  Registry holding T's descriptors and its nested field types.
    /// @pre T is registered in @p registry.
    /// @return The decoded value, or nullopt on a foreign tag, an empty blob, or a bad record.
    template <typename T>
    [[nodiscard]] optional<T> DecodeBlobRecord(const Blob& blob, const TypeRegistry& registry)
    {
        if (blob.Type != TypeIdOf<T>() || blob.IsEmpty())
        {
            return {};
        }

        T value;
        if (!ReadFields(blob.Bytes, &value, registry.Info(TypeIdOf<T>()), registry))
        {
            return {};
        }
        return value;
    }
}
