#pragma once

#include <Veng/Veng.h>

namespace Veng::Renderer
{
    /// @brief A stable, never-reused identity for a Viewport, resolved through a ViewportRegistry.
    ///
    /// Wraps a u64 whose high bits carry the minting registry's salt and low bits a monotonic
    /// counter; zero is the invalid, names-no-viewport spelling. Minted in Viewport::Create and
    /// dropped in ~Viewport, so an id names one viewport across its whole construction lifetime,
    /// and once that viewport is gone the id resolves to nothing rather than to a new viewport that
    /// reused its allocator slot. Because slots are never pooled, the counter never repeats, so
    /// absence alone detects a stale id — no generation field is needed.
    struct ViewportId
    {
        /// @brief The identity value; zero is the invalid, names-no-viewport id.
        u64 Value = 0;

        /// @brief Returns whether this id names a minted viewport.
        [[nodiscard]] bool IsValid() const { return Value != 0; }

        /// @brief Member-wise equality on the identity value.
        bool operator==(const ViewportId&) const = default;
    };
}
