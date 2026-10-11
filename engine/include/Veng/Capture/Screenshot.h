#pragma once

#include <Veng/Veng.h>
#include <Veng/Path.h>
#include <Veng/Renderer/Types.h>
#include <Veng/Result.h>

#include <ctime>
#include <span>

/// @brief Turning a rendered image into a picture file: the readback, the display encode and the PNG.
///
/// The pieces every still capture shares — whether the image is a viewport's output, a composited
/// frame (Capture::FrameCapture) or anything else the renderer drew — and the naming a screenshot
/// saved for the user takes. Nothing here owns GPU state; a caller hands it an image.
namespace Veng::Renderer
{
    class Image;
}

namespace Veng::Capture
{
    /// @brief An 8-bit, sRGB-encoded RGB picture: rows top to bottom, three bytes per pixel, unpadded.
    struct RgbImage
    {
        /// @brief Width in pixels.
        u32 Width = 0;

        /// @brief Height in pixels.
        u32 Height = 0;

        /// @brief `Width * Height * 3` bytes, red first.
        vector<u8> Pixels;
    };

    /// @brief Encodes an image download as an sRGB 8-bit RGB picture, dropping alpha.
    ///
    /// The encode follows the format: `RGBA16Sfloat` is held linear (a viewport's output, an
    /// extended-linear swap chain), so each channel takes the sRGB transfer function — a PNG is sRGB
    /// by convention and a linear write reads far too dark; the 8-bit formats (`RGBA8Unorm`,
    /// `RGBA8Srgb`, `BGRA8Srgb`) already hold display-encoded values and only their channel order
    /// is changed. Any other format is refused rather than guessed at — a PQ-encoded
    /// HDR10 image written into an sRGB PNG would be confidently wrong.
    /// @param download  The image's bytes, tightly packed, as Renderer::Image::Download returns them.
    /// @param extent    The image's width and height in pixels.
    /// @param format    The image's format.
    /// @return The picture, or why it could not be encoded (an unhandled format, a short download).
    [[nodiscard]] VE_API Result<RgbImage> ConvertToRgb8(std::span<const u8> download, uvec2 extent,
                                                        Renderer::Format format);

    /// @brief Downloads @p image and encodes it with ConvertToRgb8.
    ///
    /// The download blocks until the copy completes and is ordered after every frame already
    /// submitted, so it reads whatever the last submitted frame wrote.
    /// @param image  The image to read; must carry ImageUsage::TransferSrc.
    /// @return The picture, or why it could not be read or encoded.
    [[nodiscard]] VE_API Result<RgbImage> ReadImageRgb8(Renderer::Image& image);

    /// @brief PNG-encodes a picture in memory.
    /// @param image  The picture to encode.
    /// @return The PNG file's bytes, or why the encode failed.
    [[nodiscard]] VE_API Result<vector<u8>> EncodePng(const RgbImage& image);

    /// @brief PNG-encodes a picture and writes it to @p file, replacing anything there.
    /// @param image  The picture to write.
    /// @param file   The file to write.
    /// @return Success, or why the encode or the write failed.
    [[nodiscard]] VE_API VoidResult WritePng(const RgbImage& image, const path& file);

    /// @brief Names a screenshot by the moment it was taken: `yyyy-mm-dd_hh-mm-ss`.
    /// @param local  The moment, already in local time.
    /// @return The name, with no extension and no directory.
    [[nodiscard]] VE_API string ScreenshotStem(const std::tm& local);

    /// @brief Claims a fresh `.png` file in @p directory named by @p local, and returns its path.
    ///
    /// The file is created empty and exclusively, so the name is the caller's even against another
    /// process saving into the same directory in the same second. A name already taken gains a
    /// counter — `<stem>_2.png`, `<stem>_3.png`, … — up to a bound past which the claim fails.
    /// @param directory  The directory to claim the file in; must exist.
    /// @param local      The moment the screenshot was taken, in local time.
    /// @return The claimed file's path, or why no file could be claimed.
    [[nodiscard]] VE_API Result<path> ReserveScreenshotPath(const path& directory,
                                                            const std::tm& local);
}
