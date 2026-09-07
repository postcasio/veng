#pragma once

#include <Veng/Veng.h>
#include <Veng/Render/DisplayModes.h>

namespace Veng
{
    /// @brief One video mode a monitor can drive: a pixel resolution paired with a refresh rate.
    ///
    /// A monitor reports a list of these; the built-in Display dropdowns are populated from it, so
    /// the values are the hardware's, discovered at runtime and never authored.
    struct DisplayVideoMode
    {
        /// @brief Mode resolution in pixels.
        uvec2 Resolution{0, 0};
        /// @brief Mode refresh rate in Hz.
        u32 RefreshRateHz = 0;
    };

    /// @brief One connected monitor and the modes it can drive.
    ///
    /// MonitorId matches BuiltinDisplayChoices::MonitorId: index 0 is the primary display. Modes is
    /// deduplicated and ordered (ascending by area, then refresh); the monitor's CurrentResolution /
    /// CurrentRefreshRateHz appear in it, so a well-formed capability set always offers the mode the
    /// display is on.
    struct MonitorInfo
    {
        /// @brief Index of this monitor; 0 is the primary display.
        u32 MonitorId = 0;
        /// @brief Human-readable monitor name reported by the platform, for the menu label.
        string Name;
        /// @brief The monitor's current (desktop) resolution in pixels.
        uvec2 CurrentResolution{0, 0};
        /// @brief The monitor's current refresh rate in Hz.
        u32 CurrentRefreshRateHz = 0;
        /// @brief The distinct video modes this monitor can drive, ordered ascending.
        vector<DisplayVideoMode> Modes;
    };

    /// @brief What the hardware offers for the built-in Display group, discovered at runtime.
    ///
    /// The connected monitors (each with its supported resolutions and refresh rates) and the
    /// present modes the surface supports. This is what the menu's built-in Display dropdowns read,
    /// and what a persisted selection is validated against before it is applied — a settings file
    /// naming gone hardware (an unplugged monitor, an unsupported mode) is clamped to something the
    /// live hardware offers rather than landing the player on a black or off-screen surface. Empty
    /// when the run has no window (a headless/dedicated process).
    struct DisplayCapabilities
    {
        /// @brief The connected monitors, index 0 the primary; empty when there is no window.
        vector<MonitorInfo> Monitors;
        /// @brief The present modes the surface supports; Vsync (FIFO) is always among them.
        vector<PresentMode> PresentModes;
        /// @brief Whether the platform can drive a true exclusive full-screen mode.
        ///
        /// False on MoltenVK/macOS, where Metal exposes no exclusive-fullscreen and a request for it
        /// collapses to Borderless. A validated Exclusive selection drops to Borderless when this is
        /// false.
        bool SupportsExclusiveFullscreen = false;
    };
}
