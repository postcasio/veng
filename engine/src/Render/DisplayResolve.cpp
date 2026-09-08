#include "DisplayResolve.h"

#include <algorithm>

namespace Veng
{
    namespace
    {
        u64 ModeArea(const DisplayVideoMode& mode)
        {
            return static_cast<u64>(mode.Resolution.x) * static_cast<u64>(mode.Resolution.y);
        }

        // The absolute distance between two areas, for nearest-mode selection.
        u64 AreaDistance(u64 a, u64 b)
        {
            return a > b ? a - b : b - a;
        }

        const MonitorInfo* FindMonitor(const DisplayCapabilities& caps, u32 monitorId)
        {
            for (const MonitorInfo& monitor : caps.Monitors)
            {
                if (monitor.MonitorId == monitorId)
                {
                    return &monitor;
                }
            }
            return nullptr;
        }
    }

    vector<FullscreenMode> PlatformFullscreenModes()
    {
#if defined(__APPLE__)
        // macOS full-screen is one native Cocoa toggle; Borderless is that single choice and
        // Exclusive is meaningless under it.
        return {FullscreenMode::Windowed, FullscreenMode::Borderless};
#else
        return {FullscreenMode::Windowed, FullscreenMode::Borderless, FullscreenMode::Exclusive};
#endif
    }

    vector<DisplayVideoMode> DedupVideoModes(std::span<const DisplayVideoMode> modes)
    {
        vector<DisplayVideoMode> result(modes.begin(), modes.end());
        std::ranges::sort(result,
                          [](const DisplayVideoMode& a, const DisplayVideoMode& b)
                          {
                              const u64 areaA = ModeArea(a);
                              const u64 areaB = ModeArea(b);
                              if (areaA != areaB)
                              {
                                  return areaA < areaB;
                              }
                              if (a.Resolution.x != b.Resolution.x)
                              {
                                  return a.Resolution.x < b.Resolution.x;
                              }
                              return a.RefreshRateHz < b.RefreshRateHz;
                          });
        const auto duplicate = [](const DisplayVideoMode& a, const DisplayVideoMode& b)
        { return a.Resolution == b.Resolution && a.RefreshRateHz == b.RefreshRateHz; };
        result.erase(std::ranges::unique(result, duplicate).begin(), result.end());
        return result;
    }

    PresentMode ResolvePresentMode(PresentMode requested, std::span<const PresentMode> supported)
    {
        if (std::ranges::find(supported, requested) != supported.end())
        {
            return requested;
        }
        if (requested != PresentMode::Vsync)
        {
            Log::Warn("Requested present mode {} is unsupported; falling back to Vsync",
                      static_cast<u32>(requested));
        }
        return PresentMode::Vsync;
    }

    BuiltinDisplayChoices ResolveDisplaySelection(const BuiltinDisplayChoices& requested,
                                                  const DisplayCapabilities& caps)
    {
        BuiltinDisplayChoices resolved = requested;

        // Present mode: fall back against the surface's supported set.
        resolved.Present = ResolvePresentMode(requested.Present, caps.PresentModes);

        // A fullscreen mode the platform does not offer (Exclusive on macOS) clamps to the
        // platform's fullscreen choice, Borderless. An empty set is a headless run with nothing to
        // validate against, so the selection passes through.
        if (!caps.AvailableFullscreenModes.empty() &&
            std::ranges::find(caps.AvailableFullscreenModes, resolved.Fullscreen) ==
                caps.AvailableFullscreenModes.end())
        {
            const bool offersBorderless =
                std::ranges::find(caps.AvailableFullscreenModes, FullscreenMode::Borderless) !=
                caps.AvailableFullscreenModes.end();
            const FullscreenMode fallback =
                offersBorderless ? FullscreenMode::Borderless : FullscreenMode::Windowed;
            Log::Warn("Fullscreen mode {} is unavailable on this platform; using {}",
                      static_cast<u32>(resolved.Fullscreen), static_cast<u32>(fallback));
            resolved.Fullscreen = fallback;
        }

        // With no monitors reported (a headless run), leave the identity fields alone — there is
        // nothing to validate them against and nothing will read them.
        if (caps.Monitors.empty())
        {
            return resolved;
        }

        // Monitor: an absent index falls back to the primary (index 0).
        const MonitorInfo* monitor = FindMonitor(caps, requested.MonitorId);
        if (monitor == nullptr)
        {
            Log::Warn("Monitor {} is not connected; falling back to the primary display",
                      requested.MonitorId);
            resolved.MonitorId = 0;
            monitor = FindMonitor(caps, 0);
            if (monitor == nullptr)
            {
                monitor = &caps.Monitors.front();
                resolved.MonitorId = monitor->MonitorId;
            }
        }

        // Resolution: zero means native and is left as-is; a non-zero resolution the monitor cannot
        // drive clamps to the nearest offered mode by area.
        if (resolved.Resolution != uvec2{0, 0} && !monitor->Modes.empty())
        {
            const bool offered =
                std::ranges::any_of(monitor->Modes, [&](const DisplayVideoMode& mode)
                                    { return mode.Resolution == resolved.Resolution; });
            if (!offered)
            {
                const u64 wanted = static_cast<u64>(resolved.Resolution.x) *
                                   static_cast<u64>(resolved.Resolution.y);
                const DisplayVideoMode* nearest = nullptr;
                for (const DisplayVideoMode& mode : monitor->Modes)
                {
                    if (nearest == nullptr || AreaDistance(ModeArea(mode), wanted) <
                                                  AreaDistance(ModeArea(*nearest), wanted))
                    {
                        nearest = &mode;
                    }
                }
                Log::Warn("Resolution {}x{} is not offered by monitor {}; clamping to {}x{}",
                          resolved.Resolution.x, resolved.Resolution.y, resolved.MonitorId,
                          nearest->Resolution.x, nearest->Resolution.y);
                resolved.Resolution = nearest->Resolution;
            }
        }

        // Refresh: zero means native; a non-zero refresh the chosen resolution cannot drive clamps
        // to the highest refresh offered at that resolution.
        if (resolved.RefreshRateHz != 0 && resolved.Resolution != uvec2{0, 0})
        {
            u32 bestRefresh = 0;
            bool exact = false;
            for (const DisplayVideoMode& mode : monitor->Modes)
            {
                if (mode.Resolution != resolved.Resolution)
                {
                    continue;
                }
                if (mode.RefreshRateHz == resolved.RefreshRateHz)
                {
                    exact = true;
                    break;
                }
                bestRefresh = std::max(bestRefresh, mode.RefreshRateHz);
            }
            if (!exact && bestRefresh != 0)
            {
                Log::Warn("Refresh {}Hz is not offered at {}x{}; clamping to {}Hz",
                          resolved.RefreshRateHz, resolved.Resolution.x, resolved.Resolution.y,
                          bestRefresh);
                resolved.RefreshRateHz = bestRefresh;
            }
        }

        return resolved;
    }

    DisplayApplyActions ComputeDisplayApplyActions(const optional<BuiltinDisplayChoices>& current,
                                                   const BuiltinDisplayChoices& resolved)
    {
        DisplayApplyActions actions;
        if (!current.has_value())
        {
            // First apply of the run: honor the persisted present mode and frame cap outright (the
            // window and swapchain were created with engine defaults, so a stored non-default must be
            // pushed). The window is touched only when the selection departs from windowed-native, so
            // the common default-selection boot does not resize or re-home the fresh window.
            actions.ChangePresentMode = true;
            actions.ChangeFrameCap = true;
            actions.ChangeWindow = resolved.Fullscreen != FullscreenMode::Windowed ||
                                   resolved.Resolution != uvec2{0, 0} ||
                                   resolved.RefreshRateHz != 0 || resolved.MonitorId != 0;
            return actions;
        }

        const BuiltinDisplayChoices& prev = *current;
        actions.ChangeWindow =
            prev.Fullscreen != resolved.Fullscreen || prev.MonitorId != resolved.MonitorId ||
            prev.Resolution != resolved.Resolution || prev.RefreshRateHz != resolved.RefreshRateHz;
        actions.ChangePresentMode = prev.Present != resolved.Present;
        actions.ChangeFrameCap = prev.FrameCapHz != resolved.FrameCapHz;
        return actions;
    }

    optional<FullscreenMode> DecideFullscreenWriteBack(const FullscreenMode live,
                                                       const optional<FullscreenMode> observed,
                                                       const FullscreenMode applied)
    {
        // No prior frame to compare, or nothing changed since it: not an event.
        if (!observed.has_value() || live == *observed)
        {
            return std::nullopt;
        }
        // The change reached the engine's own last-applied mode — its async apply completing, already
        // in the store. Any other change is the user's own toggle, which the store has not recorded.
        if (live == applied)
        {
            return std::nullopt;
        }
        return live;
    }
}
