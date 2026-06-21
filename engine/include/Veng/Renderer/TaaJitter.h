#pragma once

#include <Veng/Veng.h>

/// @brief Pure, device-free temporal-AA jitter sequence.
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
    /// A power-of-two-free Halton period; eight phases give a smooth distribution
    /// without an over-long convergence tail on a static view.
    inline constexpr u32 TaaJitterSampleCount = 8;

    /// @brief One term of the radical-inverse Halton sequence in the given base.
    /// @param index  One-based sample index.
    /// @param base   Halton base (2 and 3 give the canonical 2D sequence).
    /// @return The Halton value in [0, 1).
    [[nodiscard]] inline f32 HaltonValue(u32 index, u32 base)
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

    /// @brief Returns the sub-pixel jitter offset for a frame, in pixels.
    ///
    /// The offset is centered on the pixel: each component lies in [-0.5, 0.5). A
    /// caller scales it into a clip-space projection shear by 2/extent.
    /// @param frameIndex  Monotonic frame counter; folded into the sequence period.
    /// @return The pixel-space offset, both components in [-0.5, 0.5).
    [[nodiscard]] inline vec2 TaaJitterOffset(u64 frameIndex)
    {
        // One-based index into the period; Halton(0) is the degenerate 0.
        const u32 index = static_cast<u32>(frameIndex % TaaJitterSampleCount) + 1;
        return vec2(HaltonValue(index, 2) - 0.5f, HaltonValue(index, 3) - 0.5f);
    }
}
