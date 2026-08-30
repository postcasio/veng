#pragma once

#include <Veng/Veng.h>
#include <Veng/Scene/Entity.h>

namespace Veng::Renderer
{
    /// @brief The seat rule deciding whether one presenting viewport drives a GuiSurface's driver.
    ///
    /// A surface naming a seat is driven by the viewport bound to that seat — but a viewport with
    /// no bound seat is the documented single-player posture (Viewport::SetSeat's default), the one
    /// seat that reads every device, so it must be able to claim a seated surface too: a surface
    /// has to name a seat to take routed pointer input at all, and a seated surface no viewport
    /// could ever claim would render its document forever while its driver never ran. The unbound
    /// viewport therefore falls through to the primary-presenter rule, unless some presenting
    /// viewport genuinely binds the surface's seat — real split-screen — in which case that
    /// viewport's claim is exclusive.
    ///
    /// Pure so the rule is pinned device-free; Viewport::ClaimsSurface gathers the inputs.
    /// @param surfaceSeat                The seat the surface names; Null for an unseated surface.
    /// @param selfSeat                   This viewport's bound seat; Null when unbound.
    /// @param selfIsPrimaryPresenter     Whether this viewport is the scene's sole/primary presenter.
    /// @param anotherPresenterBindsSeat  Whether any presenting viewport's bound seat is @p surfaceSeat.
    /// @return True when this viewport should drive the surface's driver.
    [[nodiscard]] inline bool ClaimsSeatedSurface(const Entity surfaceSeat, const Entity selfSeat,
                                                  const bool selfIsPrimaryPresenter,
                                                  const bool anotherPresenterBindsSeat)
    {
        if (!surfaceSeat.IsNull())
        {
            if (surfaceSeat == selfSeat)
            {
                return true;
            }
            if (!selfSeat.IsNull())
            {
                return false;
            }
            if (anotherPresenterBindsSeat)
            {
                return false;
            }
        }
        return selfIsPrimaryPresenter;
    }
}
