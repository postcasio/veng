#pragma once

#include <Veng/Veng.h>

/// @brief Device-free rect-placement helpers shared by anything positioning a box in a
/// pixel-like space.
///
/// Pure glm value math with no `Document` dependency and no notion of framebuffer pixels vs.
/// document logical points — a caller applies these in whichever space its own coordinates are
/// already in (region framebuffer pixels, document points, or an arbitrary panel's local space).
namespace Veng::Gui
{
    /// @brief Returns the top-left that keeps a size-`size` rect fully inside `[margin, bounds -
    /// margin]`.
    ///
    /// The floating-card/label idiom: given a proposed top-left, slides it the minimum amount on
    /// each axis so `[pos, pos + size]` never crosses the margin inset of `bounds`. When `bounds`
    /// is too small to hold `size` plus the margin on both edges, the valid range on that axis
    /// collapses to a single point at the margin corner rather than producing an inverted range
    /// or NaN — the rect still gets a well-defined position, just clipped against the near edge.
    /// @param pos     The proposed top-left.
    /// @param size    The rect's size.
    /// @param bounds  The size of the containing rect, whose top-left is the origin.
    /// @param margin  The inset kept clear on every edge of `bounds`.
    /// @return The clamped top-left.
    [[nodiscard]] vec2 ClampIntoBounds(vec2 pos, vec2 size, vec2 bounds, f32 margin);

    /// @brief Places a rect beside an anchor point, then clamps it fully inside the bounds.
    ///
    /// The offset-beside-a-point-clamped-into-region idiom every floating card/label site
    /// hand-writes: propose `anchor + offset` as the top-left, then run ClampIntoBounds against
    /// `bounds`/`margin`. One call replaces the two-step composition.
    /// @param anchor  The point the rect is offset from (e.g. a projected world position).
    /// @param size    The rect's size.
    /// @param offset  The signed offset from `anchor` to the rect's proposed top-left.
    /// @param bounds  The size of the containing rect, whose top-left is the origin.
    /// @param margin  The inset kept clear on every edge of `bounds`.
    /// @return The clamped top-left.
    /// @see ClampIntoBounds
    [[nodiscard]] vec2 AnchorBeside(vec2 anchor, vec2 size, vec2 offset, vec2 bounds, f32 margin);
}
