#pragma once

#include <span>

#include <Veng/Veng.h>
#include <Veng/Render/DisplayCapabilities.h>
#include <Veng/Render/DisplayModes.h>
#include <Veng/Render/GraphicsSettings.h>

namespace Veng
{
    /// @brief Sorts and deduplicates a monitor's raw video-mode list into a canonical order.
    ///
    /// Ordered ascending by pixel area, then by refresh rate, with exact (resolution, refresh)
    /// duplicates collapsed — GLFW reports one entry per (resolution, refresh, bit-depth) tuple, so
    /// the same mode recurs across colour depths. Pure and device-free.
    /// @param modes  The raw modes to canonicalize.
    /// @return The sorted, deduplicated modes.
    [[nodiscard]] vector<DisplayVideoMode> DedupVideoModes(std::span<const DisplayVideoMode> modes);

    /// @brief The full-screen modes the host platform offers, in menu order.
    ///
    /// A compile-time platform fact: {Windowed, Borderless} on macOS — where full-screen is a single
    /// native Cocoa toggle and Exclusive is meaningless — and {Windowed, Borderless, Exclusive}
    /// elsewhere. GetDisplayCapabilities publishes this as AvailableFullscreenModes, and
    /// ResolveDisplaySelection clamps a selection against it. Pure and device-free.
    /// @return The offered full-screen modes for this platform.
    [[nodiscard]] vector<FullscreenMode> PlatformFullscreenModes();

    /// @brief The unit a platform's window-system sizing calls take.
    enum class ScreenCoordinateUnit : u8
    {
        /// @brief Screen coordinates are pixels (Windows, X11): sizes pass through unchanged.
        Pixels,
        /// @brief Screen coordinates are points (macOS): a pixel size divides by the content scale.
        Points,
    };

    /// @brief The unit the host platform's screen coordinates are in.
    ///
    /// A compile-time platform fact: points on macOS, where a window's size and a video mode are
    /// reported in points and its framebuffer is the content scale larger, and pixels elsewhere,
    /// where the content scale is a UI hint only and never changes what a size means. Pure and
    /// device-free.
    /// @return The host's screen-coordinate unit.
    [[nodiscard]] ScreenCoordinateUnit PlatformScreenCoordinateUnit();

    /// @brief How many pixels one screen coordinate spans on each axis.
    ///
    /// The content scale under Points and 1 under Pixels. A cursor position arrives in screen
    /// coordinates and a viewport region is in framebuffer pixels, so this is the factor between
    /// them. A zero or negative scale component counts as 1. Pure and device-free.
    /// @param contentScale  The window's content scale.
    /// @param unit          The platform's screen-coordinate unit.
    /// @return Pixels per screen coordinate.
    [[nodiscard]] vec2 PixelsPerScreenCoordinate(vec2 contentScale, ScreenCoordinateUnit unit);

    /// @brief Converts a pixel resolution to the screen coordinates the window-system sizing calls
    ///        take.
    ///
    /// Every resolution the display settings carry is in pixels. Under Points the result divides by
    /// the content scale (rounded to nearest); under Pixels it is the identity, whatever the scale.
    /// A zero or negative scale component counts as 1. Pure and device-free.
    /// @param pixels        The resolution in pixels.
    /// @param contentScale  The window's or monitor's content scale.
    /// @param unit          The platform's screen-coordinate unit.
    /// @return The size in screen coordinates.
    [[nodiscard]] uvec2 ScreenCoordsFromPixels(uvec2 pixels, vec2 contentScale,
                                               ScreenCoordinateUnit unit);

    /// @brief Converts a size in screen coordinates to pixels; the inverse of ScreenCoordsFromPixels.
    ///
    /// Under Points the result multiplies by the content scale (rounded to nearest); under Pixels it
    /// is the identity. A zero or negative scale component counts as 1. Pure and device-free.
    /// @param coords        The size in screen coordinates (a video mode, a window size).
    /// @param contentScale  The window's or monitor's content scale.
    /// @param unit          The platform's screen-coordinate unit.
    /// @return The size in pixels.
    [[nodiscard]] uvec2 PixelsFromScreenCoords(uvec2 coords, vec2 contentScale,
                                               ScreenCoordinateUnit unit);

    /// @brief Resolves a requested present mode against what the surface supports.
    ///
    /// Returns the requested mode when it is offered, else Vsync (FIFO), which every Vulkan surface
    /// supports — so selection never fails. Pure and device-free; the swapchain and the settings
    /// validation both keep this property.
    /// @param requested  The present mode the settings ask for.
    /// @param supported  The present modes the surface reports.
    /// @return The requested mode when supported, else PresentMode::Vsync.
    [[nodiscard]] PresentMode ResolvePresentMode(PresentMode requested,
                                                 std::span<const PresentMode> supported);

    /// @brief Clamps a persisted display selection to what the live hardware offers.
    ///
    /// Validates every hardware-facing field against @p caps so a settings file naming gone hardware
    /// never lands the player off-screen: an absent MonitorId falls back to the primary, a
    /// resolution/refresh the monitor cannot drive clamps to the nearest supported mode, an
    /// unsupported present mode drops to Vsync, and a fullscreen mode the platform does not offer
    /// (an Exclusive selection on macOS) drops to the platform's fullscreen choice (Borderless) —
    /// each with a warning. A zero Resolution/RefreshRateHz means "the
    /// display's native value" and is left as zero (resolved at apply against the current mode). The
    /// non-hardware fields (frame cap, render scale, brightness, gamma) pass through untouched.
    /// @param requested  The persisted selection.
    /// @param caps       The live hardware capabilities.
    /// @return The clamped selection, safe to apply.
    [[nodiscard]] BuiltinDisplayChoices
    ResolveDisplaySelection(const BuiltinDisplayChoices& requested,
                            const DisplayCapabilities& caps);

    /// @brief The device work a display apply must perform, decided by diffing against current state.
    ///
    /// Each flag gates one class of work so an apply does only what changed: a frame-cap-only change
    /// touches no swapchain, a resolution change recreates it exactly once. A window or present-mode
    /// change both drive a swapchain recreation, coalesced by the frame-safe recreate flag.
    struct DisplayApplyActions
    {
        /// @brief Whether the window's resolution / fullscreen / monitor / refresh must be re-applied.
        bool ChangeWindow = false;
        /// @brief Whether the swapchain's present mode must be re-requested.
        bool ChangePresentMode = false;
        /// @brief Whether the run-loop frame cap must be re-set.
        bool ChangeFrameCap = false;
    };

    /// @brief Decides which display-apply actions a move from @p current to @p resolved needs.
    ///
    /// The diffed-apply core: with a current state it compares field by field; with none (the first
    /// apply of a run) it forces the present mode and frame cap so the persisted defaults take effect,
    /// and the window only when @p resolved departs from the neutral windowed-native state (so the
    /// first apply of pure defaults does not disturb the freshly created window). Pure and device-free.
    /// @param current   The last applied selection, or nullopt on the first apply of a run.
    /// @param resolved  The selection about to be applied (already clamped to the hardware).
    /// @return The actions to perform.
    [[nodiscard]] DisplayApplyActions
    ComputeDisplayApplyActions(const optional<BuiltinDisplayChoices>& current,
                               const BuiltinDisplayChoices& resolved);

    /// @brief Decides whether a user-driven full-screen toggle must be written back to the store.
    ///
    /// The macOS green title-bar button toggles the window's native full-screen directly, so the
    /// persisted selection can drift from the live window. Called once per frame with the window's
    /// live mode, the previous frame's observed mode, and the mode the engine last applied, it returns
    /// the mode to persist when the live mode changed to one the engine did not apply itself — and
    /// nullopt otherwise: no prior observation to compare against (the seed frame), no change since
    /// last frame, or a change that merely reaches the last-applied mode (the engine's own async apply
    /// completing, which the store already holds). Pure and device-free.
    /// @param live      The window's current full-screen mode.
    /// @param observed  The mode observed the previous frame, or nullopt before the first observation.
    /// @param applied   The full-screen mode the engine last applied.
    /// @return The mode to persist, or nullopt when no write-back is needed.
    [[nodiscard]] optional<FullscreenMode>
    DecideFullscreenWriteBack(FullscreenMode live, optional<FullscreenMode> observed,
                              FullscreenMode applied);
}
