#pragma once

#include <Veng/Veng.h>
#include <Veng/Result.h>

namespace Veng::Renderer
{
    class Viewport;
}

namespace Veng::Mcp
{
    /// @brief Captures a viewport's rendered output as an MCP image content-block array (JSON string).
    ///
    /// The shared Download() -> tonemap -> PNG -> base64 path behind both render.screenshot and
    /// the editor's editor.screenshot_panel: it downloads the viewport's output image, tonemaps the
    /// RGBA16F scene color to 8-bit RGB (dropping alpha), PNG-encodes it, base64-encodes that, and
    /// returns the two-element `content` array (an `image` block plus a `text` block carrying the
    /// pixel dimensions) a ReturnsContentBlocks tool hands back. Runs on the render thread (a
    /// Download() blocks in lockstep with the frame), so the caller must invoke it at the Pump()
    /// point.
    /// @param viewport  The viewport to capture; must have an output image.
    /// @return The content-array JSON string on success, or a located error (no output image, an
    ///         unexpected download size, or a PNG-encode failure).
    Result<string> CaptureViewportContentBlocks(Renderer::Viewport& viewport);
}
