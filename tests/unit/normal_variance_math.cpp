// The normal-variance encoding and the roughness widening it drives (Veng/Asset/NormalVariance.h,
// the C++ form of Veng/normal_variance.slang): a widened roughness never falls below what was
// authored, is exactly it where there is nothing to widen by, and never passes a fully rough lobe.

#include <array>

#include <doctest/doctest.h>

#include <Veng/Asset/NormalVariance.h>

using namespace Veng;

TEST_CASE("Normal variance: the spread maps to a widening that is zero, monotone and bounded")
{
    CHECK(NormalVarianceOfSpread(0.0f) == 0.0f);
    CHECK(NormalVarianceOfSpread(-1.0f) == 0.0f);

    // Near zero the von Mises-Fisher widening is the spread itself.
    CHECK(NormalVarianceOfSpread(1.0e-4f) == doctest::Approx(1.0e-4f).epsilon(0.01));

    f32 previous = 0.0f;
    u32 rising = 0;
    constexpr u32 Steps = 64;
    for (u32 i = 1; i <= Steps; ++i)
    {
        const f32 variance = NormalVarianceOfSpread(static_cast<f32>(i) / Steps);
        rising += variance >= previous ? 1u : 0u;
        previous = variance;
    }
    CHECK(rising == Steps);
    CHECK(previous == 1.0f);
}

TEST_CASE("Normal variance: the stored fourth root round-trips")
{
    for (const f32 variance : {0.0f, 1.0e-5f, 0.0016f, 0.2f, 1.0f})
    {
        CAPTURE(variance);
        CHECK(DecodeNormalVariance(EncodeNormalVariance(variance)) ==
              doctest::Approx(variance).epsilon(1.0e-4));
    }
    CHECK(EncodeNormalVariance(0.0f) == 0.0f);
    CHECK(EncodeNormalVariance(2.0f) == 1.0f);
}

TEST_CASE("Normal variance: the widened roughness is at least the authored one and at most 1")
{
    constexpr std::array<f32, 5> roughnesses{0.0f, 0.04f, 0.2f, 0.6f, 1.0f};
    constexpr std::array<f32, 5> variances{1.0e-6f, 0.001f, 0.05f, 0.5f, 1.0f};

    u32 belowAuthored = 0;
    u32 aboveOne = 0;
    u32 identical = 0;
    for (const f32 roughness : roughnesses)
    {
        identical += WidenRoughnessByNormalVariance(roughness, 0.0f, 1.0f) == roughness ? 1u : 0u;
        identical += WidenRoughnessByNormalVariance(roughness, 0.5f, 0.0f) == roughness ? 1u : 0u;
        for (const f32 variance : variances)
        {
            const f32 widened = WidenRoughnessByNormalVariance(roughness, variance, 1.0f);
            belowAuthored += widened < roughness ? 1u : 0u;
            aboveOne += widened > 1.0f ? 1u : 0u;
        }
    }
    CHECK(identical == 2 * roughnesses.size());
    CHECK(belowAuthored == 0u);
    CHECK(aboveOne == 0u);

    // A glossy surface takes the spread in alpha²: 0.2 roughness is alpha² 0.0016, so a variance of
    // the same size widens it to the roughness of alpha² 0.0032.
    CHECK(WidenRoughnessByNormalVariance(0.2f, 0.0016f, 1.0f) ==
          doctest::Approx(0.2f * 1.18920712f).epsilon(1.0e-4));
    // A full variance is a fully rough lobe.
    CHECK(WidenRoughnessByNormalVariance(0.1f, 1.0f, 1.0f) == 1.0f);
}
