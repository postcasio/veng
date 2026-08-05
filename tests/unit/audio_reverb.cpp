// The embeddable Veng::Audio::Reverb asserted as pure arithmetic — no device, no live listen. The
// reverb is a pure function of its state and the send it is handed, so its behaviour is a set of
// properties on the CPU: an impulse decays within a bounded time, a larger room rings longer, more
// damping darkens the tail, a higher quality tier packs the tail denser without changing the gross
// decay, the output is deterministic and bounded, and an embedded instance wets a dry send with no
// AudioDevice around it. The properties are absolute bounds and orderings on the current output —
// no shadow of the old reverb is reconstructed as a control.

#include <doctest/doctest.h>

#include <Veng/Audio/Reverb.h>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace Veng;
using namespace Veng::Audio;

namespace
{
    // Small rate + short buffers keep every case in milliseconds while still scaling the comb taps
    // to a few hundred samples, so the tail has room to develop its structure.
    constexpr u32 kRate = 8000;
    constexpr usize kFrames = 16000;

    std::vector<f32> ImpulseSend(usize frames)
    {
        std::vector<f32> send(frames, 0.0f);
        send[0] = 1.0f;
        return send;
    }

    // Runs one send block through a freshly-prepared reverb, returning the left wet channel.
    std::vector<f32> WetLeft(const std::vector<f32>& send, const ReverbParams& params)
    {
        Reverb reverb;
        reverb.Prepare(kRate);
        std::vector<f32> wetL(send.size(), 0.0f);
        std::vector<f32> wetR(send.size(), 0.0f);
        reverb.ProcessBlock(send.data(), wetL.data(), wetR.data(), static_cast<u32>(send.size()),
                            params);
        return wetL;
    }

    f32 Peak(const std::vector<f32>& x)
    {
        f32 peak = 0.0f;
        for (const f32 s : x)
        {
            peak = std::max(peak, std::abs(s));
        }
        return peak;
    }

    // The last index whose sample exceeds a fraction of the window peak — a decay-length proxy.
    usize TailEnd(const std::vector<f32>& x, f32 relThreshold)
    {
        const f32 threshold = Peak(x) * relThreshold;
        for (usize i = x.size(); i-- > 0;)
        {
            if (std::abs(x[i]) > threshold)
            {
                return i;
            }
        }
        return 0;
    }

    f64 Energy(const std::vector<f32>& x, usize begin, usize end)
    {
        f64 sum = 0.0;
        for (usize i = begin; i < end; ++i)
        {
            sum += static_cast<f64>(x[i]) * static_cast<f64>(x[i]);
        }
        return sum;
    }

    // A high-frequency-content proxy: first-difference energy over signal energy. A darker tail
    // (more damping) carries proportionally less, so this tracks the spectral centroid's direction.
    f64 HighFrequencyRatio(const std::vector<f32>& x)
    {
        f64 diff = 0.0;
        f64 total = 0.0;
        for (usize i = 1; i < x.size(); ++i)
        {
            const f64 d = static_cast<f64>(x[i]) - static_cast<f64>(x[i - 1]);
            diff += d * d;
            total += static_cast<f64>(x[i]) * static_cast<f64>(x[i]);
        }
        return total > 0.0 ? diff / total : 0.0;
    }

    // Echo density in a window: the count of samples rising above a fraction of the window peak.
    // A sparser comb bank leaves nulls between echoes; more filters and tap modulation fill them.
    usize EchoDensity(const std::vector<f32>& x, usize begin, usize end)
    {
        f32 peak = 0.0f;
        for (usize i = begin; i < end; ++i)
        {
            peak = std::max(peak, std::abs(x[i]));
        }
        const f32 threshold = peak * 0.05f;
        usize count = 0;
        for (usize i = begin; i < end; ++i)
        {
            if (std::abs(x[i]) > threshold)
            {
                ++count;
            }
        }
        return count;
    }
}

TEST_CASE("reverb impulse decays and a larger room rings longer")
{
    const std::vector<f32> impulse = ImpulseSend(kFrames);

    const std::vector<f32> small = WetLeft(impulse, {.RoomSize = 0.2f,
                                                     .Damping = 0.5f,
                                                     .Wet = 1.0f,
                                                     .Width = 1.0f,
                                                     .Quality = ReverbQuality::Standard});
    const std::vector<f32> large = WetLeft(impulse, {.RoomSize = 0.9f,
                                                     .Damping = 0.5f,
                                                     .Wet = 1.0f,
                                                     .Width = 1.0f,
                                                     .Quality = ReverbQuality::Standard});

    // The small room's tail has fallen far below its peak by the final eighth of the window.
    const f64 peakEnergy = Energy(small, 0, kFrames / 8);
    const f64 lateEnergy = Energy(small, kFrames - kFrames / 8, kFrames);
    REQUIRE(peakEnergy > 0.0);
    CHECK(lateEnergy < peakEnergy * 0.01);

    // A larger room feeds back harder, so its tail persists later than the smaller room's.
    CHECK(TailEnd(large, 0.05f) > TailEnd(small, 0.05f));
}

TEST_CASE("reverb damping darkens the tail")
{
    const std::vector<f32> impulse = ImpulseSend(kFrames);

    const std::vector<f32> bright = WetLeft(impulse, {.RoomSize = 0.7f,
                                                      .Damping = 0.1f,
                                                      .Wet = 1.0f,
                                                      .Width = 1.0f,
                                                      .Quality = ReverbQuality::Standard});
    const std::vector<f32> dark = WetLeft(impulse, {.RoomSize = 0.7f,
                                                    .Damping = 0.9f,
                                                    .Wet = 1.0f,
                                                    .Width = 1.0f,
                                                    .Quality = ReverbQuality::Standard});

    CHECK(HighFrequencyRatio(dark) < HighFrequencyRatio(bright));
}

TEST_CASE("reverb quality raises echo density without changing gross decay")
{
    const std::vector<f32> impulse = ImpulseSend(kFrames);

    const ReverbParams base{.RoomSize = 0.7f, .Damping = 0.3f, .Wet = 1.0f, .Width = 1.0f};
    ReverbParams low = base;
    low.Quality = ReverbQuality::Low;
    ReverbParams standard = base;
    standard.Quality = ReverbQuality::Standard;
    ReverbParams high = base;
    high.Quality = ReverbQuality::High;

    const std::vector<f32> wetLow = WetLeft(impulse, low);
    const std::vector<f32> wetStandard = WetLeft(impulse, standard);
    const std::vector<f32> wetHigh = WetLeft(impulse, high);

    // Density strictly rises with the tier: more combs at Standard over Low, and tap modulation at
    // High filling the fixed-comb nulls Standard leaves.
    const usize densityLow = EchoDensity(wetLow, 200, 3000);
    const usize densityStandard = EchoDensity(wetStandard, 200, 3000);
    const usize densityHigh = EchoDensity(wetHigh, 200, 3000);
    CHECK(densityLow < densityStandard);
    CHECK(densityStandard < densityHigh);

    // Density is the knob, not decay: the feedback is identical, so all three tails end within the
    // same band.
    const usize endLow = TailEnd(wetLow, 0.05f);
    const usize endStandard = TailEnd(wetStandard, 0.05f);
    const usize endHigh = TailEnd(wetHigh, 0.05f);
    const usize longest = std::max({endLow, endStandard, endHigh});
    const usize shortest = std::min({endLow, endStandard, endHigh});
    REQUIRE(shortest > 1000);
    CHECK(longest < shortest * 5 / 2);
}

TEST_CASE("reverb is deterministic and bounded")
{
    const std::vector<f32> impulse = ImpulseSend(kFrames);
    const ReverbParams params{.RoomSize = 0.8f,
                              .Damping = 0.4f,
                              .Wet = 1.0f,
                              .Width = 1.0f,
                              .Quality = ReverbQuality::High};

    // Identical send and params through two fresh instances produce byte-identical wet output.
    const std::vector<f32> runA = WetLeft(impulse, params);
    const std::vector<f32> runB = WetLeft(impulse, params);
    REQUIRE(runA.size() == runB.size());
    bool identical = true;
    for (usize i = 0; i < runA.size(); ++i)
    {
        identical = identical && (runA[i] == runB[i]);
    }
    CHECK(identical);

    // A sustained full-scale send at the largest room never blows up at any quality.
    const std::vector<f32> sustained(kFrames, 1.0f);
    for (const ReverbQuality quality :
         {ReverbQuality::Low, ReverbQuality::Standard, ReverbQuality::High})
    {
        const std::vector<f32> wet = WetLeft(
            sustained,
            {.RoomSize = 1.0f, .Damping = 0.2f, .Wet = 1.0f, .Width = 1.0f, .Quality = quality});
        f32 worst = 0.0f;
        bool finite = true;
        for (const f32 s : wet)
        {
            finite = finite && std::isfinite(s);
            worst = std::max(worst, std::abs(s));
        }
        CHECK(finite);
        CHECK(worst < 100.0f);
    }
}

TEST_CASE("an embedded reverb wets a dry send with no device")
{
    // The way a generator would embed one: prepare and process standalone, no AudioDevice in sight.
    Reverb reverb;
    reverb.Prepare(kRate);

    std::vector<f32> send(kFrames, 0.0f);
    send[0] = 1.0f;
    std::vector<f32> wetL(kFrames, 0.0f);
    std::vector<f32> wetR(kFrames, 0.0f);
    reverb.ProcessBlock(send.data(), wetL.data(), wetR.data(), static_cast<u32>(kFrames),
                        {.RoomSize = 0.6f,
                         .Damping = 0.5f,
                         .Wet = 1.0f,
                         .Width = 1.0f,
                         .Quality = ReverbQuality::Standard});

    // A dry impulse produces a decaying wet tail well after the impulse frame.
    CHECK(std::isfinite(Peak(wetL)));
    CHECK(Energy(wetL, 100, kFrames) > 0.0);
}
