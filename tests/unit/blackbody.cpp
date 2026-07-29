// Planck's law and the Planckian locus: pure CPU, no Context, no Vulkan.
//
// This file is the independent check on the whole spectral chain in Colorimetry.h, not only on
// the law it exercises. A blackbody's chromaticity is published to four decimal places across
// the entire temperature range, by the same body that published the standard observer — so if
// the locus computed here lands on the published one, the transcribed colour-matching tables,
// the integration, the primaries and the white point are all correct together. Nothing else
// available tests the chain end to end against an answer derived outside this repository.
//
// The references, all external:
//   * the Planckian locus — the standard tabulation of Planckian-radiator chromaticities in the
//     CIE 1931 space, as carried by Wyszecki & Stiles, "Color Science: Concepts and Methods,
//     Quantitative Data and Formulae", 2nd ed., and reproduced across the colour-science
//     literature;
//   * Wien's displacement constant b = 2.897771955e-3 m K and the Stefan-Boltzmann constant
//     sigma = 5.670374419e-8 W m^-2 K^-4, CODATA;
//   * the blackbody fractional function of the first kind F(0 -> lambda T), the standard
//     radiation-function table (Incropera & DeWitt, "Fundamentals of Heat and Mass Transfer",
//     Table 12.2), cross-checked here against its own series form;
//   * the D65 white point, x = 0.3127, y = 0.3290, as sRGB and Rec.709 state it.

#include <doctest/doctest.h>

#include <cmath>
#include <numbers>

#include <Veng/Math/Colorimetry.h>

using namespace Veng;

namespace
{
    // CODATA, and the same constants Colorimetry.cpp evaluates Planck's law from — spelled again
    // here so an assertion compares against the published value rather than against the
    // implementation's copy of it.
    constexpr f64 WienDisplacementNmK = 2.897771955e6;
    constexpr f64 StefanBoltzmann = 5.670374419e-8;
    constexpr f64 SecondRadiationConstantNmK = 1.4387769e7;

    // The blackbody fractional function of the first kind: the share of a blackbody's total
    // emission that falls below `wavelengthNm`. Depends only on the product lambda * T. This is
    // the series form of the same quantity the published radiation-function table carries, and
    // the case below pins it against that table before anything else uses it.
    f64 EmittedFractionBelow(f64 wavelengthNm, f64 temperatureK)
    {
        const f64 x = SecondRadiationConstantNmK / (wavelengthNm * temperatureK);
        f64 sum = 0.0;
        for (f64 n = 1.0; n <= 200.0; n += 1.0)
        {
            sum += std::exp(-n * x) / n *
                   ((x * x * x) + (3.0 * x * x / n) + (6.0 * x / (n * n)) + (6.0 / (n * n * n)));
        }
        const f64 pi = std::numbers::pi;
        return 15.0 / (pi * pi * pi * pi) * sum;
    }

    // Trapezoidal integral of the band-sampled spectrum. The grid holds point values at both
    // endpoints, so the plain Riemann sum SpectrumToXyz uses counts half a sample too much at
    // each end — 1 to 2% of this band's integral, which is the difference between an assertion
    // that catches a units error and one that only catches a large one.
    f64 BandRadiance(const Spectrum& spectrum)
    {
        f64 sum = 0.0;
        for (const f32 sample : spectrum.Samples)
        {
            sum += static_cast<f64>(sample);
        }
        sum -= 0.5 * static_cast<f64>(spectrum.Samples[0]);
        sum -= 0.5 * static_cast<f64>(spectrum.Samples[SpectrumSampleCount - 1]);
        return sum * static_cast<f64>(SpectrumWavelengthStep);
    }

    // Integrates Planck's law over effectively the whole spectrum, on a log-spaced grid running
    // from hc/(lambda k T) = 200 (where the exponential has annihilated the curve) out to 1e-3
    // (where the residual tail is below 1e-9 of the total).
    f64 TotalRadianceByQuadrature(f32 temperatureK, u32 steps)
    {
        const f64 temperature = static_cast<f64>(temperatureK);
        const f64 lowest = std::log(SecondRadiationConstantNmK / (200.0 * temperature));
        const f64 highest = std::log(SecondRadiationConstantNmK / (1.0e-3 * temperature));
        const f64 stride = (highest - lowest) / static_cast<f64>(steps);

        f64 total = 0.0;
        f64 previous = 0.0;
        for (u32 i = 0; i <= steps; ++i)
        {
            const f64 wavelength = std::exp(lowest + (stride * static_cast<f64>(i)));
            // d(lambda) = lambda d(log lambda), so the log-space integrand carries a factor of
            // the wavelength.
            const f64 value = static_cast<f64>(PlanckSpectralRadiance(static_cast<f32>(wavelength),
                                                                      temperatureK)) *
                              wavelength;
            if (i > 0)
            {
                total += 0.5 * (previous + value) * stride;
            }
            previous = value;
        }
        return total;
    }

    // Wavelength of the sampled spectrum's largest sample.
    f32 PeakSampleWavelength(const Spectrum& spectrum)
    {
        u32 peak = 0;
        for (u32 i = 1; i < SpectrumSampleCount; ++i)
        {
            if (spectrum.Samples[i] > spectrum.Samples[peak])
            {
                peak = i;
            }
        }
        return Spectrum::WavelengthAt(peak);
    }
}

TEST_CASE("the Planckian locus reproduces the published chromaticities")
{
    // The standard tabulation, cited in the file header. Agreement here is the whole spectral
    // chain validating at once: the observer tables, the sampling grid's alignment, the Riemann
    // integration, and the chromaticity projection. A mistyped table row or a grid off by one
    // sample moves a locus point by far more than the tolerance below and cannot be absorbed
    // anywhere else.
    //
    // The tolerance is absolute rather than relative because the published values are stated to
    // four decimals, so half a unit in the last place is 5e-5 before anything else contributes;
    // the rest is the band being truncated to 380-780 nm (the observer's published tails carry
    // under 0.03% of each function's area) and the 5 nm sum standing in for an integral.
    struct LocusPoint
    {
        f32 Temperature;
        f32 X;
        f32 Y;
    };
    constexpr LocusPoint Published[] = {
        {.Temperature = 1000.0f, .X = 0.6528f, .Y = 0.3444f},
        {.Temperature = 1500.0f, .X = 0.5857f, .Y = 0.3931f},
        {.Temperature = 2000.0f, .X = 0.5267f, .Y = 0.4133f},
        {.Temperature = 2500.0f, .X = 0.4770f, .Y = 0.4137f},
        {.Temperature = 3000.0f, .X = 0.4369f, .Y = 0.4041f},
        {.Temperature = 3500.0f, .X = 0.4053f, .Y = 0.3907f},
        {.Temperature = 4000.0f, .X = 0.3805f, .Y = 0.3768f},
        {.Temperature = 4500.0f, .X = 0.3608f, .Y = 0.3636f},
        {.Temperature = 5000.0f, .X = 0.3451f, .Y = 0.3516f},
        {.Temperature = 5500.0f, .X = 0.3325f, .Y = 0.3411f},
        {.Temperature = 6000.0f, .X = 0.3221f, .Y = 0.3318f},
        {.Temperature = 6500.0f, .X = 0.3135f, .Y = 0.3237f},
        {.Temperature = 7000.0f, .X = 0.3064f, .Y = 0.3166f},
        {.Temperature = 7500.0f, .X = 0.3004f, .Y = 0.3103f},
        {.Temperature = 8000.0f, .X = 0.2952f, .Y = 0.3048f},
        {.Temperature = 9000.0f, .X = 0.2869f, .Y = 0.2956f},
        {.Temperature = 10000.0f, .X = 0.2807f, .Y = 0.2884f},
        {.Temperature = 15000.0f, .X = 0.2637f, .Y = 0.2673f},
        {.Temperature = 20000.0f, .X = 0.2565f, .Y = 0.2577f},
        {.Temperature = 30000.0f, .X = 0.2501f, .Y = 0.2489f},
    };

    for (const LocusPoint& point : Published)
    {
        const vec2 chromaticity = BlackbodyChromaticity(point.Temperature);
        CHECK(std::abs(chromaticity.x - point.X) < 2.0e-4f);
        CHECK(std::abs(chromaticity.y - point.Y) < 2.0e-4f);
    }
}

TEST_CASE("a 6500 K blackbody is near neutral but does not sit on the D65 white point")
{
    // The most legible check in the file and the one most easily written wrong. D65 is a
    // *daylight* illuminant, and daylight sits measurably above the Planckian locus: sRGB and
    // Rec.709 put D65 at (0.3127, 0.3290) while the locus at 6500 K is at (0.3135, 0.3237). The
    // gap of 0.0053 in y is physics, not error, so the locus is what this asserts against.
    const vec2 chromaticity = BlackbodyChromaticity(6500.0f);
    CHECK(std::abs(chromaticity.x - 0.3135f) < 2.0e-4f);
    CHECK(std::abs(chromaticity.y - 0.3237f) < 2.0e-4f);

    CHECK(std::abs(chromaticity.x - SrgbWhitePoint.x) < 1.0e-3f);
    CHECK(std::abs(chromaticity.y - SrgbWhitePoint.y) > 4.0e-3f);

    // Being below the locus's daylight neighbour in y means slightly less green, so a 6500 K
    // blackbody reads faintly magenta against the white point rather than exactly neutral. The
    // 5% tolerance is what that real offset costs; a true neutral would be (1, 1, 1) exactly.
    const vec3 rgb = BlackbodyColor(6500.0f);
    CHECK(rgb.r == doctest::Approx(1.0f).epsilon(0.05));
    CHECK(rgb.g == doctest::Approx(1.0f).epsilon(0.05));
    CHECK(rgb.b == doctest::Approx(1.0f).epsilon(0.05));
    CHECK(rgb.g < rgb.r);
    CHECK(rgb.g < rgb.b);
}

TEST_CASE("the normalized colour carries unit luminance and the spectrum carries the level")
{
    // "Normalized" is Y = 1 exactly, which is the composable choice: a caller that knows the
    // emitter's luminance multiplies by it and gets that luminance back.
    for (const f32 temperature : {1000.0f, 2500.0f, 5772.0f, 12000.0f, 50000.0f})
    {
        const vec3 luminance = LinearRgbToXyz(BlackbodyColor(temperature));
        CHECK(luminance.y == doctest::Approx(1.0f).epsilon(1e-4));

        // Normalizing threw the level away, and the absolute spectrum is where it lives: the
        // same chromaticity comes back out of the unnormalized conversion.
        const vec2 direct = XyzToChromaticity(SpectrumToXyz(BlackbodySpectrum(temperature)));
        const vec2 normalized = XyzToChromaticity(LinearRgbToXyz(BlackbodyColor(temperature)));
        CHECK(normalized.x == doctest::Approx(direct.x).epsilon(1e-5));
        CHECK(normalized.y == doctest::Approx(direct.y).epsilon(1e-5));
    }

    // Eleven orders of magnitude of luminance between the ends of the range, which is why the
    // colour is the primary answer and the radiance is beside it.
    CHECK(SpectrumToXyz(BlackbodySpectrum(BlackbodyMaxTemperature)).y /
              SpectrumToXyz(BlackbodySpectrum(BlackbodyMinTemperature)).y >
          1.0e10f);
}

TEST_CASE("the locus is monotone in temperature with no fold")
{
    // A sampling or normalization error commonly leaves the ends right and folds the middle, so
    // walking the whole range in 100 K steps is the check that catches it. x decreases strictly
    // all the way up; y does not, and must not be asserted on — it peaks near 2500 K, which is
    // the locus curving through its yellow shoulder.
    f32 previousX = 1.0f;
    for (f32 temperature = BlackbodyMinTemperature; temperature <= BlackbodyMaxTemperature;
         temperature += 100.0f)
    {
        const f32 x = BlackbodyChromaticity(temperature).x;
        CHECK(x < previousX);
        previousX = x;
    }

    // Hotter is bluer, said in the working space. Below about 1900 K the blue channel is
    // negative — the colour is outside the primaries' gamut on the red side — so the ratio is
    // only meaningful from where the gamut contains the locus, and this walk starts there.
    CHECK(BlackbodyColor(1500.0f).b < 0.0f);
    CHECK(BlackbodyColor(2000.0f).b > 0.0f);

    f32 previousRatio = -1.0f;
    for (f32 temperature = 2000.0f; temperature <= BlackbodyMaxTemperature; temperature += 100.0f)
    {
        const vec3 rgb = BlackbodyColor(temperature);
        CHECK(rgb.r > 0.0f);
        const f32 ratio = rgb.b / rgb.r;
        CHECK(ratio > previousRatio);
        previousRatio = ratio;
    }
}

TEST_CASE("Wien's displacement law holds for the law itself and at the band's resolution")
{
    // Checking the spectrum rather than its colour, which is what separates a Planck error from
    // a conversion error. Evaluated on a fine sweep, so this is a statement about the law and
    // holds at every temperature — including the ones whose peak is nowhere near the visible.
    for (const f32 temperature : {2000.0f, 3000.0f, 5000.0f, 6500.0f, 10000.0f, 20000.0f})
    {
        const f64 expected = WienDisplacementNmK / static_cast<f64>(temperature);
        f64 peak = 0.0;
        f32 largest = 0.0f;
        for (f64 wavelength = expected * 0.9; wavelength <= expected * 1.1;
             wavelength += expected * 1.0e-4)
        {
            const f32 radiance = PlanckSpectralRadiance(static_cast<f32>(wavelength), temperature);
            if (radiance > largest)
            {
                largest = radiance;
                peak = wavelength;
            }
        }
        CHECK(peak == doctest::Approx(expected).epsilon(1e-3));
    }

    // On the 380-780 nm grid the law's peak is only *inside* the band between b/780 = 3715 K and
    // b/380 = 7626 K, so those are the only temperatures at which the sampled maximum can land
    // on it. Within that window it is the grid sample bracketing the continuous peak, so it
    // differs from b/T by less than one sampling interval.
    CHECK(WienDisplacementNmK / static_cast<f64>(SpectrumMaxWavelength) ==
          doctest::Approx(3715.1).epsilon(1e-4));
    CHECK(WienDisplacementNmK / static_cast<f64>(SpectrumMinWavelength) ==
          doctest::Approx(7625.7).epsilon(1e-4));

    for (const f32 temperature :
         {4000.0f, 4500.0f, 5000.0f, 5500.0f, 6000.0f, 6500.0f, 7000.0f, 7500.0f})
    {
        const f32 sampled = PeakSampleWavelength(BlackbodySpectrum(temperature));
        const f32 expected = static_cast<f32>(WienDisplacementNmK / static_cast<f64>(temperature));
        CHECK(std::abs(sampled - expected) < SpectrumWavelengthStep);
    }

    // Outside the window the in-band maximum is an *endpoint*, not a peak, and asserting it as
    // one is the point: a Wien check at 2000 K would otherwise pass while testing nothing.
    CHECK(PeakSampleWavelength(BlackbodySpectrum(2000.0f)) ==
          doctest::Approx(SpectrumMaxWavelength));
    CHECK(PeakSampleWavelength(BlackbodySpectrum(3000.0f)) ==
          doctest::Approx(SpectrumMaxWavelength));
    CHECK(PeakSampleWavelength(BlackbodySpectrum(10000.0f)) ==
          doctest::Approx(SpectrumMinWavelength));
    CHECK(PeakSampleWavelength(BlackbodySpectrum(30000.0f)) ==
          doctest::Approx(SpectrumMinWavelength));
}

TEST_CASE("the emitted fraction below a wavelength matches the published radiation function")
{
    // The standard blackbody radiation-function table, indexed by lambda * T in micrometre
    // kelvin. Pinning the series against it first is what makes it usable as an independent
    // reference in the Stefan-Boltzmann case below.
    struct FractionPoint
    {
        f64 WavelengthTimesTemperature; // micrometre kelvin
        f64 Fraction;
    };
    constexpr FractionPoint Published[] = {
        {.WavelengthTimesTemperature = 2000.0, .Fraction = 0.066728},
        {.WavelengthTimesTemperature = 2898.0, .Fraction = 0.250108},
        {.WavelengthTimesTemperature = 4000.0, .Fraction = 0.480877},
        {.WavelengthTimesTemperature = 6000.0, .Fraction = 0.737818},
        {.WavelengthTimesTemperature = 10000.0, .Fraction = 0.914199},
    };

    for (const FractionPoint& point : Published)
    {
        // One micrometre kelvin is 1000 nanometre kelvin; hold the temperature at 1000 K and the
        // wavelength in nanometres reads directly as the tabulated product.
        const f64 fraction = EmittedFractionBelow(point.WavelengthTimesTemperature, 1000.0);
        CHECK(std::abs(fraction - point.Fraction) < 1.0e-4);
    }

    // A quarter of a blackbody's emission lies below its own peak wavelength, whatever its
    // temperature — the 2898 row above, restated as the property it encodes.
    CHECK(EmittedFractionBelow(WienDisplacementNmK / 5000.0, 5000.0) ==
          doctest::Approx(0.2501).epsilon(1e-3));
}

TEST_CASE("the absolute radiance obeys Stefan-Boltzmann")
{
    // The assertion exists to catch a units error in the radiance form, which normalization
    // otherwise hides completely, so it has to compare against something with units in it. Two
    // independent comparisons, because the obvious one is wrong.
    //
    // First: integrate the law over the whole spectrum and compare against sigma T^4 / pi from
    // the published constant. This is the honest T^4 statement — the total goes as T^4, and it
    // is the total that does.
    for (const f32 temperature : {1000.0f, 2000.0f, 5772.0f, 20000.0f, 50000.0f})
    {
        const f64 expected =
            StefanBoltzmann * std::pow(static_cast<f64>(temperature), 4.0) / std::numbers::pi;
        CHECK(TotalRadianceByQuadrature(temperature, 2000) ==
              doctest::Approx(expected).epsilon(1e-4));
        CHECK(static_cast<f64>(BlackbodyTotalRadiance(temperature)) ==
              doctest::Approx(expected).epsilon(1e-5));
    }

    // Second: the *band's* share is not proportional to T^4 and must not be asserted as if it
    // were. The share is the difference of two radiation-function values, and it is strongly
    // temperature dependent — 1.7% of the total at 2000 K, 48.7% at 6500 K, 1.4% again at
    // 50000 K. Comparing the band integral against sigma T^4 / pi times that fraction is what
    // makes this a units check rather than a tautology.
    for (const f32 temperature : {2000.0f, 3000.0f, 5000.0f, 6500.0f, 10000.0f, 20000.0f, 50000.0f})
    {
        const f64 kelvin = static_cast<f64>(temperature);
        const f64 share = EmittedFractionBelow(static_cast<f64>(SpectrumMaxWavelength), kelvin) -
                          EmittedFractionBelow(static_cast<f64>(SpectrumMinWavelength), kelvin);
        const f64 expected = StefanBoltzmann * std::pow(kelvin, 4.0) / std::numbers::pi * share;
        CHECK(BandRadiance(BlackbodySpectrum(temperature)) ==
              doctest::Approx(expected).epsilon(1e-3));
    }

    // The share peaks in the middle of the range and falls away at both ends, which is the fact
    // the naive "band radiance goes as T^4" reading gets wrong.
    const f64 cool = EmittedFractionBelow(780.0, 2000.0) - EmittedFractionBelow(380.0, 2000.0);
    const f64 middling = EmittedFractionBelow(780.0, 6000.0) - EmittedFractionBelow(380.0, 6000.0);
    const f64 hot = EmittedFractionBelow(780.0, 50000.0) - EmittedFractionBelow(380.0, 50000.0);
    CHECK(middling > cool * 10.0);
    CHECK(middling > hot * 10.0);
}

TEST_CASE("outside the valid range the documented clamp happens")
{
    // Clamping rather than extrapolating, asserted rather than left to discovery. An unclamped
    // evaluation far below the range underflows the visible band toward zero and yields a
    // chromaticity that is meaningless rather than merely dim.
    CHECK(BlackbodyChromaticity(100.0f).x ==
          doctest::Approx(BlackbodyChromaticity(BlackbodyMinTemperature).x));
    CHECK(BlackbodyChromaticity(0.0f).y ==
          doctest::Approx(BlackbodyChromaticity(BlackbodyMinTemperature).y));
    CHECK(BlackbodyColor(-5.0f).r == doctest::Approx(BlackbodyColor(BlackbodyMinTemperature).r));
    CHECK(BlackbodyChromaticity(1.0e9f).x ==
          doctest::Approx(BlackbodyChromaticity(BlackbodyMaxTemperature).x));
    CHECK(BlackbodySpectrum(500.0f).Samples[40] ==
          doctest::Approx(BlackbodySpectrum(BlackbodyMinTemperature).Samples[40]));

    // The law itself is not clamped: it is evaluated wherever it is asked, which is what makes
    // the Wien and Stefan-Boltzmann checks above possible at all.
    CHECK(PlanckSpectralRadiance(10000.0f, 300.0f) > 0.0f);
    CHECK(PlanckSpectralRadiance(550.0f, 100.0f) == doctest::Approx(0.0f));
}
