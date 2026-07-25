#pragma once

// The gpu band's shared image-capture helpers: the RGBA16Sfloat -> 8-bit RGB decode, the PPM dump,
// and the fuzzy comparator every Gui golden checks itself with. One copy, so the tolerance is one
// number rather than one per case and a new capture cannot quietly widen it.
//
// stb_image's implementation is already compiled into libveng_cook (which the gpu target links), so
// this header includes only the declarations.

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include <doctest/doctest.h>

#include <stb_image.h>

#include <Veng/Path.h>
#include <Veng/Veng.h>

// After the Veng headers, so Veng.h's GLM_FORCE_DEPTH_ZERO_TO_ONE is set before glm.
#include <glm/gtc/packing.hpp>

namespace Veng::Test
{
    /// @brief Maximum per-channel difference, in 8-bit levels, a matching pixel may carry.
    ///
    /// The tolerance shape golden_compare uses, widened slightly for the MSDF text edges whose
    /// derivative-based anti-aliasing can jitter a pixel between drivers.
    inline constexpr int GoldenMaxChannelDelta = 10;

    /// @brief Maximum fraction of pixels allowed to exceed GoldenMaxChannelDelta.
    inline constexpr double GoldenMaxMismatchFraction = 0.02;

    /// @brief Outcome of a fuzzy image comparison.
    struct GoldenComparison
    {
        /// @brief Pixels whose worst channel differed by more than the allowed delta.
        long Mismatched = 0;
        /// @brief Total pixels compared.
        long PixelCount = 0;
        /// @brief The largest per-channel difference seen anywhere.
        int Worst = 0;
        /// @brief Mismatched divided by PixelCount.
        double Fraction = 0.0;
    };

    /// @brief Converts an RGBA16Sfloat download into 8-bit RGB, clamped to [0,1].
    /// @param halfBytes  The downloaded half-float RGBA bytes.
    /// @param extent     The image extent the bytes cover.
    /// @return Row-major 8-bit RGB, three bytes per pixel.
    [[nodiscard]] inline vector<u8> DecodeHalfRgb(const vector<u8>& halfBytes, const uvec2 extent)
    {
        const auto* halves = reinterpret_cast<const u16*>(halfBytes.data());
        vector<u8> rgb;
        rgb.reserve(static_cast<usize>(extent.x) * extent.y * 3);
        for (u32 pixel = 0; pixel < extent.x * extent.y; ++pixel)
        {
            for (u32 channel = 0; channel < 3; ++channel)
            {
                const f32 value =
                    glm::clamp(glm::unpackHalf1x16(halves[pixel * 4 + channel]), 0.0f, 1.0f);
                rgb.push_back(static_cast<u8>(value * 255.0f + 0.5f));
            }
        }
        return rgb;
    }

    /// @brief Writes 8-bit RGB pixels out as a binary PPM.
    /// @param out     Destination file.
    /// @param rgb     Row-major 8-bit RGB pixels.
    /// @param extent  The image extent the pixels cover.
    inline void WritePpm(const path& out, const vector<u8>& rgb, const uvec2 extent)
    {
        std::ofstream stream(out, std::ios::binary);
        stream << "P6\n" << extent.x << " " << extent.y << "\n255\n";
        stream.write(reinterpret_cast<const char*>(rgb.data()),
                     static_cast<std::streamsize>(rgb.size()));
    }

    /// @brief Counts how far two 8-bit RGB images differ, per pixel and worst case.
    /// @param actual        Row-major 8-bit RGB pixels.
    /// @param expected      Row-major 8-bit RGB pixels of the same extent.
    /// @param extent        The extent both cover.
    /// @param channelDelta  Per-channel difference a pixel may carry before it counts as mismatched.
    /// @return The mismatch counts and the worst per-channel difference.
    [[nodiscard]] inline GoldenComparison CompareRgb(const vector<u8>& actual,
                                                     const vector<u8>& expected, const uvec2 extent,
                                                     const int channelDelta = GoldenMaxChannelDelta)
    {
        GoldenComparison result;
        result.PixelCount = static_cast<long>(extent.x) * extent.y;
        for (long i = 0; i < result.PixelCount; ++i)
        {
            int pixelDelta = 0;
            for (int channel = 0; channel < 3; ++channel)
            {
                const int a = actual[i * 3 + channel];
                const int b = expected[i * 3 + channel];
                const int delta = a > b ? a - b : b - a;
                pixelDelta = delta > pixelDelta ? delta : pixelDelta;
            }
            result.Worst = pixelDelta > result.Worst ? pixelDelta : result.Worst;
            if (pixelDelta > channelDelta)
            {
                ++result.Mismatched;
            }
        }
        result.Fraction = result.PixelCount > 0 ? static_cast<double>(result.Mismatched) /
                                                      static_cast<double>(result.PixelCount)
                                                : 0.0;
        return result;
    }

    /// @brief Dumps a capture on request, else fuzzy-compares it against a committed golden PNG.
    ///
    /// The whole golden protocol in one call: when @p dumpVariable names an environment variable
    /// that is set, the capture is written there as a PPM (how a golden is regenerated — dump, sips
    /// the PPM to the golden path, commit) and nothing is asserted; otherwise the golden is loaded
    /// and compared, failing the case when too large a fraction of pixels differ.
    /// @param label         Human-readable case name, used in the reported messages.
    /// @param actual        The capture, as row-major 8-bit RGB.
    /// @param extent        The extent the capture covers; the golden must match it.
    /// @param dumpVariable  Environment variable naming a dump destination, or nullptr for none.
    /// @param golden        The committed golden PNG.
    /// @return True when the capture was dumped rather than compared (the caller may return early).
    inline bool CheckAgainstGolden(const std::string_view label, const vector<u8>& actual,
                                   const uvec2 extent, const char* dumpVariable, const path& golden)
    {
        if (dumpVariable != nullptr)
        {
            if (const char* dump = std::getenv(dumpVariable))
            {
                WritePpm(path(dump), actual, extent);
                MESSAGE(string(label), ": wrote capture to ", dump);
                return true;
            }
        }

        int width = 0;
        int height = 0;
        int channels = 0;
        u8* goldenPixels = stbi_load(golden.string().c_str(), &width, &height, &channels, 3);
        REQUIRE_MESSAGE(goldenPixels != nullptr, string(label), ": failed to load ",
                        golden.string());
        REQUIRE(static_cast<u32>(width) == extent.x);
        REQUIRE(static_cast<u32>(height) == extent.y);

        const vector<u8> expected(goldenPixels,
                                  goldenPixels + static_cast<usize>(extent.x) * extent.y * 3);
        stbi_image_free(goldenPixels);

        const GoldenComparison comparison = CompareRgb(actual, expected, extent);
        MESSAGE(string(label), ": ", comparison.Mismatched, "/", comparison.PixelCount,
                " pixels exceed delta ", GoldenMaxChannelDelta, " (worst ", comparison.Worst, ")");
        CHECK(comparison.Fraction <= GoldenMaxMismatchFraction);
        return false;
    }
}
