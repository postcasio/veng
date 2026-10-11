#pragma once

#include <Veng/Veng.h>

#include <algorithm>

namespace Veng::Renderer
{
    /// @brief How many levels above the bloom pyramid's coarsest the lens-dirt term reads.
    ///
    /// Scatter off a dirty lens spreads a bright source's light across a broad patch of the glass,
    /// so the dirt is lit by a wide, smooth bloom rather than by the full-detail accumulated mip 0,
    /// whose tight core would reveal the dirt only in a halo hugging the source. The pyramid's
    /// coarsest level spans about the same fraction of the frame at every extent (BloomMipCount),
    /// so counting from it keeps the dirt's reach a fixed fraction of the frame too: three levels
    /// up is a sixteenth of the frame's resolution, 120×67 texels for a 1920×1080 frame. The level
    /// after the up-sweep holds every coarser level's glow as well, so it carries about the same
    /// energy as mip 0, spread wider.
    inline constexpr u32 LensDirtLevelsAboveCoarsest = 3;

    /// @brief The bloom pyramid level the lens-dirt term samples, for a pyramid of @p mipCount levels.
    /// @param mipCount The pyramid's level count (BloomMipCount); at least 1.
    /// @return The level, LensDirtLevelsAboveCoarsest above the coarsest, never below 0.
    [[nodiscard]] inline u32 LensDirtBloomLevel(const u32 mipCount)
    {
        return mipCount > LensDirtLevelsAboveCoarsest + 1
                   ? mipCount - 1 - LensDirtLevelsAboveCoarsest
                   : 0u;
    }

    /// @brief The cover fit of a lens-dirt mask over a frame: `maskUv = frameUv * xy + zw`.
    ///
    /// The mask is scaled uniformly until it covers the whole frame and centred, so its aspect is
    /// kept, one axis spans it exactly and the other is cropped evenly at both ends — never
    /// stretched, never letterboxed. Computed over the presented (post-resolve) extent, so it is
    /// independent of the render scale and fixed on screen.
    /// @param frame The frame's extent in pixels; a zero axis is treated as one.
    /// @param mask  The mask's extent in texels; a zero axis is treated as one.
    /// @return `(scale.xy, offset.zw)`: the scale is at most 1 on each axis and exactly 1 on one.
    [[nodiscard]] inline vec4 ComputeLensDirtCoverMap(const uvec2 frame, const uvec2 mask)
    {
        const f32 frameAspect =
            static_cast<f32>(std::max(frame.x, 1u)) / static_cast<f32>(std::max(frame.y, 1u));
        const f32 maskAspect =
            static_cast<f32>(std::max(mask.x, 1u)) / static_cast<f32>(std::max(mask.y, 1u));
        // A frame wider than the mask spans the mask's width and crops its height, and the reverse.
        const vec2 scale = frameAspect >= maskAspect ? vec2(1.0f, maskAspect / frameAspect)
                                                     : vec2(frameAspect / maskAspect, 1.0f);
        const vec2 offset = 0.5f - 0.5f * scale;
        return {scale, offset};
    }
}
