#pragma once

#include <Veng/Veng.h>

#include <Veng/Scene/Entity.h>
#include <Veng/WorldInstanceId.h>

#include <functional>

namespace Veng
{
    /// @brief A seat named across worlds: the world it lives in and its Viewer entity there.
    ///
    /// A seat's Viewer entity is a scene-local handle, so two worlds' seats can share one — every
    /// world built from the same level mints the same handles. Whatever outlives a single world and
    /// is keyed by seat (the router's focus stacks, its cursor seat, its viewport associations)
    /// therefore names the world too, or one world's seat reads another's state.
    ///
    /// A null Viewer is the implicit seat that reads every device (see InputRouter::SetCursorSeat),
    /// which belongs to no world: the router treats every null-Viewer ref as that one seat, whatever
    /// its World.
    struct SeatRef
    {
        /// @brief The world the seat lives in.
        WorldInstanceId World;
        /// @brief The seat's Viewer entity in that world, or Entity::Null for the implicit seat.
        Entity Viewer = Entity::Null;

        /// @brief Whether this names the implicit all-devices seat.
        [[nodiscard]] bool IsImplicit() const { return Viewer == Entity::Null; }

        /// @brief Member-wise equality on world and entity.
        bool operator==(const SeatRef&) const = default;
    };
}

/// @brief std::hash specialization for Veng::SeatRef, combining the world and the entity.
template <>
struct std::hash<Veng::SeatRef>
{
    /// @brief Hashes a seat by its world id and entity index.
    Veng::usize operator()(const Veng::SeatRef& seat) const noexcept
    {
        const Veng::usize world = std::hash<Veng::u64>{}(seat.World.Value);
        const Veng::usize viewer = std::hash<Veng::Entity>{}(seat.Viewer);
        return world ^ (viewer + 0x9E3779B97F4A7C15ULL + (world << 6) + (world >> 2));
    }
};
