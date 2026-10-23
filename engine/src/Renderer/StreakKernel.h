#pragma once

#include <Veng/Veng.h>

#include <algorithm>
#include <cmath>
#include <span>

#include "BloomMips.h"

namespace Veng::Renderer
{
    /// @brief The shortest reach a channel is given, as a share of the requested length, so a large
    ///        chromatic spread never collapses a channel to nothing.
    inline constexpr f32 StreakMinChannelShare = 0.1f;

    /// @brief The streak buffer's extent for a frame: a quarter of its width and half its height.
    ///
    /// Floor-divided and at least one texel each way. The horizontal reduction is what the row
    /// filter runs along; the vertical one keeps the line about two output pixels thick.
    /// @param sceneExtent The frame's extent, in pixels.
    /// @return The streak buffer's extent, in texels.
    [[nodiscard]] inline uvec2 StreakExtent(const uvec2 sceneExtent)
    {
        return {std::max(sceneExtent.x / 4u, 1u), std::max(sceneExtent.y / 2u, 1u)};
    }

    /// @brief The sub-rect map a full-frame UV reads the streak buffer through.
    /// @param validScene The frame's valid extent this frame.
    /// @param allocScene The frame extent the streak buffer was allocated for.
    /// @return `(scale.xy, clamp.zw)`, applied as `min(uv * map.xy, map.zw)`.
    [[nodiscard]] inline vec4 StreakSampleMap(const uvec2 validScene, const uvec2 allocScene)
    {
        const MipSubRect sub =
            ComputeMipSubRect(StreakExtent(validScene), StreakExtent(allocScene), 0);
        return {sub.ScaleUV, sub.MaxUV};
    }

    /// @brief Each colour channel's weight ratio between neighbouring streak texels.
    ///
    /// A channel's weight at distance d from a source is `ratio^d`, so it falls to 1/e over its
    /// reach. Green reaches @p length of the frame's width; red and blue reach `1 − spread` of that
    /// (a positive spread shortens them, a negative one lengthens them), never under
    /// StreakMinChannelShare.
    /// @param length     The reach, as a share of the frame's width.
    /// @param spread     The chromatic spread.
    /// @param destWidth  The streak buffer's valid width, in texels.
    /// @return The three channels' ratios, each in (0, 1).
    [[nodiscard]] inline vec3 StreakChannelRatio(const f32 length, const f32 spread,
                                                 const u32 destWidth)
    {
        const f32 reach = std::max(length, 1.0e-3f) * static_cast<f32>(std::max(destWidth, 1u));
        const f32 side = std::max(1.0f - spread, StreakMinChannelShare);
        return {std::exp(-1.0f / (reach * side)), std::exp(-1.0f / reach),
                std::exp(-1.0f / (reach * side))};
    }

    /// @brief One channel of one row through the streak's filter, in place.
    ///
    /// Each texel becomes `Σ_d ratio^|d| · row[x − d]` normalised by the kernel's sum over an
    /// unbounded row, `(1 + ratio) / (1 − ratio)`, taken as one forward and one backward recursive
    /// sweep; past the row's ends is zero, so light leaves there. Both sweeps are kept as running
    /// averages — each sum times `1 − ratio` — so neither exceeds the row's brightest texel, which is
    /// what lets the shader store the forward one at half precision. Mirrors
    /// anamorphic_streak_filter.comp, which runs the same sweeps on each channel with its own ratio.
    /// @param row     The row, overwritten with the result.
    /// @param ratio   The channel's ratio (StreakChannelRatio).
    /// @param scratch Holds the forward sweep; resized to the row.
    inline void StreakFilterRow(const std::span<f32> row, const f32 ratio, vector<f32>& scratch)
    {
        const usize width = row.size();
        scratch.resize(width);
        const f32 gain = 1.0f - ratio;
        f32 forward = 0.0f;
        for (usize x = 0; x < width; x++)
        {
            forward = gain * row[x] + ratio * forward;
            scratch[x] = forward;
        }
        f32 backward = 0.0f;
        for (usize x = width; x-- > 0;)
        {
            const f32 source = row[x];
            backward = gain * source + ratio * backward;
            row[x] = (scratch[x] + backward - gain * source) / (1.0f + ratio);
        }
    }
}
