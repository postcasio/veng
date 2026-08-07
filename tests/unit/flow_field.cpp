// FlowField configuration and device-free math. ValidateFlowFieldConfig is the device-free half of
// what FlowField::Create asserts on, so the whole rejection set — a missing velocity field, a
// zero-size grid, no dye, a mismatched extent, an unsupported format, a row metric that is not one
// entry per row — is pinned here with no images, no Context and no driver. The pure CPU reference
// for the wrap fold, the step-scaled back-trace, the bilinear tap and the clamped sharpen rides
// beside it, since the gpu band cross-checks the shaders against exactly these functions.

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include <doctest/doctest.h>

#include <Veng/Renderer/FlowField.h>

#include "Renderer/FlowFieldConfig.h"

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    constexpr uvec2 Grid{16, 8};

    FlowFieldImageShape Field(const uvec2 extent, const Format format)
    {
        return {.Present = true, .Extent = extent, .Format = format};
    }

    // A configuration that validates, so each case below changes exactly one thing.
    FlowFieldConfig Valid()
    {
        FlowFieldConfig config;
        config.Velocity = Field(Grid, Format::RG16Sfloat);
        config.Dyes.push_back(Field(Grid, Format::RGBA16Sfloat));
        return config;
    }
}

TEST_CASE("flow config: the default-shaped configuration validates")
{
    CHECK(ValidateFlowFieldConfig(Valid()).has_value());
}

TEST_CASE("flow config: a missing or unusable velocity field is rejected")
{
    FlowFieldConfig missing = Valid();
    missing.Velocity = {};
    const VoidResult noVelocity = ValidateFlowFieldConfig(missing);
    REQUIRE_FALSE(noVelocity.has_value());
    CHECK(noVelocity.error().find("no velocity field") != string::npos);

    FlowFieldConfig empty = Valid();
    empty.Velocity.Extent = {16, 0};
    const VoidResult zeroSized = ValidateFlowFieldConfig(empty);
    REQUIRE_FALSE(zeroSized.has_value());
    CHECK(zeroSized.error().find("zero-sized") != string::npos);

    // A single-channel velocity field is not one the advect kernel reads as a flow.
    FlowFieldConfig wrongFormat = Valid();
    wrongFormat.Velocity.Format = Format::R16Sfloat;
    const VoidResult rejected = ValidateFlowFieldConfig(wrongFormat);
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().find("velocity field") != string::npos);

    // Both widths the primitive offers the caller are accepted.
    FlowFieldConfig wide = Valid();
    wide.Velocity.Format = Format::RG32Sfloat;
    CHECK(ValidateFlowFieldConfig(wide).has_value());
}

TEST_CASE("flow config: a dye must exist, match the grid, and be a float format the store writes")
{
    FlowFieldConfig none = Valid();
    none.Dyes.clear();
    const VoidResult noDye = ValidateFlowFieldConfig(none);
    REQUIRE_FALSE(noDye.has_value());
    CHECK(noDye.error().find("no dye field") != string::npos);

    FlowFieldConfig missing = Valid();
    missing.Dyes[0] = {};
    const VoidResult noImage = ValidateFlowFieldConfig(missing);
    REQUIRE_FALSE(noImage.has_value());
    CHECK(noImage.error().find("dye 0 has no image") != string::npos);

    FlowFieldConfig mismatched = Valid();
    mismatched.Dyes[0].Extent = {Grid.x, Grid.y + 1};
    const VoidResult wrongExtent = ValidateFlowFieldConfig(mismatched);
    REQUIRE_FALSE(wrongExtent.has_value());
    CHECK(wrongExtent.error().find("not the grid's") != string::npos);

    FlowFieldConfig unorm = Valid();
    unorm.Dyes[0].Format = Format::RGBA8Unorm;
    CHECK_FALSE(ValidateFlowFieldConfig(unorm).has_value());

    // The three widths the store family covers all pass.
    for (const Format format : {Format::R16Sfloat, Format::RG16Sfloat, Format::RGBA16Sfloat})
    {
        FlowFieldConfig config = Valid();
        config.Dyes[0].Format = format;
        CHECK(ValidateFlowFieldConfig(config).has_value());
    }

    FlowFieldConfig crowded = Valid();
    crowded.Dyes.assign(MaxFlowDyes + 1, Field(Grid, Format::R16Sfloat));
    const VoidResult tooMany = ValidateFlowFieldConfig(crowded);
    REQUIRE_FALSE(tooMany.has_value());
    CHECK(tooMany.error().find("exceeds the maximum") != string::npos);
}

TEST_CASE("flow config: the row metric is empty or one entry per row, and the step scale is usable")
{
    FlowFieldConfig config = Valid();
    CHECK(ValidateFlowFieldConfig(config).has_value());

    config.RowMetricCount = Grid.y;
    CHECK(ValidateFlowFieldConfig(config).has_value());

    config.RowMetricCount = Grid.y - 1;
    const VoidResult shortMetric = ValidateFlowFieldConfig(config);
    REQUIRE_FALSE(shortMetric.has_value());
    CHECK(shortMetric.error().find("one per row") != string::npos);

    for (const f32 stepScale : {0.0f, -1.0f, std::numeric_limits<f32>::infinity()})
    {
        FlowFieldConfig bad = Valid();
        bad.StepScale = stepScale;
        CHECK_FALSE(ValidateFlowFieldConfig(bad).has_value());
    }
}

TEST_CASE("flow reference: the fold is periodic one way and clamped the other")
{
    CHECK(FoldFlowTexel(0, 8, FlowWrap::Periodic) == 0);
    CHECK(FoldFlowTexel(8, 8, FlowWrap::Periodic) == 0);
    CHECK(FoldFlowTexel(-1, 8, FlowWrap::Periodic) == 7);
    CHECK(FoldFlowTexel(-9, 8, FlowWrap::Periodic) == 7);
    CHECK(FoldFlowTexel(19, 8, FlowWrap::Periodic) == 3);

    CHECK(FoldFlowTexel(-4, 8, FlowWrap::Clamped) == 0);
    CHECK(FoldFlowTexel(3, 8, FlowWrap::Clamped) == 3);
    CHECK(FoldFlowTexel(12, 8, FlowWrap::Clamped) == 7);
}

TEST_CASE("flow reference: the back-trace steps back by the step scale, scaled by the row metric")
{
    // A texel's centre is at i + 0.5, and the trace goes *back* along the velocity, the whole
    // displacement scaled by the step scale.
    const vec2 plain = FlowBackTrace({4, 2}, {2.0f, -1.0f}, 0.5f, 1.0f);
    CHECK(plain.x == doctest::Approx(4.5f - 1.0f));
    CHECK(plain.y == doctest::Approx(2.5f + 0.5f));

    // A larger step scale advances proportionally further: the step scale is the advance-per-step
    // knob and nothing re-times when it moves.
    const vec2 doubled = FlowBackTrace({4, 2}, {2.0f, -1.0f}, 1.0f, 1.0f);
    CHECK(doubled.x == doctest::Approx(4.5f - 2.0f));
    CHECK(doubled.y == doctest::Approx(2.5f + 1.0f));

    // The metric scales the x axis alone: a row of metric 0.5 travels half as far in x and exactly
    // as far in y.
    const vec2 stretched = FlowBackTrace({4, 2}, {2.0f, -1.0f}, 0.5f, 0.5f);
    CHECK(stretched.x == doctest::Approx(4.5f - 0.5f));
    CHECK(stretched.y == doctest::Approx(plain.y));
}

TEST_CASE("flow reference: one bilinear tap combines four folded loads")
{
    constexpr uvec2 Extent{4, 4};

    // A field whose value is its x coordinate, so a tap reads back the position it sampled.
    auto ramp = [](const ivec2 texel) { return vec4(static_cast<f32>(texel.x), 0, 0, 0); };

    const vec4 centre =
        SampleFlowBilinear({2.5f, 1.5f}, Extent, FlowWrap::Clamped, FlowWrap::Clamped, ramp);
    CHECK(centre.x == doctest::Approx(2.0f));

    const vec4 between =
        SampleFlowBilinear({3.0f, 1.5f}, Extent, FlowWrap::Clamped, FlowWrap::Clamped, ramp);
    CHECK(between.x == doctest::Approx(2.5f));

    // Past the right wall a clamped axis holds the last texel; a periodic one folds to 0.
    const vec4 clamped =
        SampleFlowBilinear({4.5f, 1.5f}, Extent, FlowWrap::Clamped, FlowWrap::Clamped, ramp);
    CHECK(clamped.x == doctest::Approx(3.0f));
    const vec4 periodic =
        SampleFlowBilinear({4.0f, 1.5f}, Extent, FlowWrap::Periodic, FlowWrap::Clamped, ramp);
    CHECK(periodic.x == doctest::Approx(1.5f));
}

TEST_CASE("flow reference: the sharpen clamps to the neighbourhood range and is a no-op at zero")
{
    const vec4 lo{0.0f};
    const vec4 hi{1.0f};

    // Fed an extreme centre, the clamped unsharp returns the neighbourhood maximum rather than the
    // amplified value — the property the feedback loop depends on to stay bounded.
    const vec4 blownUp = FlowSharpen(vec4{1000.0f}, vec4{0.0f}, lo, hi, 4.0f);
    CHECK(blownUp.x == doctest::Approx(1.0f));

    // A centre well below the neighbourhood floor clamps up to it.
    const vec4 pushedDown = FlowSharpen(vec4{-1000.0f}, vec4{0.5f}, lo, hi, 4.0f);
    CHECK(pushedDown.x == doctest::Approx(0.0f));

    // A moderate delta stays inside the range and enhances contrast: a centre above its blur is
    // pushed further above.
    const vec4 enhanced = FlowSharpen(vec4{0.6f}, vec4{0.5f}, lo, hi, 2.0f);
    CHECK(enhanced.x == doctest::Approx(0.6f + 2.0f * 0.1f));
    CHECK(enhanced.x > 0.6f);

    // Strength 0 returns the centre unchanged (it is within its own neighbourhood by construction).
    const vec4 untouched = FlowSharpen(vec4{0.42f}, vec4{0.1f}, lo, hi, 0.0f);
    CHECK(untouched.x == doctest::Approx(0.42f));
}

TEST_CASE("flow reference: a static field keeps a dye's mass bounded across many advects")
{
    // Pure semi-Lagrangian advection along a static field replaces each texel with a bilinear
    // combination of source texels — a convex blend whose weights sum to one — so no texel can ever
    // exceed the running maximum, and with no source or sink the total mass stays bounded. This
    // simulates the CPU advect the shader mirrors and asserts that aggregate once, over a sweep.
    constexpr uvec2 Extent{24, 16};
    constexpr FlowWrap WrapX = FlowWrap::Periodic;
    constexpr FlowWrap WrapY = FlowWrap::Clamped;
    constexpr f32 StepScale = 0.7f;
    constexpr u32 Steps = 64;
    constexpr usize Count = static_cast<usize>(Extent.x) * Extent.y;

    // A swirling static velocity, so the flow both converges and diverges across the grid.
    auto velocityAt = [](const u32 x, const u32 y)
    {
        const f32 fx = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(Extent.x);
        const f32 fy = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(Extent.y);
        return vec2(0.8f * std::sin(6.2831853f * fy), 0.6f * std::cos(6.2831853f * fx));
    };

    std::vector<f32> dye(Count);
    f32 seedMax = 0.0f;
    f32 seedMin = std::numeric_limits<f32>::max();
    f32 seedMass = 0.0f;
    for (u32 y = 0; y < Extent.y; ++y)
    {
        for (u32 x = 0; x < Extent.x; ++x)
        {
            const f32 value = static_cast<f32>((x * 7 + y * 5) % 13) / 12.0f;
            dye[static_cast<usize>(y) * Extent.x + x] = value;
            seedMax = std::max(seedMax, value);
            seedMin = std::min(seedMin, value);
            seedMass += value;
        }
    }

    f32 runningMax = seedMax;
    f32 runningMin = seedMin;
    std::vector<f32> next(Count);
    for (u32 step = 0; step < Steps; ++step)
    {
        for (u32 y = 0; y < Extent.y; ++y)
        {
            for (u32 x = 0; x < Extent.x; ++x)
            {
                const vec2 v = velocityAt(x, y);
                const vec2 source = FlowBackTrace({x, y}, v, StepScale, 1.0f);
                const vec4 sampled = SampleFlowBilinear(
                    source, Extent, WrapX, WrapY,
                    [&](const ivec2 texel)
                    {
                        return vec4(dye[static_cast<usize>(texel.y) * Extent.x + texel.x], 0, 0, 0);
                    });
                next[static_cast<usize>(y) * Extent.x + x] = sampled.x;
            }
        }
        dye.swap(next);
        for (const f32 value : dye)
        {
            runningMax = std::max(runningMax, value);
            runningMin = std::min(runningMin, value);
        }
    }

    // No texel ever climbs above the seed's maximum nor sinks below its minimum (a convex blend
    // cannot leave the range), so total mass is bounded by max * texel count throughout.
    const f32 epsilon = 1e-5f;
    CHECK(runningMax <= seedMax + epsilon);
    CHECK(runningMin >= seedMin - epsilon);

    f32 finalMass = 0.0f;
    for (const f32 value : dye)
    {
        finalMass += value;
    }
    CHECK(finalMass <= seedMax * static_cast<f32>(Count) + epsilon);
    CHECK(finalMass > 0.0f);
    // Mass is not conserved (a convergent flow piles dye up), but it stays a bounded multiple of
    // where it started rather than growing without limit.
    CHECK(finalMass < seedMass * 4.0f);
}
