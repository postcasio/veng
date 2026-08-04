#pragma once

#include <Veng/Veng.h>

namespace Veng::Audio
{
    /// @brief The fixed set of mixing buses, each a group under the master.
    ///
    /// The tree is deliberately closed: Master is the root every other bus routes into, and the
    /// four category buses give the game independent gain and effect control without a designer-
    /// authorable bus graph. A richer, author-defined tree is a separate capability, not a
    /// variation of this enum.
    enum class AudioBus : u8
    {
        /// @brief The root bus; every other bus mixes into it and its gain scales the whole output.
        Master = 0,
        /// @brief The background-music bus.
        Music,
        /// @brief The sound-effect bus.
        SFX,
        /// @brief The user-interface bus.
        UI,
        /// @brief The ambience bus.
        Ambience,
    };

    /// @brief The number of buses in the fixed tree; sizes every per-bus array.
    inline constexpr usize AudioBusCount = 5;

    /// @brief Returns a stable lowercase-free debug name for a bus (never null).
    /// @param bus The bus to name.
    /// @return A static string naming the bus.
    const char* ToString(AudioBus bus);
}
