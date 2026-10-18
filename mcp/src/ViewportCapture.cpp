#include "ViewportCapture.h"

#include <Veng/Capture/FrameCapture.h>
#include <Veng/Capture/Screenshot.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/TextureReadback.h>
#include <Veng/Renderer/Viewport.h>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
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

        /// @brief Encodes linear RGB as one RGBE texel (Ward's shared-exponent form).
        std::array<u8, 4> ToRgbe(const vec3 rgb)
        {
            // A negative or non-finite channel carries no radiance the format can hold.
            const vec3 clean{std::isfinite(rgb.r) ? std::max(rgb.r, 0.0f) : 0.0f,
                             std::isfinite(rgb.g) ? std::max(rgb.g, 0.0f) : 0.0f,
                             std::isfinite(rgb.b) ? std::max(rgb.b, 0.0f) : 0.0f};
            const f32 largest = std::max({clean.r, clean.g, clean.b});
            if (largest < 1e-32f)
            {
                return {0, 0, 0, 0};
            }
            int exponent = 0;
            const f32 scale = std::frexp(largest, &exponent) * 256.0f / largest;
            return {static_cast<u8>(clean.r * scale), static_cast<u8>(clean.g * scale),
                    static_cast<u8>(clean.b * scale), static_cast<u8>(exponent + 128)};
        }

        /// @brief Encodes the layers side by side as a flat (uncompressed) Radiance file.
        vector<u8> EncodeRadianceHdr(const Renderer::TexelReadback& readback)
        {
            const u32 width = readback.Extent.x * readback.Layers;
            const u32 height = readback.Extent.y;
            const string header =
                fmt::format("#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y {} +X {}\n", height, width);
            vector<u8> out(header.begin(), header.end());
            out.reserve(out.size() + static_cast<usize>(width) * height * 4);
            for (u32 y = 0; y < height; ++y)
            {
                for (u32 layer = 0; layer < readback.Layers; ++layer)
                {
                    const usize row =
                        (static_cast<usize>(layer) * readback.Extent.y + y) * readback.Extent.x;
                    for (u32 x = 0; x < readback.Extent.x; ++x)
                    {
                        const std::array<u8, 4> texel = ToRgbe(vec3(readback.Texels[row + x]));
                        out.insert(out.end(), texel.begin(), texel.end());
                    }
                }
            }
            return out;
        }

        /// @brief The sRGB transfer of one linear channel, clamped to [0, 1].
        f32 LinearToSrgb(const f32 linear)
        {
            const f32 v = std::clamp(linear, 0.0f, 1.0f);
            return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
        }

        /// @brief Lays the layers side by side as an 8-bit picture.
        Capture::RgbImage ToRgbImage(const Renderer::TexelReadback& readback)
        {
            const bool srgb = Renderer::IsSrgbFormat(readback.SourceFormat);
            Capture::RgbImage image{.Width = readback.Extent.x * readback.Layers,
                                    .Height = readback.Extent.y};
            image.Pixels.resize(static_cast<usize>(image.Width) * image.Height * 3);
            usize out = 0;
            for (u32 y = 0; y < readback.Extent.y; ++y)
            {
                for (u32 layer = 0; layer < readback.Layers; ++layer)
                {
                    const usize row =
                        (static_cast<usize>(layer) * readback.Extent.y + y) * readback.Extent.x;
                    for (u32 x = 0; x < readback.Extent.x; ++x)
                    {
                        const vec4 texel = readback.Texels[row + x];
                        for (u32 channel = 0; channel < 3; ++channel)
                        {
                            const f32 value = srgb ? LinearToSrgb(texel[channel])
                                                   : std::clamp(texel[channel], 0.0f, 1.0f);
                            image.Pixels[out++] = static_cast<u8>(std::lround(value * 255.0f));
                        }
                    }
                }
            }
            return image;
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

    Result<string> TexelContentBlocks(const Renderer::TexelReadback& readback,
                                      const TexelEncoding encoding, const string& text)
    {
        vector<u8> bytes;
        string mimeType;
        if (encoding == TexelEncoding::RadianceHdr)
        {
            bytes = EncodeRadianceHdr(readback);
            mimeType = "image/vnd.radiance";
        }
        else
        {
            Result<vector<u8>> png = Capture::EncodePng(ToRgbImage(readback));
            if (!png)
            {
                return std::unexpected(png.error());
            }
            bytes = std::move(*png);
            mimeType = "image/png";
        }
        return Json::array(
                   {Json{{"type", "image"}, {"data", Base64Encode(bytes)}, {"mimeType", mimeType}},
                    Json{{"type", "text"}, {"text", text}}})
            .dump();
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
