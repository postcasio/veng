#pragma once

// A blit that scales: Metal 4's compute-encoder copies neither filter nor scale, so a blit between
// regions of different extents is a backend-owned render pass sampling the source with a linear
// filter. And a clear confined to a render area: a Metal load action clears the whole attachment,
// so a pass clearing part of one loads it and draws the clear over its area. Defined in Blit.mm.

#include <array>

#import <Metal/Metal.h>

#include <Veng/Renderer/CommandBuffer.h>

namespace Veng::Renderer
{
    class Context;
}

namespace Veng::Renderer::Metal
{
    /// @brief Records a scaled, linearly filtered blit, one render pass per layer.
    ///
    /// The source is sampled at each destination texel's centre mapped into the source region, as
    /// Vulkan's linear blit does; a region whose end precedes its start mirrors the copy.
    /// @param context The owning context, which holds the blit pipelines.
    /// @param cmd     The command buffer, outside a render pass.
    /// @param info    The blit; both images two-dimensional, colour and floating-point readable.
    void BlitScaled(Context& context, const CommandBuffer& cmd, const BlitImageInfo& info);

    /// @brief The most colour attachments a render pass binds.
    inline constexpr u32 MaxAreaClearAttachments = 8;

    /// @brief The attachments a render pass clears over its render area alone.
    struct AreaClear
    {
        /// @brief Each colour slot's pixel format; MTLPixelFormatInvalid where the slot is unbound.
        std::array<MTLPixelFormat, MaxAreaClearAttachments> ColorFormats{};
        /// @brief The colour slots to clear, one bit each.
        u32 ColorMask = 0;
        /// @brief Each cleared colour slot's value.
        std::array<vec4, MaxAreaClearAttachments> Colors{};
        /// @brief Whether the depth attachment is cleared.
        bool ClearsDepth = false;
        /// @brief The depth it is cleared to.
        f32 Depth = 0.0f;
        /// @brief The attachments' sample count.
        NSUInteger SampleCount = 1;

        /// @brief Whether anything is cleared.
        [[nodiscard]] bool IsNeeded() const { return ColorMask != 0 || ClearsDepth; }
    };

    /// @brief Draws @p clear over the open render pass's viewport and scissor, then restores the
    ///        stage's own argument table.
    ///
    /// Vulkan clears only a pass's render area; the pass loads the attachments this clears.
    /// @param context The owning context, which holds the clear pipelines.
    /// @param cmd     The command buffer, inside the render pass.
    /// @param clear   What to clear.
    void ClearRenderArea(Context& context, const CommandBuffer& cmd, const AreaClear& clear);
}
