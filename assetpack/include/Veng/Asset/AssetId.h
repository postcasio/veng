#pragma once

#include <Veng/Asset/Types.h>

#include <compare>
#include <functional>

namespace Veng
{
    // Opaque asset identifier. No derivation rule — the cooker mints random
    // non-zero u64s (or honours ids written in a hand-written pack); the format
    // never interprets them. 0 is the reserved "invalid" id, so a pack must not
    // use it.
    struct AssetId
    {
        u64 Value = 0;

        [[nodiscard]] bool IsValid() const { return Value != 0; }

        auto operator<=>(const AssetId&) const = default;
    };
}

template <>
struct std::hash<Veng::AssetId>
{
    Veng::usize operator()(const Veng::AssetId& id) const noexcept
    {
        return std::hash<Veng::u64>{}(id.Value);
    }
};
