#pragma once

#include <Veng/Veng.h>
#include <Veng/Reflection/Reflect.h>

namespace Veng
{
    /// @brief How the window occupies the display: a bordered window, a borderless full-screen
    ///        window, or an exclusive full-screen mode.
    ///
    /// The engine resolves the selection against the window and swapchain; the persisted store
    /// carries only the choice. Windowed is the engine default. Exclusive is best-effort and
    /// platform-dependent: on a platform with no true exclusive-fullscreen (MoltenVK/macOS, where
    /// Metal exposes none) it collapses to Borderless.
    enum class FullscreenMode : u32
    {
        /// @brief A bordered, movable window at the chosen resolution.
        Windowed = 0,
        /// @brief A borderless window covering the whole monitor at its current mode.
        Borderless = 1,
        /// @brief An exclusive full-screen mode owning the display output.
        Exclusive = 2,
    };

    /// @brief How presented frames synchronize with the display's refresh.
    ///
    /// The engine maps the selection onto a concrete swapchain present mode against what the
    /// device offers; the persisted store carries only the choice. Vsync is the engine default.
    /// A requested mode the device does not offer falls back to Vsync (FIFO, always supported)
    /// with a warning.
    enum class PresentMode : u32
    {
        /// @brief Presentation waits for vertical blank — no tearing, refresh-bounded frame rate.
        Vsync = 0,
        /// @brief Presentation never waits — lowest latency, may tear.
        Immediate = 1,
        /// @brief Triple-buffered presentation — no tearing, no hard refresh cap.
        Mailbox = 2,
    };
}

VE_ENUM(::Veng::FullscreenMode, 0x5132C91170D8002BULL)
VE_ENUMERATOR(Windowed)
VE_ENUMERATOR(Borderless)
VE_ENUMERATOR(Exclusive)
VE_ENUM_END();

VE_ENUM(::Veng::PresentMode, 0xD45111C1D3B1C65EULL)
VE_ENUMERATOR(Vsync)
VE_ENUMERATOR(Immediate)
VE_ENUMERATOR(Mailbox)
VE_ENUM_END();
