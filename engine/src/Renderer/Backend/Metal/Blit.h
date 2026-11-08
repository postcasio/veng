#pragma once

// A blit that scales: Metal 4's compute-encoder copies neither filter nor scale, so a blit between
// regions of different extents is a backend-owned render pass sampling the source with a linear
// filter. Defined in Blit.mm.

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
}
