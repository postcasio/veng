#pragma once

#include <span>
#include <unordered_map>

#include <Veng/Veng.h>
#include <Veng/Math/CubeSphere.h>
#include <Veng/Renderer/SphereTerrain.h>

#include "SphereTerrainResidency.h"

namespace Veng::Renderer
{
    /// @brief How long a split's children take to morph in from their parent's shape when a
    ///        split was delayed past its distance, in seconds.
    inline constexpr f32 SphereTerrainArrivalSeconds = 0.25f;

    /// @brief How long the tiles a coarser tile replaces keep drawing while they morph into its
    ///        shape, in seconds.
    inline constexpr f32 SphereTerrainDepartureSeconds = 0.25f;

    /// @brief What one source's arrivals and departures remember from frame to frame.
    struct SphereTerrainTransitionState
    {
        /// @brief The tiles drawn last frame, departing ones included, by key.
        std::unordered_map<u64, SphereTerrainDrawnTile> Drawn;
        /// @brief Per arriving tile, the clock time its arrival began.
        std::unordered_map<u64, f64> Arrivals;
        /// @brief Per departing tile, the clock time its departure began.
        std::unordered_map<u64, f64> Departures;
        /// @brief The seconds the source has been selected over.
        f64 Clock = 0.0;
    };

    /// @brief One frame's tiles to draw once arrivals and departures are applied.
    struct SphereTerrainTransitionFrame
    {
        /// @brief The tiles to draw: the selection's, with each coarser tile whose departures are
        ///        still running replaced by the finer tiles departing under it. Covers the sphere
        ///        once, as the selection does.
        vector<SphereTerrainDrawnTile> Drawn;
        /// @brief Per tile drawn under a floor, the morph floor it takes this frame.
        std::unordered_map<u64, f32> Floors;
        /// @brief The departing tiles drawn, which the pool must keep while they draw.
        vector<CubeSphereTileKey> Departing;
        /// @brief Drawn tiles under an arrival floor.
        u32 ArrivingTiles = 0;
    };

    /// @brief Applies one frame's arrivals and departures to a selection.
    ///
    /// Device-free and pure but for the state it advances. A tile drawn this frame under one drawn
    /// last frame **arrives**: its morph floor falls from 1 to 0 over SphereTerrainArrivalSeconds,
    /// so a split taken past its distance morphs in from its parent's shape. A tile drawn last
    /// frame that the selection replaced by a coarser tile **departs**: it keeps drawing with a
    /// floor rising from 0 to 1 over SphereTerrainDepartureSeconds, and the coarser tile waits
    /// until every tile departing under it has reached its parent's shape. A merge taken short of
    /// its distance therefore morphs out rather than stepping. A tile handed from one to the other
    /// mid-way keeps its floor, so neither direction steps. A departure is dropped, and the coarser
    /// tile drawn at once, when a departing tile or its parent is no longer resident.
    ///
    /// The morph runs one level, so a departing tile more than one level below the tile replacing
    /// it reaches its own parent's shape, and the level between steps when the coarser tile draws.
    /// @param state     The source's memory, advanced by @p delta.
    /// @param selected  The selection's drawn tiles this frame.
    /// @param delta     The frame's delta in seconds.
    /// @param tiles     What the pool holds, which a departing tile's layers are read from.
    [[nodiscard]] VE_API SphereTerrainTransitionFrame AdvanceSphereTerrainTransitions(
        SphereTerrainTransitionState& state, std::span<const SphereTerrainDrawnTile> selected,
        f32 delta, const SphereTerrainTileOracle& tiles);
}
