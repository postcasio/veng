// MeterAutoExposureHistogram — the auto-exposure meter's histogram arithmetic, device-free: what the
// floor excludes, what the percentile band averages, and how the highlight cap keeps a small bright
// subject from being blown out by a large dim field around it. Histograms are binned exactly as
// auto_exposure.comp bins them (AutoExposureBin), so a claim holds to one bin's width.

#include <array>
#include <cmath>

#include <doctest/doctest.h>

#include "Renderer/AutoExposureMetering.h"

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    using Histogram = std::array<u32, AutoExposureBinCount>;

    /// @brief Adds @p count pixels of @p luminance, binned as the metering pass bins them.
    void Add(Histogram& histogram, const f32 luminance, const u32 count,
             const AutoExposureMeterParams& params)
    {
        histogram[AutoExposureBin(luminance, params)] += count;
    }

    /// @brief One bin's width as a luminance ratio: how closely a binned value can be read back.
    f32 BinRatio(const AutoExposureMeterParams& params)
    {
        return std::exp2((AutoExposureMaxLog(params) - AutoExposureMinLog(params)) / 254.0f);
    }

    /// @brief A space scene's metering: everything above 1e-5 counts, the cap holds the 99th
    ///        percentile to 1.0 after exposure.
    AutoExposureMeterParams SpaceParams()
    {
        return {
            .FloorLuminance = 1e-5f,
            .MaxLuminance = 64.0f,
            .MinLuminance = 0.01f,
            .LowPercentile = 0.5f,
            .HighPercentile = 0.95f,
            .HighlightPercentile = 0.99f,
            .HighlightLuminance = 1.0f,
        };
    }
}

TEST_CASE("auto-exposure metering: a pixel below the floor is not metered")
{
    const AutoExposureMeterParams params{.FloorLuminance = 0.002f, .MaxLuminance = 64.0f};
    Histogram histogram{};
    Add(histogram, 0.0001f, 8000, params); // a dark field under the floor
    Add(histogram, 0.1f, 2000, params);    // the lit subject

    const AutoExposureReading reading = MeterAutoExposureHistogram(histogram, params);
    REQUIRE(reading.Valid);
    CHECK(reading.Average == doctest::Approx(0.1f).epsilon(BinRatio(params) - 1.0f));
}

TEST_CASE("auto-exposure metering: the highlight cap keeps a small bright subject at its limit")
{
    const AutoExposureMeterParams params = SpaceParams();
    Histogram histogram{};
    Add(histogram, 0.002f, 9700, params); // a dim haze filling most of the frame
    Add(histogram, 0.5f, 300, params);    // a lit planet on 3 % of it

    const AutoExposureReading reading = MeterAutoExposureHistogram(histogram, params);
    REQUIRE(reading.Valid);
    // The haze's average alone would expose the planet far past white.
    CHECK(params.Key / reading.Average * 0.5f > 10.0f);
    // The cap raises the target until the 99th percentile — the planet — exposes to the limit.
    const f32 exposedHighlight = params.Key / reading.Target * 0.5f;
    CHECK(exposedHighlight <= params.HighlightLuminance * BinRatio(params));
    CHECK(exposedHighlight >= params.HighlightLuminance / BinRatio(params));
}

TEST_CASE("auto-exposure metering: a uniform dim field meters its average under the cap")
{
    const AutoExposureMeterParams params = SpaceParams();
    Histogram histogram{};
    Add(histogram, 0.002f, 10000, params);

    const AutoExposureReading reading = MeterAutoExposureHistogram(histogram, params);
    REQUIRE(reading.Valid);
    CHECK(reading.Target == reading.Average);
    CHECK(reading.Average == doctest::Approx(0.002f).epsilon(BinRatio(params) - 1.0f));
}

TEST_CASE("auto-exposure metering: a zero highlight luminance turns the cap off")
{
    AutoExposureMeterParams params = SpaceParams();
    params.HighlightLuminance = 0.0f;
    Histogram histogram{};
    Add(histogram, 0.002f, 9700, params);
    Add(histogram, 0.5f, 300, params);

    const AutoExposureReading reading = MeterAutoExposureHistogram(histogram, params);
    REQUIRE(reading.Valid);
    CHECK(reading.Target == reading.Average);
}

TEST_CASE("auto-exposure metering: a frame with nothing above the floor reads invalid")
{
    const AutoExposureMeterParams params = SpaceParams();
    Histogram histogram{};
    histogram[0] = 10000;
    CHECK_FALSE(MeterAutoExposureHistogram(histogram, params).Valid);
}

TEST_CASE("auto-exposure metering: a subject brighter than the adaptation clamp still reads at its "
          "own luminance")
{
    // The clamp limits how far the exposure may fall; the histogram reaches past it, so the cap is
    // still computed from what the subject actually is.
    AutoExposureMeterParams params = SpaceParams();
    params.MaxLuminance = 0.2f;
    Histogram histogram{};
    Add(histogram, 0.002f, 9700, params);
    Add(histogram, 0.5f, 300, params);

    const AutoExposureReading reading = MeterAutoExposureHistogram(histogram, params);
    REQUIRE(reading.Valid);
    CHECK(reading.Highlight == doctest::Approx(0.5f).epsilon(BinRatio(params) - 1.0f));
}
