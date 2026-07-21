#include "ViewportCapture.h"

#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/Viewport.h>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <stb_image_write.h>

#include <glm/gtc/packing.hpp>

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

        /// @brief The sRGB opto-electronic transfer function (linear → sRGB-encoded), per channel.
        ///
        /// The IEC 61966-2-1 encoding: a linear toe below the knee, a 1/2.4 power curve above it.
        f32 LinearToSrgb(f32 linear)
        {
            const f32 c = glm::clamp(linear, 0.0f, 1.0f);
            return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
        }

        /// @brief Encodes an RGBA16F download to sRGB 8-bit RGB, dropping alpha.
        ///
        /// The viewport output is the tonemapped scene color, held **linear** display-referred (the
        /// tonemap pass returns linear; the swapchain composite applies the display transfer — the
        /// _SRGB store for an sRGB swapchain, the scRGB/PQ encode for HDR). A PNG is sRGB by
        /// convention, so this reproduces that display transfer: unpack each half-float channel,
        /// apply the sRGB OETF, and quantize to 8-bit — otherwise a linear-to-8-bit write reads far
        /// too dark (crushed mid-tones). Returns empty when the download is smaller than the pixel
        /// count implies (a partial or unexpected-format image).
        vector<u8> EncodeRgba16fToSrgb8(std::span<const u8> download, u32 width, u32 height)
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
                    const f32 encoded =
                        LinearToSrgb(glm::unpackHalf1x16(halves[pixel * 4 + channel]));
                    rgb8[pixel * 3 + channel] = static_cast<u8>(encoded * 255.0f + 0.5f);
                }
            }
            return rgb8;
        }
        /// @brief Reorders an 8-bit four-channel download to 8-bit RGB, dropping alpha.
        ///
        /// A swap chain's 8-bit formats are already display-encoded — the composite wrote through an
        /// _SRGB store, or the values are the encoded ones themselves — so the bytes transfer to a
        /// PNG unchanged and only the channel order differs. Returns empty on a short download.
        vector<u8> EncodeRgba8ToRgb8(std::span<const u8> download, u32 width, u32 height, bool bgra)
        {
            const usize pixelCount = static_cast<usize>(width) * height;
            if (download.size() < pixelCount * 4)
            {
                return {};
            }
            vector<u8> rgb8;
            rgb8.resize(pixelCount * 3);
            for (usize pixel = 0; pixel < pixelCount; ++pixel)
            {
                const u8 first = download[pixel * 4];
                const u8 second = download[pixel * 4 + 1];
                const u8 third = download[pixel * 4 + 2];
                rgb8[pixel * 3] = bgra ? third : first;
                rgb8[pixel * 3 + 1] = second;
                rgb8[pixel * 3 + 2] = bgra ? first : third;
            }
            return rgb8;
        }
    }

    Result<string> CaptureSwapChainContentBlocks(Renderer::Context& context)
    {
        if (!context.IsSwapChainCaptureSupported())
        {
            return std::unexpected(
                string("the presented frame cannot be captured: the surface did not grant "
                       "transfer-source usage on its swap chain images, or the run is headless "
                       "(no swap chain, and no UI overlay to capture)"));
        }

        const Ref<Renderer::Image> image = context.GetCurrentSwapChainImage();
        if (!image)
        {
            return std::unexpected(string("no presented swap chain image is available"));
        }
        const u32 width = image->GetWidth();
        const u32 height = image->GetHeight();
        const Renderer::Format format = image->GetFormat();

        const vector<u8> download = image->Download();
        vector<u8> rgb8;
        switch (format)
        {
        // An extended-linear (scRGB) swap chain holds the blended colour linear and unencoded,
        // exactly as a viewport output does, so it takes the same sRGB encode.
        case Renderer::Format::RGBA16Sfloat:
            rgb8 = EncodeRgba16fToSrgb8(download, width, height);
            break;
        case Renderer::Format::BGRA8Srgb:
            rgb8 = EncodeRgba8ToRgb8(download, width, height, true);
            break;
        case Renderer::Format::RGBA8Srgb:
        case Renderer::Format::RGBA8Unorm:
            rgb8 = EncodeRgba8ToRgb8(download, width, height, false);
            break;
        default:
            // An HDR10 swap chain is PQ-encoded against its own primaries; writing those bytes
            // into an sRGB PNG would produce a confidently wrong image, so it is refused rather
            // than guessed at.
            return std::unexpected(
                fmt::format("the presented frame is in a swap chain format this capture does "
                            "not encode ({})",
                            static_cast<u32>(format)));
        }
        if (rgb8.empty())
        {
            return std::unexpected(string("the presented frame could not be encoded"));
        }

        const vector<u8> png = EncodePng(width, height, rgb8);
        if (png.empty())
        {
            return std::unexpected(string("PNG encoding failed"));
        }

        return Json::array(
                   {Json{{"type", "image"}, {"data", Base64Encode(png)}, {"mimeType", "image/png"}},
                    Json{{"type", "text"},
                         {"text", Json{{"width", width}, {"height", height}}.dump()}}})
            .dump();
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
        const vector<u8> rgb8 = EncodeRgba16fToSrgb8(download, width, height);
        if (rgb8.empty())
        {
            return std::unexpected(string("the viewport output could not be encoded"));
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
