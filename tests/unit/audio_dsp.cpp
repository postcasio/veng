// The Veng::Audio::Dsp primitives asserted as pure arithmetic — no device, no live listen. Each
// primitive is a pure function of its state and the samples it is handed, so its correctness is a
// property on the CPU: an oscillator's fundamental and its band-limiting, an envelope's segment
// shape, a filter's response and its stability under modulation, noise character and determinism, a
// delay line's readback, a smoother's monotone approach, and a custom node's live parameterisation.

#include <doctest/doctest.h>

#include <Veng/Audio/Dsp.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

using namespace Veng;
using namespace Veng::Audio;

namespace
{
    constexpr u32 kSampleRate = 48000;

    // A single-frequency DFT magnitude over a Hann-windowed buffer. Windowing keeps a strong
    // harmonic from leaking into the inharmonic probes, so the alias measurement is a bound on the
    // current output rather than a comparison against any shadow oscillator.
    f64 MagnitudeAt(const std::vector<f32>& buffer, u32 sampleRate, f64 frequency)
    {
        const usize n = buffer.size();
        f64 re = 0.0;
        f64 im = 0.0;
        const f64 twoPi = 2.0 * std::numbers::pi;
        for (usize i = 0; i < n; ++i)
        {
            const f64 t = static_cast<f64>(i);
            const f64 window = 0.5 - 0.5 * std::cos(twoPi * t / static_cast<f64>(n - 1)); // Hann
            const f64 phase = twoPi * frequency * t / static_cast<f64>(sampleRate);
            re += static_cast<f64>(buffer[i]) * window * std::cos(phase);
            im -= static_cast<f64>(buffer[i]) * window * std::sin(phase);
        }
        return std::sqrt(re * re + im * im);
    }

    // The frequency at which frequency `freq` folds back into [0, Nyquist] under sampling.
    f64 FoldFrequency(f64 freq, u32 sampleRate)
    {
        const f64 fs = static_cast<f64>(sampleRate);
        f64 m = std::fmod(freq, fs);
        if (m < 0.0)
        {
            m += fs;
        }
        if (m > fs * 0.5)
        {
            m = fs - m;
        }
        return m;
    }

    std::vector<f32> RunOscillator(f32 hz, f32 shape, u32 count)
    {
        Dsp::Oscillator osc;
        osc.SetFrequency(hz, kSampleRate);
        osc.SetShape(shape);
        std::vector<f32> out(count);
        for (u32 i = 0; i < count; ++i)
        {
            out[i] = osc.Tick();
        }
        return out;
    }

    u32 CountZeroCrossings(const std::vector<f32>& buffer)
    {
        u32 crossings = 0;
        for (usize i = 1; i < buffer.size(); ++i)
        {
            const bool prevNeg = buffer[i - 1] < 0.0f;
            const bool curNeg = buffer[i] < 0.0f;
            if (prevNeg != curNeg)
            {
                ++crossings;
            }
        }
        return crossings;
    }
}

TEST_CASE("oscillator frequency tracks the request across the shape axis")
{
    const f32 hz = 1000.0f;
    const u32 count = 4800; // exactly 100 cycles at 1 kHz / 48 kHz

    // Every shape is bipolar and crosses zero twice per cycle, so the crossing count recovers the
    // frequency. Sweep the four archetype anchors; assert each recovers the fundamental in aggregate.
    f32 worstError = 0.0f;
    for (const f32 shape : {0.0f, 1.0f / 3.0f, 2.0f / 3.0f, 1.0f})
    {
        const std::vector<f32> buffer = RunOscillator(hz, shape, count);
        const u32 crossings = CountZeroCrossings(buffer);
        const f32 cycles = static_cast<f32>(crossings) / 2.0f;
        const f32 measuredHz = cycles * static_cast<f32>(kSampleRate) / static_cast<f32>(count);
        worstError = std::max(worstError, std::abs(measuredHz - hz) / hz);
    }
    CHECK(worstError < 0.02f);
}

TEST_CASE("a pure sine concentrates its energy at the fundamental")
{
    const f32 hz = 1000.0f;
    const std::vector<f32> buffer = RunOscillator(hz, 0.0f, 4096);

    const f64 fundamental = MagnitudeAt(buffer, kSampleRate, hz);
    // Sum the energy at the first several overtones; a clean sine leaves them near zero.
    f64 overtones = 0.0;
    for (u32 k = 2; k <= 8; ++k)
    {
        overtones += MagnitudeAt(buffer, kSampleRate, hz * k);
    }
    CHECK(fundamental > 0.0);
    CHECK(overtones < 0.05 * fundamental);
}

TEST_CASE("the saw and square shapes are band-limited")
{
    // A high fundamental that does not divide the sample rate, so its overtones alias well into the
    // band when un-band-limited. PolyBLEP suppresses the images: the aliased-image energy stays a
    // small fraction of the fundamental — a bound on the current output.
    const f32 hz = 2350.0f;
    const u32 count = 4096;

    for (const f32 shape : {2.0f / 3.0f, 1.0f}) // saw, square
    {
        const std::vector<f32> buffer = RunOscillator(hz, shape, count);
        const f64 fundamental = MagnitudeAt(buffer, kSampleRate, hz);
        REQUIRE(fundamental > 0.0);

        // Every harmonic above Nyquist folds to an inharmonic image; sum the magnitudes there.
        f64 aliasEnergy = 0.0;
        const f64 nyquist = kSampleRate * 0.5;
        for (u32 k = 2; k <= 40; ++k)
        {
            const f64 harmonic = static_cast<f64>(hz) * k;
            if (harmonic <= nyquist)
            {
                continue; // a real, in-band harmonic — not an alias
            }
            const f64 folded = FoldFrequency(harmonic, kSampleRate);
            if (folded < 50.0)
            {
                continue; // folds onto DC/fundamental region; skip the coincidence
            }
            aliasEnergy += MagnitudeAt(buffer, kSampleRate, folded);
        }
        CHECK(aliasEnergy < 0.25 * fundamental);
    }
}

TEST_CASE("the ADSR envelope walks its four segments in order")
{
    Dsp::Envelope env;
    const f32 attack = 100.0f;
    const f32 decay = 200.0f;
    const f32 sustain = 0.5f;
    const f32 release = 150.0f;
    env.SetAttack(attack);
    env.SetDecay(decay);
    env.SetSustain(sustain);
    env.SetRelease(release);

    CHECK_FALSE(env.IsActive());
    env.NoteOn();
    CHECK(env.IsActive());

    // Attack: rises monotonically to 1. Accumulate any non-increasing step as a violation.
    u32 attackDrops = 0;
    f32 prev = env.Value();
    for (u32 i = 0; i < static_cast<u32>(attack); ++i)
    {
        const f32 v = env.Tick();
        if (v < prev)
        {
            ++attackDrops;
        }
        prev = v;
    }
    CHECK(attackDrops == 0);
    CHECK(env.Value() == doctest::Approx(1.0f).epsilon(0.02));

    // Decay: falls monotonically toward the sustain level.
    u32 decayRises = 0;
    prev = env.Value();
    for (u32 i = 0; i < static_cast<u32>(decay); ++i)
    {
        const f32 v = env.Tick();
        if (v > prev)
        {
            ++decayRises;
        }
        prev = v;
    }
    CHECK(decayRises == 0);
    CHECK(env.Value() == doctest::Approx(sustain).epsilon(0.02));

    // Sustain: holds the level for as long as no note-off arrives.
    for (u32 i = 0; i < 500; ++i)
    {
        static_cast<void>(env.Tick());
    }
    CHECK(env.Value() == doctest::Approx(sustain).epsilon(0.001));
    CHECK(env.IsActive());

    // Release: falls monotonically to silence, and the envelope retires.
    env.NoteOff();
    u32 releaseRises = 0;
    prev = env.Value();
    u32 stepsToSilence = 0;
    for (u32 i = 0; i < static_cast<u32>(release) + 8; ++i)
    {
        const f32 v = env.Tick();
        if (v > prev + 1e-6f)
        {
            ++releaseRises;
        }
        prev = v;
        if (env.IsActive())
        {
            ++stepsToSilence;
        }
    }
    CHECK(releaseRises == 0);
    CHECK(env.Value() == doctest::Approx(0.0f));
    CHECK_FALSE(env.IsActive());
    // The release length is respected to within a sample or two of the set count.
    CHECK(stepsToSilence >= static_cast<u32>(release) - 2);
    CHECK(stepsToSilence <= static_cast<u32>(release) + 2);
}

TEST_CASE("Advance matches repeated Tick to the endpoint")
{
    Dsp::Envelope a;
    Dsp::Envelope b;
    for (Dsp::Envelope* e : {&a, &b})
    {
        e->SetAttack(64.0f);
        e->SetDecay(64.0f);
        e->SetSustain(0.4f);
        e->SetRelease(64.0f);
        e->NoteOn();
    }
    f32 last = 0.0f;
    for (u32 i = 0; i < 100; ++i)
    {
        last = a.Tick();
    }
    const f32 advanced = b.Advance(100);
    CHECK(advanced == doctest::Approx(last));
}

namespace
{
    // The RMS of the filter's chosen response driven by a sine at `driveHz`.
    f32 FilterResponseRms(f32 cutoffHz, f32 q, f32 driveHz, int outputIndex)
    {
        Dsp::Filter filter;
        filter.SetCutoff(cutoffHz, kSampleRate);
        filter.SetResonance(q);
        const u32 count = 4096;
        f64 sumSq = 0.0;
        const u32 warmup = 512; // let the filter settle before measuring
        for (u32 i = 0; i < count + warmup; ++i)
        {
            const f32 in = std::sin(2.0f * std::numbers::pi_v<f32> * driveHz * static_cast<f32>(i) /
                                    static_cast<f32>(kSampleRate));
            const Dsp::Filter::Outputs out = filter.Tick(in);
            const f32 y = outputIndex == 0   ? out.LowPass
                          : outputIndex == 1 ? out.HighPass
                          : outputIndex == 2 ? out.BandPass
                                             : out.Notch;
            if (i >= warmup)
            {
                sumSq += static_cast<f64>(y) * static_cast<f64>(y);
            }
        }
        return static_cast<f32>(std::sqrt(sumSq / count));
    }
}

TEST_CASE("the state-variable filter separates its four responses")
{
    const f32 cutoff = 2000.0f;
    const f32 low = 200.0f;
    const f32 high = 12000.0f;

    // Low-pass passes the low tone and attenuates the high one; the high-pass is the reverse.
    CHECK(FilterResponseRms(cutoff, 1.0f, low, 0) >
          4.0f * FilterResponseRms(cutoff, 1.0f, high, 0));
    CHECK(FilterResponseRms(cutoff, 1.0f, high, 1) >
          4.0f * FilterResponseRms(cutoff, 1.0f, low, 1));

    // Band-pass peaks at cutoff relative to a tone well below or above it.
    const f32 bpAtCutoff = FilterResponseRms(cutoff, 4.0f, cutoff, 2);
    CHECK(bpAtCutoff > FilterResponseRms(cutoff, 4.0f, low, 2));
    CHECK(bpAtCutoff > FilterResponseRms(cutoff, 4.0f, high, 2));

    // Notch nulls at cutoff relative to a tone away from it.
    const f32 notchAtCutoff = FilterResponseRms(cutoff, 1.0f, cutoff, 3);
    CHECK(notchAtCutoff < FilterResponseRms(cutoff, 1.0f, low, 3));

    // Raising resonance raises the band-pass peak at cutoff.
    CHECK(FilterResponseRms(cutoff, 8.0f, cutoff, 2) > FilterResponseRms(cutoff, 1.0f, cutoff, 2));
}

TEST_CASE("the filter stays bounded under per-sample cutoff modulation")
{
    Dsp::Filter filter;
    filter.SetResonance(10.0f);
    Dsp::Noise noise(0xC0FFEEu);

    f32 peak = 0.0f;
    u32 nonFinite = 0;
    for (u32 i = 0; i < 20000; ++i)
    {
        // Sweep the cutoff across the whole band every sample — the case a one-pole cannot survive.
        const f32 sweep = 100.0f + 0.5f * (1.0f + std::sin(static_cast<f32>(i) * 0.01f)) * 18000.0f;
        filter.SetCutoff(sweep, kSampleRate);
        const Dsp::Filter::Outputs out = filter.Tick(noise.White());
        for (const f32 y : {out.LowPass, out.HighPass, out.BandPass, out.Notch})
        {
            if (!std::isfinite(y))
            {
                ++nonFinite;
            }
            peak = std::max(peak, std::abs(y));
        }
    }
    CHECK(nonFinite == 0);
    CHECK(peak < 100.0f);
}

TEST_CASE("noise is near-zero-mean white, negatively-sloped pink, and seed-deterministic")
{
    const u32 count = 16384;

    Dsp::Noise white(1234u);
    std::vector<f32> whiteBuf(count);
    f64 mean = 0.0;
    for (u32 i = 0; i < count; ++i)
    {
        whiteBuf[i] = white.White();
        mean += static_cast<f64>(whiteBuf[i]);
    }
    mean /= count;
    CHECK(std::abs(mean) < 0.03);

    // White: comparable energy in a low band and a high band (roughly flat spectrum).
    f64 whiteLow = 0.0;
    f64 whiteHigh = 0.0;
    for (u32 k = 1; k <= 6; ++k)
    {
        whiteLow += MagnitudeAt(whiteBuf, kSampleRate, 300.0 * k);
        whiteHigh += MagnitudeAt(whiteBuf, kSampleRate, 8000.0 + 300.0 * k);
    }
    CHECK(whiteHigh > 0.3 * whiteLow); // not collapsing to only low energy

    Dsp::Noise pink(1234u);
    std::vector<f32> pinkBuf(count);
    for (u32 i = 0; i < count; ++i)
    {
        pinkBuf[i] = pink.Pink();
    }
    f64 pinkLow = 0.0;
    f64 pinkHigh = 0.0;
    for (u32 k = 1; k <= 6; ++k)
    {
        pinkLow += MagnitudeAt(pinkBuf, kSampleRate, 300.0 * k);
        pinkHigh += MagnitudeAt(pinkBuf, kSampleRate, 8000.0 + 300.0 * k);
    }
    // Pink falls with frequency: the low band carries clearly more energy than the high band.
    CHECK(pinkLow > 2.0 * pinkHigh);

    // Determinism: the same seed reproduces the sequence exactly.
    Dsp::Noise a(777u);
    Dsp::Noise b(777u);
    u32 mismatches = 0;
    for (u32 i = 0; i < 2048; ++i)
    {
        if (a.White() != b.White())
        {
            ++mismatches;
        }
    }
    CHECK(mismatches == 0);
}

TEST_CASE("the delay line reads a written sample back after the set delay")
{
    Dsp::DelayLine delay;
    delay.Prepare(64);

    // Write an impulse, then zeros; it reads back exactly `d` writes later.
    const u32 d = 10;
    delay.Write(1.0f);
    for (u32 i = 1; i < d; ++i)
    {
        delay.Write(0.0f);
    }
    CHECK(delay.Read(d) == doctest::Approx(1.0f));
    CHECK(delay.Read(1) == doctest::Approx(0.0f));

    // Fractional read interpolates the two straddling samples.
    Dsp::DelayLine frac;
    frac.Prepare(16);
    frac.Write(2.0f); // becomes Read(2) after the next write
    frac.Write(4.0f); // becomes Read(1)
    CHECK(frac.Read(2) == doctest::Approx(2.0f));
    CHECK(frac.Read(1) == doctest::Approx(4.0f));
    CHECK(frac.ReadInterpolated(1.5f) == doctest::Approx(3.0f));
}

TEST_CASE("the smoother approaches a step target monotonically without overshoot")
{
    Dsp::Smoother smoother;
    smoother.SetValue(0.0f);
    smoother.SetTime(0.01f, kSampleRate); // 10 ms time constant
    smoother.SetTarget(1.0f);

    const u32 tau = kSampleRate / 100; // one time constant in samples
    f32 prev = smoother.Value();
    u32 drops = 0;
    u32 overshoots = 0;
    f32 valueAtTau = 0.0f;
    for (u32 i = 0; i < tau * 8; ++i)
    {
        const f32 v = smoother.Tick();
        if (v < prev)
        {
            ++drops;
        }
        if (v > 1.0f + 1e-4f)
        {
            ++overshoots;
        }
        if (i == tau - 1)
        {
            valueAtTau = v;
        }
        prev = v;
    }
    CHECK(drops == 0);
    CHECK(overshoots == 0);
    // After one time constant it has closed roughly 63% of the gap.
    CHECK(valueAtTau > 0.5f);
    CHECK(valueAtTau < 0.75f);
    // After several time constants it has settled within tolerance of the target.
    CHECK(smoother.Value() == doctest::Approx(1.0f).epsilon(0.01));
}

TEST_CASE("a custom source is deterministic and a custom filter composes and reads live knobs")
{
    // CustomSource bound to a known callable: two runs with the same inputs agree sample-for-sample.
    auto rampFill = [](f32* out, u32 frames, u32 /*sampleRate*/)
    {
        for (u32 i = 0; i < frames; ++i)
        {
            out[i] = static_cast<f32>(i);
        }
    };
    Dsp::CustomSource sourceA{rampFill};
    Dsp::CustomSource sourceB{rampFill};
    std::vector<f32> a(32);
    std::vector<f32> b(32);
    sourceA.Render(a.data(), 32, kSampleRate);
    sourceB.Render(b.data(), 32, kSampleRate);
    u32 mismatches = 0;
    for (u32 i = 0; i < 32; ++i)
    {
        if (a[i] != b[i])
        {
            ++mismatches;
        }
    }
    CHECK(mismatches == 0);

    // A CustomFilter composes in a chain: doubling then adding one transforms as specified.
    Dsp::CustomFilter doubler{[](f32* s, u32 frames, u32)
                              {
                                  for (u32 i = 0; i < frames; ++i)
                                  {
                                      s[i] *= 2.0f;
                                  }
                              }};
    Dsp::CustomFilter plusOne{[](f32* s, u32 frames, u32)
                              {
                                  for (u32 i = 0; i < frames; ++i)
                                  {
                                      s[i] += 1.0f;
                                  }
                              }};
    std::vector<f32> chain = {1.0f, 2.0f, 3.0f};
    doubler.Apply(chain.data(), 3, kSampleRate);
    plusOne.Apply(chain.data(), 3, kSampleRate);
    CHECK(chain[0] == doctest::Approx(3.0f));
    CHECK(chain[1] == doctest::Approx(5.0f));
    CHECK(chain[2] == doctest::Approx(7.0f));

    // A live knob reaches the bound callable through a pointer it captured at construction: the code
    // is fixed, the parameter is not. Changing the knob between blocks changes the output.
    f32 gain = 1.0f;
    Dsp::CustomFilter scaler{[knob = &gain](f32* s, u32 frames, u32)
                             {
                                 for (u32 i = 0; i < frames; ++i)
                                 {
                                     s[i] *= *knob;
                                 }
                             }};
    std::vector<f32> block = {1.0f, 1.0f, 1.0f, 1.0f};
    scaler.Apply(block.data(), 4, kSampleRate);
    CHECK(block[0] == doctest::Approx(1.0f));
    gain = 3.0f; // the same live parameter path a GeneratorParams latch feeds
    std::ranges::fill(block, 1.0f);
    scaler.Apply(block.data(), 4, kSampleRate);
    CHECK(block[0] == doctest::Approx(3.0f));
}
