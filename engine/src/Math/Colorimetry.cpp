#include <Veng/Math/Colorimetry.h>

namespace Veng
{
    namespace
    {
        // The CIE 1931 2-degree standard colorimetric observer, 380-780 nm at 5 nm, transcribed
        // from CIE 15:2004 "Colorimetry, 3rd edition", Table T.4, at the precision published
        // there. The published column sums are 21.371524 / 21.371327 / 21.371540 — the equal-area
        // property the functions are constructed to have, and the check that catches a mistyped
        // row, which is otherwise invisible.
        constexpr Spectrum ObserverX{{
            0.001368f, 0.002236f, 0.004243f, 0.00765f,  0.01431f,  0.02319f,  0.04351f,  0.07763f,
            0.13438f,  0.21477f,  0.2839f,   0.3285f,   0.34828f,  0.34806f,  0.3362f,   0.3187f,
            0.2908f,   0.2511f,   0.19536f,  0.1421f,   0.09564f,  0.05795f,  0.03201f,  0.0147f,
            0.0049f,   0.0024f,   0.0093f,   0.0291f,   0.06327f,  0.1096f,   0.1655f,   0.22575f,
            0.2904f,   0.3597f,   0.43345f,  0.51205f,  0.5945f,   0.6784f,   0.7621f,   0.8425f,
            0.9163f,   0.9786f,   1.0263f,   1.0567f,   1.0622f,   1.0456f,   1.0026f,   0.9384f,
            0.85445f,  0.7514f,   0.6424f,   0.5419f,   0.4479f,   0.3608f,   0.2835f,   0.2187f,
            0.1649f,   0.1212f,   0.0874f,   0.0636f,   0.04677f,  0.0329f,   0.0227f,   0.01584f,
            0.011359f, 0.008111f, 0.00579f,  0.004109f, 0.002899f, 0.002049f, 0.00144f,  0.001f,
            0.00069f,  0.000476f, 0.000332f, 0.000235f, 0.000166f, 0.000117f, 0.000083f, 0.000059f,
            0.000042f,
        }};

        constexpr Spectrum ObserverY{{
            0.000039f, 0.000064f, 0.00012f,  0.000217f, 0.000396f, 0.00064f,  0.00121f, 0.00218f,
            0.004f,    0.0073f,   0.0116f,   0.01684f,  0.023f,    0.0298f,   0.038f,   0.048f,
            0.06f,     0.0739f,   0.09098f,  0.1126f,   0.13902f,  0.1693f,   0.20802f, 0.2586f,
            0.323f,    0.4073f,   0.503f,    0.6082f,   0.71f,     0.7932f,   0.862f,   0.91485f,
            0.954f,    0.9803f,   0.99495f,  1.0f,      0.995f,    0.9786f,   0.952f,   0.9154f,
            0.87f,     0.8163f,   0.757f,    0.6949f,   0.631f,    0.5668f,   0.503f,   0.4412f,
            0.381f,    0.321f,    0.265f,    0.217f,    0.175f,    0.1382f,   0.107f,   0.0816f,
            0.061f,    0.04458f,  0.032f,    0.0232f,   0.017f,    0.01192f,  0.00821f, 0.005723f,
            0.004102f, 0.002929f, 0.002091f, 0.001484f, 0.001047f, 0.00074f,  0.00052f, 0.000361f,
            0.000249f, 0.000172f, 0.00012f,  0.000085f, 0.00006f,  0.000042f, 0.00003f, 0.000021f,
            0.000015f,
        }};

        constexpr Spectrum ObserverZ{{
            0.00645f, 0.01055f, 0.02005f, 0.03621f, 0.06785f, 0.1102f,  0.2074f,  0.3713f,
            0.6456f,  1.03905f, 1.3856f,  1.62296f, 1.74706f, 1.7826f,  1.77211f, 1.7441f,
            1.6692f,  1.5281f,  1.28764f, 1.0419f,  0.81295f, 0.6162f,  0.46518f, 0.3533f,
            0.272f,   0.2123f,  0.1582f,  0.1117f,  0.07825f, 0.05725f, 0.04216f, 0.02984f,
            0.0203f,  0.0134f,  0.00875f, 0.00575f, 0.0039f,  0.00275f, 0.0021f,  0.0018f,
            0.00165f, 0.0014f,  0.0011f,  0.001f,   0.0008f,  0.0006f,  0.00034f, 0.00024f,
            0.00019f, 0.0001f,  0.00005f, 0.00003f, 0.00002f, 0.00001f, 0.0f,     0.0f,
            0.0f,     0.0f,     0.0f,     0.0f,     0.0f,     0.0f,     0.0f,     0.0f,
            0.0f,     0.0f,     0.0f,     0.0f,     0.0f,     0.0f,     0.0f,     0.0f,
            0.0f,     0.0f,     0.0f,     0.0f,     0.0f,     0.0f,     0.0f,     0.0f,
            0.0f,
        }};

        // CIE standard illuminant D65, relative spectral power normalized to 100 at 560 nm,
        // transcribed from CIE 15:2004 Table T.1 over the same 380-780 nm grid.
        constexpr Spectrum IlluminantD65{{
            49.9755f, 52.3118f, 54.6482f, 68.7015f, 82.7549f, 87.1204f, 91.486f,  92.4589f,
            93.4318f, 90.057f,  86.6823f, 95.7736f, 104.865f, 110.936f, 117.008f, 117.41f,
            117.812f, 116.336f, 114.861f, 115.392f, 115.923f, 112.367f, 108.811f, 109.082f,
            109.354f, 108.578f, 107.802f, 106.296f, 104.79f,  106.239f, 107.689f, 106.047f,
            104.405f, 104.225f, 104.046f, 102.023f, 100.0f,   98.1671f, 96.3342f, 96.0611f,
            95.788f,  92.2368f, 88.6856f, 89.3459f, 90.0062f, 89.8026f, 89.5991f, 88.6489f,
            87.6987f, 85.4936f, 83.2886f, 83.4939f, 83.6992f, 81.863f,  80.0268f, 80.1207f,
            80.2146f, 81.2462f, 82.2778f, 80.281f,  78.2842f, 74.0027f, 69.7213f, 70.6652f,
            71.6091f, 72.979f,  74.349f,  67.9765f, 61.604f,  65.7448f, 69.8856f, 72.4863f,
            75.087f,  69.3398f, 63.5927f, 55.0054f, 46.4182f, 56.6118f, 66.8054f, 65.0941f,
            63.3828f,
        }};

        // The unscaled primary matrix: column c is the chromaticity of primary c lifted to XYZ at
        // unit y. Scaling its columns so the three sum to the white point's XYZ gives RGB -> XYZ.
        mat3 BuildRgbToXyz()
        {
            const mat3 primaries(ChromaticityToXyz(SrgbRedPrimary, SrgbRedPrimary.y),
                                 ChromaticityToXyz(SrgbGreenPrimary, SrgbGreenPrimary.y),
                                 ChromaticityToXyz(SrgbBluePrimary, SrgbBluePrimary.y));
            const vec3 white = ChromaticityToXyz(SrgbWhitePoint, 1.0f);
            const vec3 scale = glm::inverse(primaries) * white;
            return mat3(primaries[0] * scale.x, primaries[1] * scale.y, primaries[2] * scale.z);
        }

        const mat3& RgbToXyzMatrix()
        {
            static const mat3 matrix = BuildRgbToXyz();
            return matrix;
        }

        const mat3& XyzToRgbMatrix()
        {
            static const mat3 matrix = glm::inverse(BuildRgbToXyz());
            return matrix;
        }

        // Accumulating in f64 keeps the sum of 81 terms free of the rounding drift that would
        // otherwise show up against a published reference value at the fifth decimal.
        vec3 Integrate(const Spectrum& spectrum)
        {
            f64 x = 0.0;
            f64 y = 0.0;
            f64 z = 0.0;
            for (u32 i = 0; i < SpectrumSampleCount; ++i)
            {
                const f64 value = static_cast<f64>(spectrum.Samples[i]);
                x += value * static_cast<f64>(ObserverX.Samples[i]);
                y += value * static_cast<f64>(ObserverY.Samples[i]);
                z += value * static_cast<f64>(ObserverZ.Samples[i]);
            }
            return vec3(static_cast<f32>(x), static_cast<f32>(y), static_cast<f32>(z));
        }
    }

    const Spectrum& CieObserverX()
    {
        return ObserverX;
    }

    const Spectrum& CieObserverY()
    {
        return ObserverY;
    }

    const Spectrum& CieObserverZ()
    {
        return ObserverZ;
    }

    const Spectrum& CieIlluminantD65()
    {
        return IlluminantD65;
    }

    vec3 SpectrumToXyz(const Spectrum& radiance)
    {
        return Integrate(radiance) * SpectrumWavelengthStep;
    }

    vec3 ReflectanceToXyz(const Spectrum& reflectance, const Spectrum& illuminant)
    {
        f64 luminanceOfIlluminant = 0.0;
        for (u32 i = 0; i < SpectrumSampleCount; ++i)
        {
            luminanceOfIlluminant +=
                static_cast<f64>(illuminant.Samples[i]) * static_cast<f64>(ObserverY.Samples[i]);
        }
        if (luminanceOfIlluminant <= 0.0)
        {
            return vec3(0.0f);
        }
        return Integrate(reflectance * illuminant) * static_cast<f32>(1.0 / luminanceOfIlluminant);
    }

    vec2 XyzToChromaticity(vec3 xyz)
    {
        const f32 sum = xyz.x + xyz.y + xyz.z;
        if (sum == 0.0f)
        {
            return vec2(0.0f);
        }
        return vec2(xyz.x / sum, xyz.y / sum);
    }

    vec3 ChromaticityToXyz(vec2 chromaticity, f32 luminance)
    {
        if (chromaticity.y == 0.0f)
        {
            return vec3(0.0f);
        }
        const f32 scale = luminance / chromaticity.y;
        return vec3(chromaticity.x * scale, luminance,
                    (1.0f - chromaticity.x - chromaticity.y) * scale);
    }

    vec3 XyzToLinearRgb(vec3 xyz)
    {
        return XyzToRgbMatrix() * xyz;
    }

    vec3 LinearRgbToXyz(vec3 rgb)
    {
        return RgbToXyzMatrix() * rgb;
    }
}
