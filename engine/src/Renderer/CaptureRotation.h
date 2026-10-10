#pragma once

#include <Veng/Veng.h>

namespace Veng::Renderer
{
    /// @brief Whether a bounded capture drive can still afford one more capture this frame.
    ///
    /// A driven capture claims at most one view slot (it renders one cube face per frame), and the
    /// registered viewports are reserved one slot each: a viewport that cannot claim shows a stale
    /// window, while a capture that cannot only holds its last map. So the drive stops as soon as all
    /// that remains is the viewports' reservation.
    /// @param remainingViews  Slots still claimable this frame (BindlessRegistry::GetRemainingViews).
    /// @param reservedViews   Slots held back for the registered viewports: one each, two for one
    ///                        whose half-res translucent layer is active
    ///                        (SceneRenderer::GetViewSlotsPerExecute).
    /// @return True while a capture may still claim a slot.
    [[nodiscard]] constexpr bool CaptureDriveHasRoom(const u32 remainingViews,
                                                     const u32 reservedViews)
    {
        return remainingViews > reservedViews;
    }

    /// @brief The view slots work recorded ahead of a frame's renders leaves for them.
    ///
    /// The viewports' slots are reserved whole. The captures and portraits are background too — each
    /// keeps its last render when it cannot claim — so their demand is reserved only up to half of
    /// what the viewports leave, and work ahead of them can always reach the other half rather than
    /// a large capture set starving it.
    /// @param viewportSlots  Slots the registered viewports' renders claim (ReservedViewSlots).
    /// @param captureDemand  Slots the captures and portraits would claim: one each.
    /// @param maxViews       The frame's view budget (BindlessRegistry::MaxViewsPerFrame).
    /// @return The slots to leave for the frame's renders.
    [[nodiscard]] constexpr u32 BackgroundViewReserve(const u32 viewportSlots,
                                                      const u32 captureDemand, const u32 maxViews)
    {
        const u32 spare = maxViews > viewportSlots ? maxViews - viewportSlots : 0u;
        return viewportSlots + (captureDemand < spare / 2 ? captureDemand : spare / 2);
    }

    /// @brief Whether a renderer may claim its half-res translucent region with this many view slots
    ///        left this frame: only while one would remain for its full region after it.
    /// @param remainingViews  Slots still claimable this frame (BindlessRegistry::GetRemainingViews).
    /// @return True when both regions fit.
    [[nodiscard]] constexpr bool HalfResViewFits(const u32 remainingViews)
    {
        return remainingViews >= 2;
    }

    /// @brief The drive-list index a frame's capture drive visits on its step-th step.
    ///
    /// The rotation is the drive-list read cyclically from the cursor, so a frame that can afford only
    /// part of the list still covers a contiguous run of it rather than always the same prefix.
    /// @param cursor  Index this frame's drive resumes at.
    /// @param step    Step within this frame's drive, from zero.
    /// @param count   Captures in the drive-list.
    /// @return The index to drive, or 0 for an empty list.
    [[nodiscard]] constexpr usize CaptureDriveIndex(const usize cursor, const usize step,
                                                    const usize count)
    {
        return count == 0 ? 0 : (cursor + step) % count;
    }

    /// @brief The cursor the next frame's capture drive resumes at.
    ///
    /// A frame that drove the whole list leaves the cursor alone — the order is immaterial when nothing
    /// was dropped. A frame that ran out of budget leaves it on the first capture it could not afford,
    /// so the next frame begins with the ones this frame starved: every capture in a list larger than
    /// the budget is reached within ceil(count / driven) frames rather than the tail never rendering.
    /// @param cursor  Index this frame's drive resumed at.
    /// @param driven  Captures this frame attempted.
    /// @param count   Captures in the drive-list.
    /// @return The cursor for the next frame.
    [[nodiscard]] constexpr usize NextCaptureCursor(const usize cursor, const usize driven,
                                                    const usize count)
    {
        return count == 0 || driven >= count ? cursor : (cursor + driven) % count;
    }
}
