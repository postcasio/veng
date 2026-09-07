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
    /// unsupported present mode drops to Vsync, and an Exclusive selection the platform cannot honor
    /// drops to Borderless — each with a warning. A zero Resolution/RefreshRateHz means "the
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
}
