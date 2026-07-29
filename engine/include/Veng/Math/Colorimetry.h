#pragma once

#include <array>

#include <Veng/Veng.h>

/// @brief Spectral colorimetry — a sampled spectrum, the CIE 1931 standard observer, and the
/// conversion from a spectral curve to a linear RGB triple in the renderer's working space.
///
/// A transfer function moves a colour between encodings; it cannot produce one. Turning a
/// physical measurement into a colour takes a different path: sample the quantity across the
/// visible band, integrate it against the standard observer's three colour-matching functions
/// to CIE XYZ, then convert XYZ to the working primaries. Light-source colour temperature,
/// emissive output, a participating medium's scattering and absorption, and physically
/// authored reflectance all reach a renderer through that one path.
///
/// Everything here is CPU-side, device-free and allocation-free, and is meant to run once per
/// material or per light rather than per pixel — the result is an ordinary RGB triple.
namespace Veng
{
    /// @brief Shortest wavelength the sampling grid covers, in nanometres.
    ///
    /// The grid is CIE 15:2004's own tabulation range for both the standard observer
    /// (Table T.4) and the D illuminants (Table T.1), so the observer and an illuminant are
    /// defined over exactly the same wavelengths and neither is extrapolated. The observer's
    /// published tails outside it are negligible: 360-380 nm and 780-830 nm together carry
    /// under 0.03% of each colour-matching function's area.
    inline constexpr f32 SpectrumMinWavelength = 380.0f;

    /// @brief Longest wavelength the sampling grid covers, in nanometres.
    inline constexpr f32 SpectrumMaxWavelength = 780.0f;

    /// @brief Spacing between adjacent samples, in nanometres — the CIE publication interval.
    inline constexpr f32 SpectrumWavelengthStep = 5.0f;

    /// @brief Number of samples spanning the band inclusive of both endpoints.
    inline constexpr u32 SpectrumSampleCount = 81;

    /// @brief A spectral quantity sampled uniformly across the visible band.
    ///
    /// Sample `i` is the quantity's value at `WavelengthAt(i)`; the samples are point values on
    /// the grid, not bin averages, and the integrations below are the corresponding Riemann sum.
    /// What the values mean is the caller's: a reflectance is dimensionless in [0, 1], an
    /// emitter's spectral radiance carries whatever radiometric unit the caller works in, and an
    /// illuminant is conventionally relative (D65 is 100 at 560 nm).
    ///
    /// The arithmetic is deliberately narrow — a scale, and a per-wavelength product with another
    /// spectrum, which is what "this light through this material" means. It is not a general
    /// vector type.
    struct Spectrum
    {
        /// @brief The per-wavelength samples, index `i` at `WavelengthAt(i)`.
        std::array<f32, SpectrumSampleCount> Samples;

        /// @brief Returns the wavelength in nanometres that sample `index` is taken at.
        /// @param index  Sample index; values at or past SpectrumSampleCount are caller error.
        /// @return The sample's wavelength, `SpectrumMinWavelength + index * SpectrumWavelengthStep`.
        [[nodiscard]] static constexpr f32 WavelengthAt(u32 index)
        {
            return SpectrumMinWavelength + (static_cast<f32>(index) * SpectrumWavelengthStep);
        }

        /// @brief Returns the flat spectrum holding `value` at every wavelength.
        ///
        /// Constant(1) is the equal-energy illuminant E as well as the perfect diffuse
        /// reflector, which is why it is the reference both normalizations are stated against.
        /// @param value  The value every sample takes.
        [[nodiscard]] static constexpr Spectrum Constant(f32 value)
        {
            Spectrum spectrum{};
            for (f32& sample : spectrum.Samples)
            {
                sample = value;
            }
            return spectrum;
        }

        /// @brief Returns the all-zero spectrum.
        [[nodiscard]] static constexpr Spectrum Zero() { return Constant(0.0f); }

        /// @brief Scales every sample in place.
        /// @param scale  Factor applied to each sample.
        /// @return This spectrum.
        constexpr Spectrum& operator*=(f32 scale)
        {
            for (f32& sample : Samples)
            {
                sample *= scale;
            }
            return *this;
        }

        /// @brief Multiplies wavelength by wavelength in place.
        /// @param other  The spectrum multiplied in, sample for sample.
        /// @return This spectrum.
        constexpr Spectrum& operator*=(const Spectrum& other)
        {
            for (u32 i = 0; i < SpectrumSampleCount; ++i)
            {
                Samples[i] *= other.Samples[i];
            }
            return *this;
        }
    };

    /// @brief Returns `spectrum` with every sample scaled by `scale`.
    /// @param spectrum  The spectrum scaled.
    /// @param scale     Factor applied to each sample.
    [[nodiscard]] constexpr Spectrum operator*(Spectrum spectrum, f32 scale)
    {
        spectrum *= scale;
        return spectrum;
    }

    /// @brief Returns `spectrum` with every sample scaled by `scale`.
    /// @param scale     Factor applied to each sample.
    /// @param spectrum  The spectrum scaled.
    [[nodiscard]] constexpr Spectrum operator*(f32 scale, Spectrum spectrum)
    {
        spectrum *= scale;
        return spectrum;
    }

    /// @brief Returns the wavelength-by-wavelength product of two spectra.
    ///
    /// This is the operation behind "this illuminant through this material": an illuminant's
    /// spectral power times a reflectance is the reflected spectral power.
    /// @param lhs  Left operand.
    /// @param rhs  Right operand.
    [[nodiscard]] constexpr Spectrum operator*(Spectrum lhs, const Spectrum& rhs)
    {
        lhs *= rhs;
        return lhs;
    }

    /// @brief Returns the CIE 1931 2-degree standard observer's x-bar colour-matching function.
    ///
    /// From CIE 15:2004 Table T.4, transcribed at the published 5 nm interval and precision.
    [[nodiscard]] VE_API const Spectrum& CieObserverX();

    /// @brief Returns the CIE 1931 2-degree standard observer's y-bar colour-matching function.
    ///
    /// y-bar is also the CIE 1924 photopic luminous efficiency function V(lambda), which is why
    /// Y alone carries luminance. From CIE 15:2004 Table T.4.
    [[nodiscard]] VE_API const Spectrum& CieObserverY();

    /// @brief Returns the CIE 1931 2-degree standard observer's z-bar colour-matching function.
    ///
    /// From CIE 15:2004 Table T.4.
    [[nodiscard]] VE_API const Spectrum& CieObserverZ();

    /// @brief Returns the relative spectral power distribution of CIE standard illuminant D65.
    ///
    /// Relative power, 100 at 560 nm, from CIE 15:2004 Table T.1. D65 is the white point the
    /// working primaries below are defined against, so it is the illuminant a reflectance is
    /// converted under unless a caller has a specific reason to pick another.
    [[nodiscard]] VE_API const Spectrum& CieIlluminantD65();

    /// @brief CIE 1931 xy chromaticity of the sRGB / Rec.709 red primary.
    inline constexpr vec2 SrgbRedPrimary{0.6400f, 0.3300f};

    /// @brief CIE 1931 xy chromaticity of the sRGB / Rec.709 green primary.
    inline constexpr vec2 SrgbGreenPrimary{0.3000f, 0.6000f};

    /// @brief CIE 1931 xy chromaticity of the sRGB / Rec.709 blue primary.
    inline constexpr vec2 SrgbBluePrimary{0.1500f, 0.0600f};

    /// @brief CIE 1931 xy chromaticity of the D65 white point, as sRGB / Rec.709 state it.
    ///
    /// The rounded value the standards carry, not the one integrating CieIlluminantD65()
    /// against the observer yields (0.312721, 0.329031); the two agree to five decimals.
    inline constexpr vec2 SrgbWhitePoint{0.3127f, 0.3290f};

    /// @brief Integrates a spectral quantity against the standard observer, preserving level.
    ///
    /// The absolute conversion: XYZ is the Riemann sum of the quantity against each
    /// colour-matching function, times the sampling interval, so doubling the input doubles Y.
    /// This is what an **emitter** wants — a light source, an emissive material, anything whose
    /// magnitude is itself the measurement. No photometric constant is applied, so Y is in the
    /// caller's own radiometric unit times nanometres and the caller owns the scale that puts it
    /// where the renderer's exposure expects it.
    ///
    /// A **reflectance** wants ReflectanceToXyz instead: a reflectance has no level of its own,
    /// and converting one through here yields a triple scaled by whatever the equal-energy
    /// integral happens to be.
    /// @param radiance  The emitted spectral quantity.
    /// @return The CIE 1931 XYZ tristimulus values.
    /// @see ReflectanceToXyz
    [[nodiscard]] VE_API vec3 SpectrumToXyz(const Spectrum& radiance);

    /// @brief Converts a reflectance spectrum under an illuminant, normalized to that illuminant.
    ///
    /// The relative conversion: the integral is divided by the illuminant's own luminance
    /// integral, so a perfect diffuse reflector comes out at Y = 1 with the illuminant's
    /// chromaticity — white, by construction, whatever the illuminant's absolute power. A flat
    /// 50% reflector comes out at Y = 0.5. This is what a **material** wants; an emitter wants
    /// SpectrumToXyz, whose level is meaningful.
    /// @param reflectance  Dimensionless reflectance, conventionally in [0, 1].
    /// @param illuminant   The illuminant it is lit by, in any consistent relative unit.
    /// @return The CIE 1931 XYZ tristimulus values, with Y = 1 for a perfect diffuse reflector.
    /// @see SpectrumToXyz
    [[nodiscard]] VE_API vec3 ReflectanceToXyz(const Spectrum& reflectance,
                                               const Spectrum& illuminant);

    /// @brief Projects XYZ onto the CIE 1931 xy chromaticity plane.
    /// @param xyz  Tristimulus values.
    /// @return The chromaticity (x, y); (0, 0) when the tristimulus sum is zero.
    [[nodiscard]] VE_API vec2 XyzToChromaticity(vec3 xyz);

    /// @brief Reconstructs XYZ from a chromaticity and a luminance.
    /// @param chromaticity  CIE 1931 (x, y). A zero y is caller error and yields zero.
    /// @param luminance     The Y the result carries.
    /// @return The tristimulus values with that chromaticity and luminance.
    [[nodiscard]] VE_API vec3 ChromaticityToXyz(vec2 chromaticity, f32 luminance);

    /// @brief Converts XYZ to linear RGB in the renderer's working space.
    ///
    /// The working space is **linear sRGB / Rec.709 with a D65 white point** — the primaries the
    /// engine's output path is built on. An SDR swapchain presents an sRGB-nonlinear surface the
    /// composite writes linear values into; the HDR10 path converts Rec.709 to Rec.2020 on the
    /// way out, which only makes sense if what it receives is Rec.709; and the exposure and bloom
    /// passes weight channels by Rec.709 relative luminance. The matrix is derived from the
    /// primary and white-point chromaticities above rather than transcribed.
    ///
    /// The result is not clamped: a saturated spectral colour lies outside the primaries' gamut
    /// and yields a negative channel, which is a real fact about the colour rather than an error.
    /// @param xyz  Tristimulus values.
    /// @return Linear RGB, unclamped.
    [[nodiscard]] VE_API vec3 XyzToLinearRgb(vec3 xyz);

    /// @brief Converts linear RGB in the renderer's working space back to XYZ.
    /// @param rgb  Linear RGB in the same space XyzToLinearRgb produces.
    /// @return Tristimulus values.
    /// @see XyzToLinearRgb
    [[nodiscard]] VE_API vec3 LinearRgbToXyz(vec3 rgb);

    /// @brief Coldest temperature the band-sampled blackbody functions are defined at, in kelvin.
    ///
    /// The lower end of the range the CIE tabulates the Planckian locus over, and roughly a
    /// flame. Colder than this a blackbody's visible-band radiance falls away toward the limit
    /// of what a float carries, and its chromaticity — already outside the working primaries'
    /// gamut on the red side — stops meaning anything. BlackbodySpectrum, BlackbodyChromaticity
    /// and BlackbodyColor clamp to this rather than extrapolate.
    inline constexpr f32 BlackbodyMinTemperature = 1000.0f;

    /// @brief Hottest temperature the band-sampled blackbody functions are defined at, in kelvin.
    ///
    /// Past the hottest thermal emitters, and far enough up the locus that the chromaticity has
    /// converged: 50000 K sits within 0.001 of the infinite-temperature limit in both x and y,
    /// so clamping here discards no colour anyone can see.
    inline constexpr f32 BlackbodyMaxTemperature = 50000.0f;

    /// @brief Planck's law: the spectral radiance of an ideal blackbody at one wavelength.
    ///
    /// `2hc^2 / (lambda^5 (exp(hc / (lambda k T)) - 1))`, evaluated in double and returned in
    /// **W sr^-1 m^-2 nm^-1** — per nanometre, to match the wavelength argument's unit. The
    /// constants are CODATA: h, c and k are SI-exact since the 2019 redefinition.
    ///
    /// This is the law itself and is **not** clamped to the temperature range above: a caller
    /// integrating outside the visible band, or checking the law against Wien's displacement or
    /// Stefan-Boltzmann, has every reason to evaluate it anywhere. A temperature cold enough to
    /// overflow the exponential returns zero rather than a denormal.
    /// @param wavelengthNm  Wavelength in nanometres; must be positive.
    /// @param temperatureK  Absolute temperature in kelvin; must be positive.
    /// @return Spectral radiance in W sr^-1 m^-2 nm^-1.
    [[nodiscard]] VE_API f32 PlanckSpectralRadiance(f32 wavelengthNm, f32 temperatureK);

    /// @brief Stefan-Boltzmann: an ideal blackbody's radiance summed over all wavelengths.
    ///
    /// `sigma T^4 / pi`, in **W sr^-1 m^-2** — the integral of PlanckSpectralRadiance over the
    /// whole spectrum, which is the absolute level a caller scales BlackbodyColor by when it
    /// wants the emitter's real magnitude rather than its colour. Unclamped, like the law it
    /// integrates. The visible band's share of it is not proportional to T^4: the fraction of the
    /// curve falling inside 380-780 nm itself moves with temperature, peaking near 6000 K.
    /// @param temperatureK  Absolute temperature in kelvin; must be non-negative.
    /// @return Total radiance in W sr^-1 m^-2.
    [[nodiscard]] VE_API f32 BlackbodyTotalRadiance(f32 temperatureK);

    /// @brief Samples Planck's law onto the band as absolute spectral radiance.
    ///
    /// Every sample is PlanckSpectralRadiance at that sample's wavelength, so the spectrum
    /// carries the emitter's true magnitude and SpectrumToXyz is the conversion it wants. Across
    /// the temperature range that magnitude spans sixteen orders of magnitude at 380 nm and
    /// eleven in luminance, which is why BlackbodyColor rather than this is what a consumer
    /// usually wants.
    /// @param temperatureK  Absolute temperature in kelvin, clamped to
    ///                      [BlackbodyMinTemperature, BlackbodyMaxTemperature].
    /// @return Spectral radiance in W sr^-1 m^-2 nm^-1 at each sampled wavelength.
    [[nodiscard]] VE_API Spectrum BlackbodySpectrum(f32 temperatureK);

    /// @brief Returns a blackbody's CIE 1931 chromaticity — a point on the Planckian locus.
    ///
    /// The colour with the level divided out, and the form the locus is published in, so it is
    /// the value to check an implementation or a table against.
    ///
    /// **An ideal radiator is not a measured source.** A real thermal emitter departs from the
    /// locus — line absorption, self-absorption in a cool outer layer, molecular bands, and a
    /// non-uniform emitting surface all move its colour — and an astronomical source's published
    /// colour index reflects that. This is the ideal blackbody, which is the right model for a
    /// colour-temperature knob and a first-order model for a thermal emitter, not a substitute
    /// for a measured spectrum.
    /// @param temperatureK  Absolute temperature in kelvin, clamped to
    ///                      [BlackbodyMinTemperature, BlackbodyMaxTemperature].
    /// @return CIE 1931 (x, y) on the Planckian locus.
    [[nodiscard]] VE_API vec2 BlackbodyChromaticity(f32 temperatureK);

    /// @brief Returns a blackbody's colour in the working space, normalized to unit luminance.
    ///
    /// **Normalized means Y = 1 exactly**: the triple's Rec.709 relative luminance is one, so
    /// multiplying it by a luminance in the caller's own unit yields a triple carrying that
    /// luminance. That is the composable normalization — level is owned by whatever knows the
    /// emitter's output, and BlackbodyTotalRadiance is available when that is the blackbody's
    /// own. Handing a caller the absolute radiance instead would have it multiply an already
    /// enormous number by its own luminance term.
    ///
    /// Individual channels routinely leave [0, 1] in both directions, and below about 1900 K the
    /// blue channel is **negative**: a deep red blackbody is outside the working primaries' gamut,
    /// which XyzToLinearRgb reports rather than clamps away. The same ideal-radiator caveat as
    /// BlackbodyChromaticity applies.
    /// @param temperatureK  Absolute temperature in kelvin, clamped to
    ///                      [BlackbodyMinTemperature, BlackbodyMaxTemperature].
    /// @return Unclamped linear RGB with Y = 1.
    /// @see BlackbodyChromaticity
    /// @see BlackbodyTotalRadiance
    [[nodiscard]] VE_API vec3 BlackbodyColor(f32 temperatureK);
}
