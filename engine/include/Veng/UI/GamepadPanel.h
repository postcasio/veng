#pragma once
#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>

#include <span>

/// @brief The gamepad debug panel, in the `Veng::UI` widget vocabulary.
///
/// Like every panel in `DebugPanels.h` it opens no window of its own and keeps no static state: a
/// host draws it inside a window it owns.

namespace Veng
{
    class Application;
}

namespace Veng::Haptics
{
    class RumbleClip;
}

namespace Veng::UI
{
    /// @brief A rumble clip the gamepad panel offers to play, with the label its button shows.
    struct GamepadPanelClip
    {
        /// @brief The button's label.
        string_view Name;
        /// @brief The clip; a clip still loading plays nothing.
        AssetHandle<Haptics::RumbleClip> Clip;
    };

    /// @brief Draws every connected pad's slot, family, name, buttons, axes and touchpad, its
    ///        mixed rumble with the one-shots and layers feeding it, and sliders driving its
    ///        deadzones.
    ///
    /// Reads the pads through the application's Input, so what it shows is exactly what the action
    /// layer reads — neutral while the window is unfocused, a virtual pad like any other. Each axis
    /// shows its raw value beside the deadzone-shaped one the action layer reads, and the deadzone
    /// sliders set the pad's zones through Input::SetGamepadDeadzones, live. The rumble rows show the
    /// haptics engine's mix for the pad and every one-shot and layer on it, each with its scope and
    /// that scope's state; a button per offered clip plays it once on the pad in the application
    /// scope (Application::GetApplicationHaptics), and Stop drops the application scope's one-shots
    /// there.
    /// @param app    The application whose pads are shown.
    /// @param clips  The clips offered for playing on each pad; none draws no play buttons.
    void GamepadPanel(const Application& app, std::span<const GamepadPanelClip> clips = {});
}
