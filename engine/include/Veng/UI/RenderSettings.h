#pragma once

#include <Veng/Veng.h>

namespace Veng::Renderer
{
    struct ViewState;
}

namespace Veng::UI
{
    /// @brief Draws editable widgets for a ViewState's per-frame tone and bloom knobs.
    ///
    /// The reusable render-settings surface for any viewport owner: exposure and the bloom
    /// threshold/intensity/radius — the ViewState values a consumer pushes each frame, so an
    /// edit takes effect immediately with no renderer reconfigure. The caller owns the edited
    /// ViewState (typically a persistent template it copies into its per-frame push), and
    /// places the widgets inside whatever window or section it draws.
    /// @param state  The view state edited in place.
    /// @return True the frame any value changed.
    [[nodiscard]] bool DrawViewStateSettings(Renderer::ViewState& state);
}
