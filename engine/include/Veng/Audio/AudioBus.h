#pragma once

#include <Veng/Veng.h>

#include <string_view>

namespace Veng::Audio
{
    /// @brief The most buses an authored graph may declare; sizes the RT-owned per-bus arrays.
    ///
    /// The real-time accumulators and per-bus filter state are grown to this once, off the RT
    /// thread, and never resized when a graph is adopted — so a data-driven tree stays
    /// allocation-free on the mixing thread. A graph exceeding this is rejected at load.
    inline constexpr u32 MaxBuses = 64;

    /// @brief The deepest a bus may sit below the root; a graph exceeding this is rejected.
    ///
    /// Bounds the fold's work and rules out a pathologically deep tree. Master is depth 0.
    inline constexpr u32 MaxBusDepth = 8;

    /// @brief A stable, graph-independent handle to a mixing bus: the interned hash of its name.
    ///
    /// A bus is named by a string in the authored graph; a BusId is that name's 64-bit hash, so
    /// game code computes a bus id without holding the graph — `BusId{"Engines"}`. Voices carry a
    /// BusId, and the mixer resolves it to a table index at publish time (an id absent from the
    /// active graph falls back to Master). 0 is the reserved invalid id.
    struct BusId
    {
        /// @brief FNV-1a 64-bit hash of a bus name; the interning function every BusId shares.
        /// @param name  The bus name to hash.
        /// @return The name's hash (never 0 for a non-empty name in practice).
        static constexpr u64 Hash(std::string_view name)
        {
            u64 hash = 1469598103934665603ULL;
            for (const char character : name)
            {
                hash ^= static_cast<u64>(static_cast<unsigned char>(character));
                hash *= 1099511628211ULL;
            }
            return hash;
        }

        /// @brief The interned hash value; 0 is the reserved invalid id.
        u64 Value = 0;

        /// @brief Constructs the invalid id.
        constexpr BusId() = default;

        /// @brief Constructs the id of a named bus by interning its name.
        /// @param name  The bus name.
        constexpr explicit BusId(std::string_view name) : Value(Hash(name)) {}

        /// @brief Returns whether the id is non-zero (not the reserved invalid id).
        [[nodiscard]] constexpr bool IsValid() const { return Value != 0; }

        /// @brief Value equality over the interned hash.
        constexpr auto operator<=>(const BusId&) const = default;
    };

    /// @brief The well-known root-bus names the engine reserves, and their ids.
    ///
    /// These are the generic roots a game gets for free from the roots-only default graph; an
    /// authored graph that wants them declares them, and a name constant resolves only if the
    /// active graph actually declares that bus. The names are the contract, not a fixed topology:
    /// a game authors its own complete tree beneath (or beside) them.
    namespace AudioBuses
    {
        /// @brief The root bus name; every valid graph declares exactly this one root.
        inline constexpr std::string_view MasterName = "Master";
        /// @brief The background-music bus name.
        inline constexpr std::string_view MusicName = "Music";
        /// @brief The sound-effect bus name.
        inline constexpr std::string_view SFXName = "SFX";
        /// @brief The user-interface bus name.
        inline constexpr std::string_view UIName = "UI";
        /// @brief The ambience bus name.
        inline constexpr std::string_view AmbienceName = "Ambience";

        /// @brief The id of the root bus — the fallback every unresolved id routes to.
        constexpr BusId Master()
        {
            return BusId{MasterName};
        }
        /// @brief The id of the background-music bus.
        constexpr BusId Music()
        {
            return BusId{MusicName};
        }
        /// @brief The id of the sound-effect bus.
        constexpr BusId SFX()
        {
            return BusId{SFXName};
        }
        /// @brief The id of the user-interface bus.
        constexpr BusId UI()
        {
            return BusId{UIName};
        }
        /// @brief The id of the ambience bus.
        constexpr BusId Ambience()
        {
            return BusId{AmbienceName};
        }
    }
}
