#pragma once

#include <Veng/Veng.h>

#include <algorithm>
#include <limits>

/// @brief The device-free half of the depth-of-field battery: the tile reduction rule and the
///        two hard quality clamps.
///
/// The gather bounds its kernel with a coarse per-tile record reduced from the half-resolution
/// circle-of-confusion buffer. That reduction is pure arithmetic, so it lives here — the shader
/// mirrors these expressions and a test pins them with no device.
namespace Veng::Renderer
{
    /// @brief Upper bound on DofMaxCoc, in half-resolution pixels.
    ///
    /// The gather's kernel radius is bounded by this, so the per-frame value is clamped to it
    /// where it is pushed into the view state rather than trusted from an authored source.
    inline constexpr f32 MaxDofCoc = 32.0f;

    /// @brief Upper bound on DofRingCount.
    ///
    /// The ring count is a GPU loop bound. An unclamped value is an unbounded loop — a device
    /// hang rather than a recoverable error — so it is clamped where it is pushed into the view
    /// state, and the gather shader ceilings it a second time with a compile-time constant.
    inline constexpr u32 MaxDofRings = 8;

    /// @brief Edge length, in half-resolution pixels, of one depth-of-field tile.
    inline constexpr u32 DofTileSize = 8;

    /// @brief The coarse per-tile record the gather reads to bound its kernel.
    ///
    /// One record per DofTileSize x DofTileSize block of the half-resolution circle-of-confusion
    /// buffer, dilated one tile ring so a gather also sees the near-field spill its neighbours
    /// advertise.
    struct DofTile
    {
        /// @brief Smallest view-space depth in the tile, in metres.
        f32 MinDepth = 0.0f;
        /// @brief Largest near-field (negative-signed) circle of confusion, in half-res pixels.
        f32 MaxNearCoc = 0.0f;
        /// @brief Largest far-field (positive-signed) circle of confusion, in half-res pixels.
        f32 MaxFarCoc = 0.0f;
    };

    /// @brief The identity record a tile reduction starts from.
    ///
    /// Its depth is the largest representable float, so the first accumulated sample wins the
    /// minimum; both radii start at zero, the additive identity of a maximum over non-negative
    /// magnitudes.
    /// @return The reduction identity.
    [[nodiscard]] inline DofTile EmptyDofTile()
    {
        return DofTile{
            .MinDepth = std::numeric_limits<f32>::max(),
            .MaxNearCoc = 0.0f,
            .MaxFarCoc = 0.0f,
        };
    }

    /// @brief Folds one circle-of-confusion sample into a tile record.
    ///
    /// The sign of the sample selects the field it contributes to: negative is near (in front of
    /// the focus plane) and positive is far, and each side keeps the largest magnitude seen. A
    /// non-positive depth carries no defocus and contributes nothing at all, so a cleared
    /// background texel cannot pull the tile's minimum depth to zero.
    /// @param tile       The record so far.
    /// @param signedCoc  The sample's signed circle of confusion, in half-resolution pixels.
    /// @param depth      The sample's view-space depth in metres.
    /// @return The record including the sample.
    [[nodiscard]] inline DofTile AccumulateDofTile(const DofTile& tile, const f32 signedCoc,
                                                   const f32 depth)
    {
        if (depth <= 0.0f)
        {
            return tile;
        }
        return DofTile{
            .MinDepth = std::min(tile.MinDepth, depth),
            .MaxNearCoc = std::max(tile.MaxNearCoc, std::max(-signedCoc, 0.0f)),
            .MaxFarCoc = std::max(tile.MaxFarCoc, std::max(signedCoc, 0.0f)),
        };
    }

    /// @brief Merges two tile records — the dilation step's combine.
    ///
    /// Associative and commutative, so a tile may be dilated against its neighbours in any order.
    /// @param a  One record.
    /// @param b  The other record.
    /// @return The record covering both.
    [[nodiscard]] inline DofTile MergeDofTiles(const DofTile& a, const DofTile& b)
    {
        return DofTile{
            .MinDepth = std::min(a.MinDepth, b.MinDepth),
            .MaxNearCoc = std::max(a.MaxNearCoc, b.MaxNearCoc),
            .MaxFarCoc = std::max(a.MaxFarCoc, b.MaxFarCoc),
        };
    }

    /// @brief Clamps an authored maximum circle of confusion into the supported range.
    /// @param maxCoc  The authored radius in half-resolution pixels.
    /// @return The value clamped to [0, MaxDofCoc]; a non-finite input resolves to zero.
    [[nodiscard]] inline f32 ClampDofMaxCoc(const f32 maxCoc)
    {
        if (!(maxCoc > 0.0f))
        {
            return 0.0f;
        }
        return std::min(maxCoc, MaxDofCoc);
    }

    /// @brief Clamps an authored gather ring count into the supported range.
    /// @param rings  The authored ring count.
    /// @return The value clamped to [1, MaxDofRings].
    [[nodiscard]] inline u32 ClampDofRingCount(const u32 rings)
    {
        return std::clamp(rings, 1u, MaxDofRings);
    }
}
