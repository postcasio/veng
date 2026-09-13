#pragma once
#include <Veng/Veng.h>

#include <Veng/Capture/VideoRecorder.h>

/// @brief The video recorder's à-la-carte control panel, in the `Veng::UI` widget vocabulary.
///
/// One panel and the two pure helpers it is built from. Like every panel in `DebugPanels.h` it
/// opens no window of its own and keeps no static state: a host draws it inside a window it owns
/// and holds the edited settings in a `VideoCapturePanelState` of its own, so a debug shell and an
/// editor can each place it in their own menu and docking.

namespace Veng::UI
{
    /// @brief The settings a host's capture panel edits between frames, owned by the host.
    ///
    /// The panel is a free function over this, so nothing about a capture's in-progress editing
    /// lives in the engine: a host holds one per panel and it survives the window being closed.
    struct VideoCapturePanelState
    {
        /// @brief The edited settings, less the name (the field below is what the widget edits).
        Capture::VideoCaptureSettings Settings;

        /// @brief The name field's buffer; blank starts a capture under the recorder's default name.
        string Name;
    };

    /// @brief Returns the settings a Start from @p state would use.
    ///
    /// The edited settings with the name field's buffer folded in, trimmed of surrounding
    /// whitespace — so a blank buffer yields an empty name, which the recorder resolves to its
    /// default `<app>-<yyyymmdd-hhmmss>` form.
    /// @param state  The panel state to resolve.
    /// @return The settings to hand VideoRecorder::Start.
    [[nodiscard]] Capture::VideoCaptureSettings
    ResolveCaptureSettings(const VideoCapturePanelState& state);

    /// @brief Returns the average bitrate @p settings resolve to at @p extent, in megabits/second.
    ///
    /// The figure the panel shows beside the quality control, so a bits-per-pixel number reads as a
    /// file size. The recorder's own derivation, rounded to whole megabits; 0 when the extent is
    /// not yet known.
    /// @param settings  The capture's settings.
    /// @param extent    The picture size in pixels.
    /// @return The average bitrate in megabits per second.
    [[nodiscard]] u32 CaptureBitrateMbps(const Capture::VideoCaptureSettings& settings,
                                         uvec2 extent);

    /// @brief Draws the recorder's settings, live state and Start/Stop control into the current window.
    ///
    /// Three layouts, by the recorder's status. Idle draws the settings — encoding, codec, quality,
    /// lockstep and its frame rate, the frame budget, audio, whether the application's own overlay
    /// is composited in, and the capture's name — over a Start button, or the reason a recorder
    /// that cannot record gives instead. Recording draws the same settings disabled above the live
    /// figures (codec, encoding, extent, frames, duration, file size, and the encoder-behind and
    /// audio-loss rows, which appear only when they are non-zero) over a Stop button. Finalizing
    /// draws neither button while the file is committed.
    ///
    /// Reads the recorder's state once per call, which is also what advances a finished capture
    /// from Finalizing to Off. Draws into the current window; the caller owns the window.
    /// @param recorder  The recorder the panel reports on and drives.
    /// @param state     The host-owned edited settings, read and written in place.
    void VideoCapturePanel(Capture::VideoRecorder& recorder, VideoCapturePanelState& state);
}
