#include "ViewportCapture.h"

#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/Viewport.h>

#include <nlohmann/json.hpp>

#include <stb_image_write.h>

#include <glm/gtc/packing.hpp>

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

        /// @brief PNG-encodes an 8-bit RGB image via stb_image_write into a byte vector.
        ///
        /// stbi_write_png_to_func appends each written chunk into the collected buffer, so the
        /// whole encode lands in memory rather than on disk. Returns empty on an encode failure.
        vector<u8> EncodePng(u32 width, u32 height, std::span<const u8> rgb8)
        {
            vector<u8> encoded;
            const auto sink = [](void* context, void* data, int size)
            {
                auto& out = *static_cast<vector<u8>*>(context);
                const auto* bytes = static_cast<const u8*>(data);
                out.insert(out.end(), bytes, bytes + size);
            };

            const int rowStride = static_cast<int>(width) * 3;
            const int ok =
                stbi_write_png_to_func(sink, &encoded, static_cast<int>(width),
                                       static_cast<int>(height), 3, rgb8.data(), rowStride);
            if (ok == 0)
            {
                encoded.clear();
            }
            return encoded;
        }

        /// @brief Tonemaps an RGBA16F download to 8-bit RGB, dropping alpha.
        ///
        /// Matches the smoke capture: unpack each half-float channel, clamp to [0,1], and scale to
        /// 8-bit — the tonemapped scene-color output, not a raw HDR dump. Returns empty when the
        /// download is smaller than the pixel count implies (a partial or unexpected-format image).
        vector<u8> TonemapRgba16fToRgb8(std::span<const u8> download, u32 width, u32 height)
        {
            const usize pixelCount = static_cast<usize>(width) * height;
            if (download.size() < pixelCount * 4 * sizeof(u16))
            {
                return {};
            }

            const auto* halves = reinterpret_cast<const u16*>(download.data());
            vector<u8> rgb8;
            rgb8.resize(pixelCount * 3);
            for (usize pixel = 0; pixel < pixelCount; ++pixel)
            {
                for (u32 channel = 0; channel < 3; ++channel)
                {
                    const f32 value =
                        glm::clamp(glm::unpackHalf1x16(halves[pixel * 4 + channel]), 0.0f, 1.0f);
                    rgb8[pixel * 3 + channel] = static_cast<u8>(value * 255.0f + 0.5f);
                }
            }
            return rgb8;
        }
    }

    Result<string> CaptureViewportContentBlocks(Renderer::Viewport& viewport)
    {
        const Ref<Renderer::ImageView> output = viewport.GetOutput();
        if (!output || !output->GetImage())
        {
            return std::unexpected(string("the viewport has no output image"));
        }
        const Ref<Renderer::Image> image = output->GetImage();
        const u32 width = image->GetWidth();
        const u32 height = image->GetHeight();

        const vector<u8> download = image->Download();
        const vector<u8> rgb8 = TonemapRgba16fToRgb8(download, width, height);
        if (rgb8.empty())
        {
            return std::unexpected(string("the viewport output could not be tonemapped"));
        }

        const vector<u8> png = EncodePng(width, height, rgb8);
        if (png.empty())
        {
            return std::unexpected(string("PNG encoding failed"));
        }

        // The content array a ReturnsContentBlocks tool returns: the image block plus a text block
        // carrying the pixel dimensions (an image block has no room for them).
        return Json::array(
                   {Json{{"type", "image"}, {"data", Base64Encode(png)}, {"mimeType", "image/png"}},
                    Json{{"type", "text"},
                         {"text", Json{{"width", width}, {"height", height}}.dump()}}})
            .dump();
    }
}
