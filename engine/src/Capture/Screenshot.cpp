#include <Veng/Capture/Screenshot.h>

#include <Veng/Renderer/Image.h>

#include <fmt/format.h>

#include <stb_image_write.h>

#include <glm/gtc/packing.hpp>

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace Veng::Capture
{
    namespace
    {
        /// @brief The most counter suffixes ReserveScreenshotPath tries before giving up.
        constexpr u32 MaxScreenshotSuffix = 999;

        /// @brief The sRGB opto-electronic transfer function (linear → sRGB-encoded), per channel.
        f32 LinearToSrgb(const f32 linear)
        {
            const f32 c = glm::clamp(linear, 0.0f, 1.0f);
            return c <= 0.0031308f ? c * 12.92f : (1.055f * std::pow(c, 1.0f / 2.4f)) - 0.055f;
        }

        /// @brief Encodes a linear RGBA16F download to sRGB 8-bit RGB.
        void EncodeRgba16f(std::span<const u8> download, const usize pixelCount, vector<u8>& rgb8)
        {
            const auto* halves = reinterpret_cast<const u16*>(download.data());
            for (usize pixel = 0; pixel < pixelCount; ++pixel)
            {
                for (usize channel = 0; channel < 3; ++channel)
                {
                    const f32 encoded =
                        LinearToSrgb(glm::unpackHalf1x16(halves[(pixel * 4) + channel]));
                    rgb8[(pixel * 3) + channel] = static_cast<u8>((encoded * 255.0f) + 0.5f);
                }
            }
        }

        /// @brief Reorders an 8-bit four-channel download to RGB, dropping alpha.
        void EncodeRgba8(std::span<const u8> download, const usize pixelCount, const bool bgra,
                         vector<u8>& rgb8)
        {
            for (usize pixel = 0; pixel < pixelCount; ++pixel)
            {
                const u8 first = download[pixel * 4];
                const u8 second = download[(pixel * 4) + 1];
                const u8 third = download[(pixel * 4) + 2];
                rgb8[pixel * 3] = bgra ? third : first;
                rgb8[(pixel * 3) + 1] = second;
                rgb8[(pixel * 3) + 2] = bgra ? first : third;
            }
        }

        /// @brief Returns the bytes per texel of a format ConvertToRgb8 encodes, or 0 for any other.
        usize EncodableTexelBytes(const Renderer::Format format)
        {
            switch (format)
            {
            case Renderer::Format::RGBA16Sfloat:
                return 8;
            case Renderer::Format::RGBA8Unorm:
            case Renderer::Format::RGBA8Srgb:
            case Renderer::Format::BGRA8Srgb:
                return 4;
            default:
                return 0;
            }
        }
    }

    Result<RgbImage> ConvertToRgb8(std::span<const u8> download, const uvec2 extent,
                                   const Renderer::Format format)
    {
        const usize texelBytes = EncodableTexelBytes(format);
        if (texelBytes == 0)
        {
            return std::unexpected(
                fmt::format("the image is in a format a picture is not encoded from ({})",
                            static_cast<u32>(format)));
        }

        const usize pixelCount = static_cast<usize>(extent.x) * extent.y;
        if (pixelCount == 0)
        {
            return std::unexpected(string("the image is empty"));
        }
        if (download.size() < pixelCount * texelBytes)
        {
            return std::unexpected(
                fmt::format("the download holds {} bytes, short of the {} a {}x{} image needs",
                            download.size(), pixelCount * texelBytes, extent.x, extent.y));
        }

        RgbImage image{.Width = extent.x, .Height = extent.y};
        image.Pixels.resize(pixelCount * 3);
        if (format == Renderer::Format::RGBA16Sfloat)
        {
            EncodeRgba16f(download, pixelCount, image.Pixels);
        }
        else
        {
            EncodeRgba8(download, pixelCount, format == Renderer::Format::BGRA8Srgb, image.Pixels);
        }
        return image;
    }

    Result<RgbImage> ReadImageRgb8(Renderer::Image& image)
    {
        const vector<u8> download = image.Download();
        return ConvertToRgb8(download, uvec2(image.GetWidth(), image.GetHeight()),
                             image.GetFormat());
    }

    Result<vector<u8>> EncodePng(const RgbImage& image)
    {
        if (image.Width == 0 || image.Height == 0 ||
            image.Pixels.size() < static_cast<usize>(image.Width) * image.Height * 3)
        {
            return std::unexpected(string("the picture is empty or short of its pixels"));
        }

        vector<u8> encoded;
        const auto sink = [](void* context, void* data, const int size)
        {
            auto& out = *static_cast<vector<u8>*>(context);
            const auto* bytes = static_cast<const u8*>(data);
            out.insert(out.end(), bytes, bytes + size);
        };

        const int ok = stbi_write_png_to_func(
            sink, &encoded, static_cast<int>(image.Width), static_cast<int>(image.Height), 3,
            image.Pixels.data(), static_cast<int>(image.Width) * 3);
        if (ok == 0)
        {
            return std::unexpected(string("PNG encoding failed"));
        }
        return encoded;
    }

    VoidResult WritePng(const RgbImage& image, const path& file)
    {
        const Result<vector<u8>> png = EncodePng(image);
        if (!png)
        {
            return std::unexpected(png.error());
        }

        std::FILE* const out = std::fopen(file.string().c_str(), "wb");
        if (out == nullptr)
        {
            return std::unexpected(
                fmt::format("could not open '{}': {}", file.string(), std::strerror(errno)));
        }
        const usize written = std::fwrite(png->data(), 1, png->size(), out);
        const bool closed = std::fclose(out) == 0;
        if (written != png->size() || !closed)
        {
            return std::unexpected(fmt::format("could not write '{}'", file.string()));
        }
        return {};
    }

    string ScreenshotStem(const std::tm& local)
    {
        return fmt::format("{:04}-{:02}-{:02}_{:02}-{:02}-{:02}", local.tm_year + 1900,
                           local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min,
                           local.tm_sec);
    }

    Result<path> ReserveScreenshotPath(const path& directory, const std::tm& local)
    {
        const string stem = ScreenshotStem(local);
        for (u32 counter = 1; counter <= MaxScreenshotSuffix; ++counter)
        {
            const path candidate =
                directory / (counter == 1 ? fmt::format("{}.png", stem)
                                          : fmt::format("{}_{}.png", stem, counter));

            // Exclusive creation ("x") claims the name atomically, so two savers racing for the same
            // second each end up with a file of their own.
            std::FILE* const claimed = std::fopen(candidate.string().c_str(), "wbx");
            if (claimed != nullptr)
            {
                std::fclose(claimed);
                return candidate;
            }
            if (errno != EEXIST)
            {
                return std::unexpected(fmt::format("could not create '{}': {}", candidate.string(),
                                                   std::strerror(errno)));
            }
        }
        return std::unexpected(fmt::format("every name for {} up to _{} is taken in '{}'", stem,
                                           MaxScreenshotSuffix, directory.string()));
    }
}
