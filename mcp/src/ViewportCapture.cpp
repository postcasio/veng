#include "ViewportCapture.h"

#include <Veng/Capture/FrameCapture.h>
#include <Veng/Capture/Screenshot.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/Viewport.h>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <array>
#include <span>

namespace Veng::Mcp
{
    using Json = nlohmann::json;

    namespace
    {
        /// @brief The base64 alphabet (RFC 4648), indexed by a 6-bit group.
        constexpr std::array<char, 64> Base64Alphabet{
            'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P',
            'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f',
            'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v',
            'w', 'x', 'y', 'z', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '+', '/'};

        /// @brief Encodes bytes as standard base64 (padded), for the image content block.
        string Base64Encode(std::span<const u8> bytes)
        {
            string out;
            out.reserve(((bytes.size() + 2) / 3) * 4);

            usize i = 0;
            for (; i + 3 <= bytes.size(); i += 3)
            {
                const u32 triple = (static_cast<u32>(bytes[i]) << 16) |
                                   (static_cast<u32>(bytes[i + 1]) << 8) |
                                   static_cast<u32>(bytes[i + 2]);
                out.push_back(Base64Alphabet[(triple >> 18) & 0x3F]);
                out.push_back(Base64Alphabet[(triple >> 12) & 0x3F]);
                out.push_back(Base64Alphabet[(triple >> 6) & 0x3F]);
                out.push_back(Base64Alphabet[triple & 0x3F]);
            }

            const usize remaining = bytes.size() - i;
            if (remaining == 1)
            {
                const u32 triple = static_cast<u32>(bytes[i]) << 16;
                out.push_back(Base64Alphabet[(triple >> 18) & 0x3F]);
                out.push_back(Base64Alphabet[(triple >> 12) & 0x3F]);
                out.push_back('=');
                out.push_back('=');
            }
            else if (remaining == 2)
            {
                const u32 triple =
                    (static_cast<u32>(bytes[i]) << 16) | (static_cast<u32>(bytes[i + 1]) << 8);
                out.push_back(Base64Alphabet[(triple >> 18) & 0x3F]);
                out.push_back(Base64Alphabet[(triple >> 12) & 0x3F]);
                out.push_back(Base64Alphabet[(triple >> 6) & 0x3F]);
                out.push_back('=');
            }

            return out;
        }
    }

    Result<string> PictureContentBlocks(const Capture::RgbImage& image)
    {
        const Result<vector<u8>> png = Capture::EncodePng(image);
        if (!png)
        {
            return std::unexpected(png.error());
        }

        // An image block has no room for the pixel dimensions, so a text block carries them.
        return Json::array(
                   {Json{
                        {"type", "image"}, {"data", Base64Encode(*png)}, {"mimeType", "image/png"}},
                    Json{{"type", "text"},
                         {"text", Json{{"width", image.Width}, {"height", image.Height}}.dump()}}})
            .dump();
    }

    Result<string> CaptureViewportContentBlocks(const Renderer::Viewport& viewport)
    {
        const Ref<Renderer::ImageView> output = viewport.GetOutput();
        if (!output || !output->GetImage())
        {
            return std::unexpected(string("the viewport has no output image"));
        }

        const Result<Capture::RgbImage> picture = Capture::ReadImageRgb8(*output->GetImage());
        if (!picture)
        {
            return std::unexpected(
                fmt::format("the viewport output could not be encoded: {}", picture.error()));
        }
        return PictureContentBlocks(*picture);
    }

    Result<string> TakeFrameContentBlocks(Capture::FrameCapture& capture)
    {
        if (capture.IsPending())
        {
            capture.Cancel();
            return std::unexpected(string("no frame was composited since the capture was "
                                          "requested (is the window minimized?)"));
        }

        const Result<Capture::RgbImage> picture = capture.Take();
        if (!picture)
        {
            return std::unexpected(picture.error());
        }
        return PictureContentBlocks(*picture);
    }
}
