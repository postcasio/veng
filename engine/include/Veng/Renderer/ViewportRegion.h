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

    /// @brief A window-relative placement in normalized fractions ([0,1]), resolved to pixels per resize.
    ///
    /// Where a window-tracking viewport sits, expressed as fractions of the render extent the
    /// ViewportCompositor resolves to a pixel ViewportRegion (round(Layout · render extent)) at
    /// registration and on every swapchain resize. The default is the full window, so a viewport
    /// with the default Layout covers the whole window exactly as a viewport given that full region
    /// directly. Two quadrant Layouts give resize-stable split-screen with no per-frame re-apply.
    struct ViewportLayout
    {
        /// @brief Top-left, as a fraction of the render extent ([0,1]).
        vec2 Offset = {0.0f, 0.0f};
        /// @brief Size, as a fraction of the render extent ([0,1]); full window by default.
        vec2 Extent = {1.0f, 1.0f};
    };
}
