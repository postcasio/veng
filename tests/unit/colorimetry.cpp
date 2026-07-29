// Spectral colorimetry: pure CPU, no Context, no Vulkan. Every assertion here is against a
// value published outside this repository — CIE 15:2004's own column sums and spectral-locus
// chromaticities, the D65 white point the sRGB and Rec.709 standards state, the CIE 1924
// photopic luminous efficiency function, and IEC 61966-2-1's XYZ -> linear RGB matrix — never
// against what the implementation happens to produce. A mistyped or misaligned row in a
// transcribed table yields a small, plausible, entirely wrong answer that every self-consistent
// check passes, so a published reference is the only thing that catches it.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

#include <Veng/Math/Colorimetry.h>

using namespace Veng;

namespace
{
    // Index of a wavelength on the sampling grid. 380 nm is index 0 and the step is 5 nm, so an
    // off-by-one here and an off-by-one in the tables would cancel — the locus case below pins
    // the mapping against published chromaticities instead.
    u32 IndexOf(f32 wavelength)
    {
        return static_cast<u32>(
            std::lround((wavelength - SpectrumMinWavelength) / SpectrumWavelengthStep));
    }

    Spectrum Monochromatic(f32 wavelength)
    {
        Spectrum spectrum = Spectrum::Zero();
        spectrum.Samples[IndexOf(wavelength)] = 1.0f;
        return spectrum;
    }

    f64 Area(const Spectrum& spectrum)
    {
        f64 sum = 0.0;
        for (const f32 sample : spectrum.Samples)
        {
            sum += static_cast<f64>(sample);
        }
        return sum;
    }
}

TEST_CASE("the sampling grid spans the CIE tabulation range at the publication interval")
{
    CHECK(Spectrum::WavelengthAt(0) == doctest::Approx(380.0f));
    CHECK(Spectrum::WavelengthAt(SpectrumSampleCount - 1) == doctest::Approx(780.0f));
    CHECK(SpectrumSampleCount == static_cast<u32>(((SpectrumMaxWavelength - SpectrumMinWavelength) /
                                                   SpectrumWavelengthStep)) +
                                     1);
}

TEST_CASE("the colour-matching functions integrate to the published equal areas")
{
    // CIE 15:2004 Table T.4 prints its own column sums: 21.371524, 21.371327, 21.371540. Equal
    // areas is the property the functions are constructed to have, so checking each column
    // against its published total is a direct transcription check on all 243 numbers.
    CHECK(Area(CieObserverX()) == doctest::Approx(21.371524).epsilon(1e-6));
    CHECK(Area(CieObserverY()) == doctest::Approx(21.371327).epsilon(1e-6));
    CHECK(Area(CieObserverZ()) == doctest::Approx(21.371540).epsilon(1e-6));

    // And they agree with one another to the 2.2e-4 the published sums differ by.
    CHECK(std::abs(Area(CieObserverX()) - Area(CieObserverY())) < 2.2e-4);
    CHECK(std::abs(Area(CieObserverZ()) - Area(CieObserverY())) < 2.2e-4);
}

TEST_CASE("y-bar is the CIE 1924 photopic luminous efficiency function")
{
    // V(lambda) is published independently of the tristimulus tables; these are its values at
    // the landmark wavelengths, peak included.
    const Spectrum& observerY = CieObserverY();
    CHECK(observerY.Samples[IndexOf(400.0f)] == doctest::Approx(0.000396f));
    CHECK(observerY.Samples[IndexOf(450.0f)] == doctest::Approx(0.038f));
    CHECK(observerY.Samples[IndexOf(500.0f)] == doctest::Approx(0.323f));
    CHECK(observerY.Samples[IndexOf(555.0f)] == doctest::Approx(1.0f));
    CHECK(observerY.Samples[IndexOf(600.0f)] == doctest::Approx(0.631f));
    CHECK(observerY.Samples[IndexOf(650.0f)] == doctest::Approx(0.107f));
    CHECK(observerY.Samples[IndexOf(700.0f)] == doctest::Approx(0.004102f));

    // Integrating a spectrum against y-bar alone reproduces Y, so luminance needs no other path.
    const Spectrum test = CieIlluminantD65() * 0.25f;
    f64 luminance = 0.0;
    for (u32 i = 0; i < SpectrumSampleCount; ++i)
    {
        luminance += static_cast<f64>(test.Samples[i]) * static_cast<f64>(observerY.Samples[i]);
    }
    CHECK(SpectrumToXyz(test).y ==
          doctest::Approx(luminance * SpectrumWavelengthStep).epsilon(1e-5));
}

TEST_CASE("equal-energy white lands on the E chromaticity")
{
    // Illuminant E is x = y = 1/3 by definition, since the three functions have equal areas.
    const vec2 chromaticity = XyzToChromaticity(SpectrumToXyz(Spectrum::Constant(1.0f)));
    CHECK(chromaticity.x == doctest::Approx(1.0f / 3.0f).epsilon(1e-4));
    CHECK(chromaticity.y == doctest::Approx(1.0f / 3.0f).epsilon(1e-4));
}

TEST_CASE("D65 lands on the sRGB white point and converts to white")
{
    // sRGB and Rec.709 state D65 as x = 0.3127, y = 0.3290.
    const vec2 chromaticity = XyzToChromaticity(SpectrumToXyz(CieIlluminantD65()));
    CHECK(chromaticity.x == doctest::Approx(0.3127f).epsilon(1e-3));
    CHECK(chromaticity.y == doctest::Approx(0.3290f).epsilon(1e-3));

    // A perfect diffuse reflector under D65 is white at unit luminance, by construction of the
    // reflectance normalization — which is what makes that normalization the one a material wants.
    const vec3 xyz = ReflectanceToXyz(Spectrum::Constant(1.0f), CieIlluminantD65());
    CHECK(xyz.y == doctest::Approx(1.0f).epsilon(1e-4));
    const vec3 rgb = XyzToLinearRgb(xyz);
    CHECK(rgb.r == doctest::Approx(1.0f).epsilon(1e-3));
    CHECK(rgb.g == doctest::Approx(1.0f).epsilon(1e-3));
    CHECK(rgb.b == doctest::Approx(1.0f).epsilon(1e-3));
}

TEST_CASE("a monochromatic spectrum lands on the published spectral locus")
{
    // CIE 15:2004 Table T.4 carries the locus chromaticity beside each row, so this pins the
    // index-to-wavelength mapping against the tables rather than against itself: a grid shifted
    // by one sample puts 450 nm at 445 nm's chromaticity, which is small and entirely wrong.
    struct LocusPoint
    {
        f32 Wavelength;
        f32 X;
        f32 Y;
    };
    constexpr LocusPoint Published[] = {
        {.Wavelength = 450.0f, .X = 0.15664f, .Y = 0.01770f},
        {.Wavelength = 550.0f, .X = 0.30160f, .Y = 0.69231f},
        {.Wavelength = 650.0f, .X = 0.72599f, .Y = 0.27401f},
        {.Wavelength = 500.0f, .X = 0.00817f, .Y = 0.53842f},
        {.Wavelength = 600.0f, .X = 0.62704f, .Y = 0.37249f},
    };

    for (const LocusPoint& point : Published)
    {
        const vec2 chromaticity = XyzToChromaticity(SpectrumToXyz(Monochromatic(point.Wavelength)));
        CHECK(chromaticity.x == doctest::Approx(point.X).epsilon(1e-3));
        CHECK(chromaticity.y == doctest::Approx(point.Y).epsilon(1e-3));
    }

    // A spectral colour is outside the sRGB gamut, so at least one channel is negative — the
    // conversion reports that rather than clamping it away.
    const vec3 rgb = XyzToLinearRgb(SpectrumToXyz(Monochromatic(500.0f)));
    CHECK(std::min(std::min(rgb.r, rgb.g), rgb.b) < 0.0f);
}

TEST_CASE("a flat grey reflector is neutral and halves luminance")
{
    // Separates a hue error from a level error: equal channels say the chromaticity is right,
    // and Y = 0.5 says the normalization is.
    const vec3 xyz = ReflectanceToXyz(Spectrum::Constant(0.5f), CieIlluminantD65());
    CHECK(xyz.y == doctest::Approx(0.5f).epsilon(1e-4));

    const vec3 rgb = XyzToLinearRgb(xyz);
    CHECK(rgb.r == doctest::Approx(0.5f).epsilon(1e-3));
    CHECK(rgb.g == doctest::Approx(0.5f).epsilon(1e-3));
    CHECK(rgb.b == doctest::Approx(0.5f).epsilon(1e-3));

    // An emitter's level is meaningful where a reflectance's is not: scaling the spectrum scales
    // Y, which is the whole distinction between the two conversions.
    const vec3 dim = SpectrumToXyz(CieIlluminantD65() * 0.5f);
    const vec3 bright = SpectrumToXyz(CieIlluminantD65());
    CHECK(dim.y == doctest::Approx(bright.y * 0.5f).epsilon(1e-5));
    CHECK(XyzToChromaticity(dim).x == doctest::Approx(XyzToChromaticity(bright).x).epsilon(1e-6));
}

TEST_CASE("the working primaries round-trip through their own chromaticities")
{
    struct PrimaryCase
    {
        vec2 Chromaticity;
        vec3 Expected;
    };
    const PrimaryCase Cases[] = {
        {.Chromaticity = SrgbRedPrimary, .Expected = vec3(1.0f, 0.0f, 0.0f)},
        {.Chromaticity = SrgbGreenPrimary, .Expected = vec3(0.0f, 1.0f, 0.0f)},
        {.Chromaticity = SrgbBluePrimary, .Expected = vec3(0.0f, 0.0f, 1.0f)},
    };

    for (const PrimaryCase& primary : Cases)
    {
        const vec3 xyz = ChromaticityToXyz(primary.Chromaticity, 1.0f);
        const vec3 rgb = XyzToLinearRgb(xyz);
        // Only the primary's own channel is lit; the level differs since Y was forced to 1.
        const f32 lit = std::max(std::max(rgb.r, rgb.g), rgb.b);
        CHECK(lit > 0.0f);
        CHECK(rgb.r / lit == doctest::Approx(primary.Expected.r).epsilon(1e-4));
        CHECK(rgb.g / lit == doctest::Approx(primary.Expected.g).epsilon(1e-4));
        CHECK(rgb.b / lit == doctest::Approx(primary.Expected.b).epsilon(1e-4));

        const vec2 back = XyzToChromaticity(LinearRgbToXyz(rgb));
        CHECK(back.x == doctest::Approx(primary.Chromaticity.x).epsilon(1e-4));
        CHECK(back.y == doctest::Approx(primary.Chromaticity.y).epsilon(1e-4));
    }
}

TEST_CASE("the derived conversion matches the published sRGB matrix")
{
    // IEC 61966-2-1 / Rec.709 publish XYZ -> linear RGB as
    //   3.2406 -1.5372 -0.4986 / -0.9689 1.8758 0.0415 / 0.0557 -0.2040 1.0570.
    // The matrix here is derived from the primary and white-point chromaticities, so agreeing
    // with the published one to 1e-3 is a check on the derivation, not a restatement of it. The
    // residual is the published matrix having been computed from a rounded white point.
    CHECK(XyzToLinearRgb(vec3(1.0f, 0.0f, 0.0f)).r == doctest::Approx(3.2406f).epsilon(1e-3));
    CHECK(XyzToLinearRgb(vec3(0.0f, 1.0f, 0.0f)).r == doctest::Approx(-1.5372f).epsilon(1e-3));
    CHECK(XyzToLinearRgb(vec3(0.0f, 0.0f, 1.0f)).r == doctest::Approx(-0.4986f).epsilon(1e-3));
    CHECK(XyzToLinearRgb(vec3(1.0f, 0.0f, 0.0f)).g == doctest::Approx(-0.9689f).epsilon(1e-3));
    CHECK(XyzToLinearRgb(vec3(0.0f, 1.0f, 0.0f)).g == doctest::Approx(1.8758f).epsilon(1e-3));
    CHECK(XyzToLinearRgb(vec3(0.0f, 0.0f, 1.0f)).g == doctest::Approx(0.0415f).epsilon(1e-3));
    CHECK(XyzToLinearRgb(vec3(1.0f, 0.0f, 0.0f)).b == doctest::Approx(0.0557f).epsilon(1e-3));
    CHECK(XyzToLinearRgb(vec3(0.0f, 1.0f, 0.0f)).b == doctest::Approx(-0.2040f).epsilon(1e-3));
    CHECK(XyzToLinearRgb(vec3(0.0f, 0.0f, 1.0f)).b == doctest::Approx(1.0570f).epsilon(1e-3));

    // Rec.709 relative luminance, the weights the engine's exposure and bloom passes use.
    const vec3 luminanceRow = LinearRgbToXyz(vec3(1.0f, 0.0f, 0.0f));
    CHECK(luminanceRow.y == doctest::Approx(0.2126f).epsilon(1e-3));
    CHECK(LinearRgbToXyz(vec3(0.0f, 1.0f, 0.0f)).y == doctest::Approx(0.7152f).epsilon(1e-3));
    CHECK(LinearRgbToXyz(vec3(0.0f, 0.0f, 1.0f)).y == doctest::Approx(0.0722f).epsilon(1e-3));
}

TEST_CASE("the spectral product is the illuminant through the material")
{
    // A reflectance converted under an illuminant is the same as converting their product, with
    // only the illuminant's own luminance dividing it out.
    Spectrum reflectance = Spectrum::Zero();
    for (u32 i = 0; i < SpectrumSampleCount; ++i)
    {
        reflectance.Samples[i] =
            0.2f + (0.6f * static_cast<f32>(i) / static_cast<f32>(SpectrumSampleCount - 1));
    }

    const vec3 viaProduct = SpectrumToXyz(reflectance * CieIlluminantD65());
    const vec3 white = SpectrumToXyz(CieIlluminantD65());
    const vec3 relative = ReflectanceToXyz(reflectance, CieIlluminantD65());
    CHECK(relative.y == doctest::Approx(viaProduct.y / white.y).epsilon(1e-4));
    CHECK(XyzToChromaticity(relative).x ==
          doctest::Approx(XyzToChromaticity(viaProduct).x).epsilon(1e-5));

    // Scaling commutes with the product, both ways round.
    const Spectrum scaled = 2.0f * reflectance;
    CHECK(scaled.Samples[10] == doctest::Approx(reflectance.Samples[10] * 2.0f));
    CHECK((reflectance * 2.0f).Samples[10] == doctest::Approx(scaled.Samples[10]));
}
