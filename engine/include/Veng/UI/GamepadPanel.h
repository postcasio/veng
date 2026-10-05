#pragma once
#include <Veng/Veng.h>

/// @brief The gamepad debug panel, in the `Veng::UI` widget vocabulary.
///
/// Like every panel in `DebugPanels.h` it opens no window of its own and keeps no static state: a
/// host draws it inside a window it owns.

namespace Veng
{
    class Application;
}

namespace Veng::UI
{
    /// @brief Draws every connected pad's slot, family, name, buttons, axes and touchpad, with
    ///        sliders driving its motors.
    ///
    /// Reads the pads through the application's Input, so what it shows is exactly what the action
    /// layer reads — neutral while the window is unfocused, a virtual pad like any other. The motor
    /// sliders set the pad's levels directly and hold them until moved back to zero; they are a
    /// diagnostic override of the engine's haptics, and an application has no other way to write a
    /// motor. With no pad backend (a headless run) the panel says so.
    /// @param app  The application whose pads are shown.
    void GamepadPanel(Application& app);
}
