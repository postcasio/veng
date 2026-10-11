#pragma once

#include <Veng/Veng.h>

/// @brief Pure, device-free temporal-AA jitter sequence and its projection.
///
/// Temporal anti-aliasing perturbs the projection by a sub-pixel offset each frame
/// so that, integrated over the sequence, every pixel is sampled across its area —
/// the supersampling TAA reconstructs. The offsets follow a low-discrepancy Halton
/// (2, 3) sequence, which fills the pixel more evenly than a random or grid pattern.
/// glm-only, no Context, so it unit-tests without an ICD.
namespace Veng::Renderer
{
    /// @brief Number of distinct jitter offsets before the sequence repeats.
    ///
    /// Eight phases give an even distribution without an over-long convergence tail on a
    /// static view.
    inline constexpr u32 TaaJitterSampleCount = 8;

    /// @brief One term of the radical-inverse Halton sequence in the given base.
    /// @param index  Sample index; index 0 is the degenerate 0.
    /// @param base   Halton base (2 and 3 give the canonical 2D sequence).
    /// @return The Halton value in [0, 1).
    [[nodiscard]] constexpr f32 HaltonValue(u32 index, const u32 base)
    {
        f32 fraction = 1.0f;
        f32 result = 0.0f;
        while (index > 0)
        {
            fraction /= static_cast<f32>(base);
            result += fraction * static_cast<f32>(index % base);
            index /= base;
        }
        return result;
    }

    /// @brief The mean of the Halton terms one jitter period draws in the given base.
    ///
    /// The period draws indices 1..TaaJitterSampleCount, whose base-2 terms average below one
    /// half; subtracting this mean rather than one half is what makes the period's offsets sum
    /// to zero, so the integrated image is not displaced.
    /// @param base  Halton base.
    /// @return The period's mean term.
    [[nodiscard]] constexpr f32 TaaJitterPeriodMean(const u32 base)
    {
        f32 sum = 0.0f;
        for (u32 index = 1; index <= TaaJitterSampleCount; ++index)
        {
            sum += HaltonValue(index, base);
        }
        return sum / static_cast<f32>(TaaJitterSampleCount);
    }

    /// @brief Returns the sub-pixel jitter offset for a frame, in pixels.
    ///
    /// Each component lies within half a pixel of the pixel centre, and the offsets of one full
    /// period sum to zero on both axes. A caller converts it to the NDC translation the
    /// projection carries with TaaJitterNdc.
    /// @param frameIndex  Monotonic frame counter; folded into the sequence period.
    /// @return The pixel-space offset, both components in (-0.5, 0.5).
    [[nodiscard]] inline vec2 TaaJitterOffset(const u64 frameIndex)
    {
        // One-based index into the period; Halton(0) is the degenerate 0.
        const u32 index = static_cast<u32>(frameIndex % TaaJitterSampleCount) + 1;
        return vec2(HaltonValue(index, 2) - TaaJitterPeriodMean(2),
                    HaltonValue(index, 3) - TaaJitterPeriodMean(3));
    }

    /// @brief Converts a pixel jitter over a rasterized extent into an NDC translation.
    ///
    /// NDC spans two units across the extent, so a pixel is 2/extent of it. Positive values move
    /// the image right and down, in the direction pixel coordinates grow under Vulkan's Y-down
    /// clip space.
    /// @param pixelOffset  The jitter in pixels (TaaJitterOffset).
    /// @param extent       The extent the scene rasterizes at; zero components yield zero.
    /// @return The NDC translation.
    [[nodiscard]] inline vec2 TaaJitterNdc(const vec2 pixelOffset, const uvec2 extent)
    {
        return {extent.x > 0 ? 2.0f * pixelOffset.x / static_cast<f32>(extent.x) : 0.0f,
                extent.y > 0 ? 2.0f * pixelOffset.y / static_cast<f32>(extent.y) : 0.0f};
    }

    /// @brief Applies an NDC translation to a projection (or a view-projection).
    ///
    /// The translation is left-multiplied, so a clip position (x, y, z, w) becomes
    /// (x + dx·w, y + dy·w, z, w): every projected point moves by exactly (dx, dy) in NDC whatever
    /// its depth, under a perspective and an orthographic projection alike, and depth is unchanged.
    /// @param projection  The unjittered projection or view-projection.
    /// @param jitterNdc   The NDC translation (TaaJitterNdc).
    /// @return The jittered matrix.
    [[nodiscard]] inline mat4 JitterProjection(const mat4& projection, const vec2 jitterNdc)
    {
        mat4 translation(1.0f);
        translation[3][0] = jitterNdc.x;
        translation[3][1] = jitterNdc.y;
        return translation * projection;
    }
}
