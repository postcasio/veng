#pragma once

#include <Veng/Veng.h>

namespace Veng
{
    /// @brief A stable, never-reused identity for a world, resolved through a WorldRunner.
    ///
    /// Wraps a u64 minted from the runner's instance counter; zero is the invalid,
    /// names-no-world spelling. Minted in WorldRunner::OpenWorld and dropped in CloseWorld, so an
    /// id names one world across its whole open lifetime, and once that world is closed the id
    /// resolves to nothing rather than to a world that reused its slot. The counter never repeats,
    /// so absence alone detects a stale id — no generation field is needed.
    struct WorldInstanceId
    {
        /// @brief The identity value; zero is the invalid, names-no-world id.
        u64 Value = 0;

        /// @brief Returns whether this id names a minted world.
        [[nodiscard]] bool IsValid() const { return Value != 0; }

        /// @brief Member-wise equality on the identity value.
        bool operator==(const WorldInstanceId&) const = default;
    };
}
