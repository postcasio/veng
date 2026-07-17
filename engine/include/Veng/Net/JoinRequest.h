#pragma once

#include <Veng/Net/AccountId.h>
#include <Veng/Net/NetEvents.h>
#include <Veng/Net/Blob.h>
#include <Veng/Net/WorldKey.h>
#include <Veng/Veng.h>

// Veng/Net/JoinRequest.h — the identity of one world-join request, as policy hooks see it.
//
// Every player-keyed join decision — the Authorize gate, the Placement policy — reads the same
// four facts: which transport link is asking (Connection), who that link is (Account), which world
// it names (Key), and the opaque arrival data it carries (Payload). Carrying them as one struct
// keeps the hook signatures stable as the vocabulary grows. Pure value/view type — compiles under
// include_hygiene.

namespace Veng::Net
{
    /// @brief One world-join request's identity: the connection, its account, the key, the payload.
    ///
    /// Handed to the consumer's Authorize and Placement hooks. A connection-borne request carries
    /// the connection's admitted account — admission precedes authorization, so it is always valid
    /// there. A standalone (transport-less) resolve carries ConnectionId{} with the local player's
    /// account, invalid only on a headless dedicated host, which resolves no local account.
    struct JoinRequestInfo
    {
        /// @brief The requesting connection, or ConnectionId{} for a standalone (local) resolve.
        ConnectionId Connection = ServerConnectionId;
        /// @brief The requester's account (see the struct doc for when it is valid).
        AccountId Account;
        /// @brief The opaque world the request names (the engine never interprets it).
        WorldKey Key;
        /// @brief The opaque travel payload the request carries; borrowed for the hook call.
        const Blob& Payload;
    };
}
