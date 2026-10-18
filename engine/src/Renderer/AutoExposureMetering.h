#pragma once

#include <Veng/Veng.h>

#include <algorithm>
#include <cmath>
#include <span>

namespace Veng::Renderer
{
    /// @brief Log-luminance histogram resolution: bin 0 is the excluded black bin, bins 1..255
    ///        span [floor, max]. Matches BinCount in auto_exposure.comp.
    inline constexpr u32 AutoExposureBinCount = 256;

    /// @brief What the metering reads off one histogram: the knobs SceneView carries for it.
    struct AutoExposureMeterParams
    {
        /// @brief Luminance below which a pixel is not metered (the histogram's lower bound).
        f32 FloorLuminance = 0.002f;
        /// @brief Upper clamp on the adapted luminance: how far a bright scene may darken the exposure.
        f32 MaxLuminance = 8.0f;
        /// @brief Lower clamp on the adapted luminance: the darkest scene exposed up to the key.
        f32 MinLuminance = 0.002f;
        /// @brief Lower percentile of the metered pixels the average starts at, in [0, 1].
        f32 LowPercentile = 0.0f;
        /// @brief Upper percentile of the metered pixels the average ends at, in [0, 1].
        f32 HighPercentile = 1.0f;
        /// @brief The percentile of the metered pixels the highlight cap protects, in [0, 1].
        f32 HighlightPercentile = 0.99f;
        /// @brief The exposed luminance the highlight percentile may reach; zero turns the cap off.
        f32 HighlightLuminance = 0.0f;
        /// @brief The mid-grey the adapted luminance maps to.
        f32 Key = 0.18f;
    };

    /// @brief The histogram's log2 lower bound.
    [[nodiscard]] inline f32 AutoExposureMinLog(const AutoExposureMeterParams& params)
    {
        return std::log2(std::max(params.FloorLuminance, 1e-5f));
    }

    /// @brief The luminance the histogram's top bin reaches: the scene colour's own limit, the
    ///        largest finite half-float.
    ///
    /// Fixed rather than tied to MaxLuminance, so the clamp on how far the exposure may fall never
    /// truncates what the meter reads: a subject brighter than the clamp still reads at its own
    /// luminance, and the highlight cap still sees it.
    inline constexpr f32 AutoExposureMeterCeiling = 65504.0f;

    /// @brief The histogram's log2 upper bound, kept above the lower one.
    [[nodiscard]] inline f32 AutoExposureMaxLog(const AutoExposureMeterParams& params)
    {
        return std::max(std::log2(AutoExposureMeterCeiling), AutoExposureMinLog(params) + 1e-3f);
    }

    /// @brief The bin auto_exposure.comp scatters a luminance into (0 for an unmetered pixel).
    [[nodiscard]] inline u32 AutoExposureBin(const f32 luminance,
                                             const AutoExposureMeterParams& params)
    {
        const f32 lo = AutoExposureMinLog(params);
        if (!(luminance >= 1e-5f) || std::log2(luminance) < lo)
        {
            return 0;
        }
        const f32 t =
            std::clamp((std::log2(luminance) - lo) / (AutoExposureMaxLog(params) - lo), 0.0f, 1.0f);
        return std::clamp(static_cast<u32>(t * 254.0f + 1.0f), 1u, AutoExposureBinCount - 1);
    }

    /// @brief The log2 luminance at a fractional position through a bin (0 its low edge, 1 its high).
    [[nodiscard]] inline f32 AutoExposureBinLog(const u32 bin, const f32 within,
                                                const AutoExposureMeterParams& params)
    {
        const f32 lo = AutoExposureMinLog(params);
        const f32 t = (static_cast<f32>(bin) - 1.0f + within) / 254.0f;
        return lo + std::min(t, 1.0f) * (AutoExposureMaxLog(params) - lo);
    }

    /// @brief One histogram's metering result.
    struct AutoExposureReading
    {
        /// @brief Whether any pixel was metered; nothing else is meaningful when false.
        bool Valid = false;
        /// @brief The geometric mean of the metered pixels inside the percentile band.
        f32 Average = 0.0f;
        /// @brief The luminance at the highlight percentile.
        f32 Highlight = 0.0f;
        /// @brief The adapted luminance the frame asks for: the average, raised far enough that
        ///        the highlight percentile exposes no brighter than HighlightLuminance.
        f32 Target = 0.0f;
    };

    /// @brief Meters one log-luminance histogram.
    ///
    /// Bin 0 holds the pixels below the floor and is not metered, so a frame of mostly black
    /// space meters its lit content. The average is the geometric mean over the [low, high]
    /// percentile band of the metered pixels, bins straddling a band edge contributing
    /// fractionally. With the cap on, the target is the larger of that average and
    /// `Highlight · Key / HighlightLuminance` — the adapted luminance at which the highlight
    /// percentile exposes to exactly HighlightLuminance — so a small bright subject in a large dim
    /// field sets the exposure rather than being blown out by the field's average.
    /// @param histogram  The per-bin pixel counts, AutoExposureBinCount of them.
    /// @param params     The metering knobs the histogram was built with.
    /// @return The reading; invalid when no pixel was metered.
    [[nodiscard]] inline AutoExposureReading
    MeterAutoExposureHistogram(const std::span<const u32> histogram,
                               const AutoExposureMeterParams& params)
    {
        AutoExposureReading reading;
        const usize bins = std::min<usize>(histogram.size(), AutoExposureBinCount);
        f64 metered = 0.0;
        for (usize bin = 1; bin < bins; ++bin)
        {
            metered += histogram[bin];
        }
        if (metered <= 0.0)
        {
            return reading;
        }

        const f64 lowPercentile = std::clamp(params.LowPercentile, 0.0f, 1.0f);
        const f64 highPercentile =
            std::clamp(static_cast<f64>(params.HighPercentile), lowPercentile, 1.0);
        const f64 lowRank = metered * lowPercentile;
        const f64 highRank = metered * highPercentile;
        const f64 highlightRank =
            metered * std::clamp(static_cast<f64>(params.HighlightPercentile), 0.0, 1.0);

        f64 weightedLog = 0.0;
        f64 count = 0.0;
        f64 rank = 0.0;
        optional<f32> highlightLog;
        for (usize bin = 1; bin < bins; ++bin)
        {
            const u32 binCount = histogram[bin];
            if (binCount == 0)
            {
                continue;
            }
            const f64 binLow = rank;
            rank += binCount;
            const f64 inBand = std::max(0.0, std::min(rank, highRank) - std::max(binLow, lowRank));
            if (inBand > 0.0)
            {
                weightedLog +=
                    static_cast<f64>(AutoExposureBinLog(static_cast<u32>(bin), 0.5f, params)) *
                    inBand;
                count += inBand;
            }
            if (!highlightLog && rank >= highlightRank)
            {
                const auto within = static_cast<f32>((highlightRank - binLow) / binCount);
                highlightLog = AutoExposureBinLog(static_cast<u32>(bin), within, params);
            }
        }
        if (count <= 0.0)
        {
            return reading;
        }

        reading.Valid = true;
        reading.Average = std::exp2(static_cast<f32>(weightedLog / count));
        reading.Highlight = std::exp2(highlightLog.value_or(AutoExposureMaxLog(params)));
        reading.Target = reading.Average;
        if (params.HighlightLuminance > 0.0f)
        {
            reading.Target = std::max(reading.Target,
                                      reading.Highlight * params.Key / params.HighlightLuminance);
        }
        return reading;
    }
}
