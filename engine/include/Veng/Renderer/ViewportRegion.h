#pragma once

#include <Veng/Veng.h>

namespace Veng::Renderer
{
    /// @brief A viewport's rectangle in window framebuffer pixels.
    ///
    /// The Extent is the render resolution; the Offset is where a Presented viewport is
    /// placed in the window (and the origin an Offscreen panel viewport's picking maps from).
    /// The gather pass uses it as both the scissor and the viewport for a placement's blit.
    struct ViewportRegion
    {
        /// @brief Top-left placement in window framebuffer pixels.
        ivec2 Offset = {};
        /// @brief Render resolution in window framebuffer pixels.
        uvec2 Extent = {};
    };
}
