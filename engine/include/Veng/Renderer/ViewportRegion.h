#pragma once

#include <Veng/Veng.h>

namespace Veng::Renderer
{
    /// @brief A viewport's placement rectangle in window framebuffer pixels.
    ///
    /// The Extent is the on-screen placement size, not the render resolution: a viewport
    /// renders at Extent scaled by its RenderScale (see Viewport), so a reduced scale renders
    /// below the region and the compositor upscales. The Offset is where a Presented viewport
    /// is placed in the window (and the origin an Offscreen panel viewport's picking maps from).
    /// The gather pass uses the region as both the scissor and the viewport for a placement's blit.
    struct ViewportRegion
    {
        /// @brief Top-left placement in window framebuffer pixels.
        ivec2 Offset = {};
        /// @brief On-screen placement size in window framebuffer pixels.
        uvec2 Extent = {};
    };
}
