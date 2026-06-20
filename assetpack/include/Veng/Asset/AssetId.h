#pragma once

#include <Veng/Asset/Types.h>

#include <compare>
#include <functional>

namespace Veng
{
    /// @brief Opaque 64-bit asset identifier.
    ///
    /// The cooker mints random non-zero u64s (or honours ids from a hand-written pack); the
    /// format never interprets them. 0 is the reserved invalid id — a pack must not use it.
    struct AssetId
    {
        /// @brief The raw identifier value; 0 is invalid.
        u64 Value = 0;

        /// @brief Returns true if the id is non-zero (i.e. not the reserved invalid id).
        [[nodiscard]] bool IsValid() const { return Value != 0; }

        /// @brief Three-way comparison, giving AssetId a total order for sorted containers.
        auto operator<=>(const AssetId&) const = default;
    };
}

/// @brief std::hash specialization for Veng::AssetId, enabling use in unordered containers.
template <>
struct std::hash<Veng::AssetId>
{
    /// @brief Returns the hash of the AssetId's underlying value.
    /// @param id  The id to hash.
    /// @return Hash of id.Value.
    Veng::usize operator()(const Veng::AssetId& id) const noexcept
    {
        return std::hash<Veng::u64>{}(id.Value);
    }
};
