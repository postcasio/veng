#pragma once

#include <Veng/Reflection/Reflect.h>
#include <Veng/Veng.h>

#include <functional>

// Veng/Net/WorldKey.h — the opaque 128-bit value a consumer presents to request a world.
//
// A WorldKey names a world *by intent or content*: the engine never interprets it — it holds a
// UUID, a content hash, a packed seed+coordinate, or a zero-extended integer, whatever the game
// chooses. It is what a client presents to join a world and the key the server's get-or-create map
// is keyed on, so two connections presenting the same key converge on one shared instance. 128 bits
// so no consumer must hash a wider natural id down and risk collisions; it costs nothing on the hot
// path because that carries the small JoinId, not the key — the WorldKey rides only the join
// request, a cold control message. Pure value type — compiles under include_hygiene.

namespace Veng::Net
{
    /// @brief An opaque 128-bit world name a consumer defines and the server resolves get-or-create.
    ///
    /// The engine treats it as an uninterpreted pair of 64-bit halves with value semantics; a
    /// consumer packs whatever identity it wants into the two words. Equality is over both halves,
    /// so it keys the server's shared WorldKey → WorldInstanceId map and the client's join set.
    struct WorldKey
    {
        /// @brief Low 64 bits of the key.
        u64 Lo = 0;
        /// @brief High 64 bits of the key.
        u64 Hi = 0;

        /// @brief Equality over both halves.
        [[nodiscard]] bool operator==(const WorldKey&) const = default;

        /// @brief Builds a key from a natural 64-bit id, zero-extending the high half.
        /// @param value  The low 64 bits; the high half is zero.
        /// @return The zero-extended key.
        [[nodiscard]] static WorldKey FromU64(const u64 value) { return WorldKey{.Lo = value}; }
    };

    /// @brief The well-known key a single-world session joins by default.
    ///
    /// A fixed shared world every convenience-path client converges on: the one hosted world of a
    /// single-world server. A game instancing worlds by content picks its own content-derived keys
    /// instead; this is the zero-config default that keeps the one-world path a single line.
    inline constexpr WorldKey DefaultWorldKey = WorldKey{.Lo = 1};
}

// Reflected as a plain two-u64 struct so it sits in reflected values (a session record's standing
// joins, a game's own world-naming data) as ordinary data; the halves stay opaque to every consumer.
VE_REFLECT(::Veng::Net::WorldKey, 0x4F7EB06F2D29EE40ULL)
VE_FIELD(Lo, .DisplayName = "Key Lo")
VE_FIELD(Hi, .DisplayName = "Key Hi")
VE_REFLECT_END();

namespace std
{
    /// @brief Hash of a WorldKey over both 64-bit halves, so it keys unordered containers.
    template <>
    struct hash<Veng::Net::WorldKey>
    {
        [[nodiscard]] size_t operator()(const Veng::Net::WorldKey& key) const noexcept
        {
            const size_t lo = std::hash<Veng::u64>{}(key.Lo);
            const size_t hi = std::hash<Veng::u64>{}(key.Hi);
            return lo ^ (hi + 0x9E3779B97F4A7C15ULL + (lo << 6) + (lo >> 2));
        }
    };
}
