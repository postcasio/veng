#pragma once

#include <Veng/Veng.h>
#include <Veng/Result.h>

namespace Veng::Renderer
{
    class Context;
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

    /// @brief Captures the presented frame off the swap chain as an MCP image content-block array.
    ///
    /// The counterpart of CaptureViewportContentBlocks for the *finished* frame: a viewport carries
    /// scene color alone, while the composite writes the scene and the UI overlay together straight
    /// into the swap chain, so a presented image is the only surface holding both. It downloads the
    /// last presented image and encodes it to 8-bit RGB, reading its display encoding off the swap
    /// chain format rather than assuming the viewport's linear half-float.
    ///
    /// Runs on the render thread (the Download() blocks in lockstep with the frame), so the caller
    /// must invoke it at the Pump() point — where the index still names the last *presented* image
    /// rather than one acquired for a frame not yet drawn.
    /// @param context  The render context whose swap chain is captured.
    /// @return The content-array JSON string on success, or a located error (capture unsupported,
    ///         an unhandled swap chain format, an unexpected download size, or a PNG-encode failure).
    Result<string> CaptureSwapChainContentBlocks(Renderer::Context& context);
}
