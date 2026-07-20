#pragma once

#include <Veng/Reflection/Reflect.h>
#include <Veng/Veng.h>

#include <functional>

// Veng/Net/AccountId.h — the opaque persistent 128-bit account id naming who a connection is.
//
// An AccountId names a *player* where a ConnectionId names a transport link: the connection dies
// with the socket, the account survives it, so seats, authorization, directory membership, and
// durable records key on the account. The engine never interprets the bits — a consumer packs a
// config identity, a machine-derived id, or an auth-token subject (the WorldKey discipline applied
// to identity). It is supplied per connection by the consumer's GameNetInfo::Identity hook, admitted
// by the server's AdmitAccount hook, and carried once in the connect request — a cold message, never
// the hot path. Pure value type — compiles under include_hygiene.

namespace Veng::Net
{
    /// @brief An opaque persistent 128-bit account id the consumer mints and the engine never interprets.
    ///
    /// The engine treats it as an uninterpreted pair of 64-bit halves with value semantics — it only
    /// compares and hashes them. The zero value is the invalid id (no account); every admitted
    /// connection carries a valid one.
    ///
    /// @warning There is no authentication behind it: whoever presents an account id *is* that
    ///          account for everything keyed on it, so an account id is a capability token once
    ///          anything durable keys on it. Hosting beyond a trusted LAN is unsafe until a consumer
    ///          verifies identity in its AdmitAccount hook.
    struct AccountId
    {
        /// @brief Low 64 bits of the id.
        u64 Lo = 0;
        /// @brief High 64 bits of the id.
        u64 Hi = 0;

        /// @brief Equality over both halves.
        [[nodiscard]] constexpr bool operator==(const AccountId&) const = default;

        /// @brief Returns whether the id names an account (any nonzero bit); zero is "no account".
        [[nodiscard]] constexpr bool IsValid() const { return Lo != 0 || Hi != 0; }
    };

    /// @brief Mints a random, valid account id — the ephemeral per-process default.
    ///
    /// The zero-config posture when no Identity hook is set: the id is valid but derives from
    /// nothing durable, so reattach and persistence key on nothing across relaunches. A consumer
    /// wanting a stable identity supplies its own id through GameNetInfo::Identity instead.
    /// @return A random nonzero account id.
    [[nodiscard]] VE_API AccountId GenerateAccountId();
}

namespace std
{
    /// @brief Hash of an AccountId over both 64-bit halves, so it keys unordered containers.
    template <>
    struct hash<Veng::Net::AccountId>
    {
        [[nodiscard]] size_t operator()(const Veng::Net::AccountId& id) const noexcept
        {
            const size_t lo = std::hash<Veng::u64>{}(id.Lo);
            const size_t hi = std::hash<Veng::u64>{}(id.Hi);
            return lo ^ (hi + 0x9E3779B97F4A7C15ULL + (lo << 6) + (lo >> 2));
        }
    };
}

// Reflected as a plain two-u64 struct so it sits in reflected components (SeatAccount, a game's own
// identity-keyed components) as ordinary data; the halves stay opaque to every consumer of the schema.
VE_REFLECT(::Veng::Net::AccountId, 0xF1FB3C95B8B810F7ULL)
VE_FIELD(Lo, .DisplayName = "Account Lo")
VE_FIELD(Hi, .DisplayName = "Account Hi")
VE_REFLECT_END();
