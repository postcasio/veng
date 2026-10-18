#pragma once

#include <Veng/Mcp/Export.h>
#include <Veng/Veng.h>
#include <Veng/Result.h>

namespace Veng::Capture
{
    class FrameCapture;
    struct RgbImage;
}

namespace Veng::Renderer
{
    class Viewport;
    struct TexelReadback;
}

namespace Veng::Mcp
{
    /// @brief Encodes a picture as an MCP image content-block array (JSON string).
    ///
    /// PNG-encodes it (Capture::EncodePng), base64-encodes that, and returns the two-element
    /// `content` array — an `image` block plus a `text` block carrying the pixel dimensions — a
    /// ReturnsContentBlocks tool hands back.
    /// @param image  The picture to encode.
    /// @return The content-array JSON string, or the PNG encode's error.
    Result<string> PictureContentBlocks(const Capture::RgbImage& image);

    /// @brief Captures a viewport's rendered output as an MCP image content-block array.
    ///
    /// The path behind both render.screenshot and the editor's editor.screenshot_panel: it reads the
    /// viewport's output image back (Capture::ReadImageRgb8, which encodes its linear half-float to
    /// sRGB) and hands the picture to PictureContentBlocks. Runs on the render thread (the download
    /// blocks in lockstep with the frame), so the caller must invoke it at the Pump() point.
    /// @param viewport  The viewport to capture; must have an output image.
    /// @return The content-array JSON string on success, or a located error (no output image, an
    ///         unencodable download, or a PNG-encode failure).
    VE_MCP_API Result<string> CaptureViewportContentBlocks(const Renderer::Viewport& viewport);

    /// @brief Takes a composited frame off @p capture as an MCP image content-block array.
    ///
    /// The read half of render.screenshot_window: the request was made a pump earlier
    /// (Capture::FrameCapture::Request), so the frame it asked for has ended by now. A capture still
    /// pending — a window with no presentable size composites nothing — is cancelled and reported, so
    /// a failed call leaves no request open.
    /// @param capture  The application's frame capture, holding a request made at the previous pump.
    /// @return The content-array JSON string on success, or why there is no frame to return.
    Result<string> TakeFrameContentBlocks(Capture::FrameCapture& capture);

    /// @brief How render.export_texture encodes the texels it read back.
    enum class TexelEncoding : u8
    {
        /// @brief Radiance RGBE (`.hdr`): linear float RGB, about 1 % precision, alpha dropped.
        RadianceHdr,
        /// @brief 8-bit PNG: the stored values clamped to [0, 1], sRGB-encoded only where the
        ///        source format was sRGB, so an 8-bit texture round-trips its bytes.
        Png,
    };

    /// @brief Encodes a readback as an MCP image content-block array, its layers side by side.
    ///
    /// The layers are laid out left to right in layer order — for a cube +X, −X, +Y, −Y, +Z, −Z —
    /// so the picture is `Extent.x × Layers` wide.
    /// @param readback  The decoded texels.
    /// @param encoding  The file encoding.
    /// @param text      The text block's content (the tool's description of what it read).
    /// @return The content-array JSON string, or the encode's error.
    Result<string> TexelContentBlocks(const Renderer::TexelReadback& readback,
                                      TexelEncoding encoding, const string& text);
}
