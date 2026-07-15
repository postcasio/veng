#pragma once

#include <Veng/Veng.h>

// Veng/Net/TravelPayload.h — the opaque per-travel data a consumer attaches to a world request.
//
// A TravelPayload is uninterpreted bytes: the engine carries it from a request into the world it
// opens or joins and never reads it. A game packs whatever it wants (an arrival pose, a drop
// position, a spawn parameter) and unpacks it on the other side. Empty is valid — most requests
// carry no payload. Pure value type — compiles under include_hygiene.

namespace Veng::Net
{
    /// @brief Opaque bytes a consumer attaches to a world request; the engine carries but never reads them.
    ///
    /// A game serialises its own arrival data into the bytes and deserialises it where the travel
    /// lands. The engine treats it as an uninterpreted blob with value semantics; an empty payload
    /// (no bytes) is the common no-data case.
    struct TravelPayload
    {
        /// @brief The opaque payload bytes; empty when the request carries no data.
        vector<u8> Bytes;

        /// @brief Returns whether the payload carries no bytes.
        [[nodiscard]] bool IsEmpty() const { return Bytes.empty(); }

        /// @brief Value equality over the byte contents.
        [[nodiscard]] bool operator==(const TravelPayload&) const = default;
    };
}
