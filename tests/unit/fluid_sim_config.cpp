// Fluid-solver configuration cases. ValidateFluidSimShape is the device-free half of what
// FluidSim::Create asserts on, so the whole rejection set — a missing velocity field, a
// zero-size grid, a mismatched extent, an unsupported format, a row metric that is not one entry
// per row — is pinned here with no images, no Context and no driver. The pure CPU reference for
// one advection tap (the fold, the back-trace, the bilinear combine the kernels perform) rides
// beside it, since the gpu band cross-checks a texel against exactly these functions.

#include <doctest/doctest.h>

#include <Veng/Renderer/FluidSim.h>

#include "Renderer/FluidSimShape.h"

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    constexpr uvec2 Grid{16, 8};

    FluidFieldShape Field(const uvec2 extent, const Format format)
    {
        return {.Present = true, .Extent = extent, .Format = format};
    }

    // A configuration that validates, so each case below changes exactly one thing.
    FluidSimShape Valid()
    {
        FluidSimShape shape;
        shape.Velocity = Field(Grid, Format::RG16Sfloat);
        shape.Dyes.push_back(Field(Grid, Format::RGBA16Sfloat));
        return shape;
    }
}

TEST_CASE("fluid config: the default-shaped configuration validates")
{
    const VoidResult result = ValidateFluidSimShape(Valid());
    CHECK(result.has_value());
}

TEST_CASE("fluid config: a missing or unusable velocity field is rejected")
{
    FluidSimShape missing = Valid();
    missing.Velocity = {};
    const VoidResult noVelocity = ValidateFluidSimShape(missing);
    REQUIRE_FALSE(noVelocity.has_value());
    CHECK(noVelocity.error().find("no velocity field") != string::npos);

    FluidSimShape empty = Valid();
    empty.Velocity.Extent = {16, 0};
    const VoidResult zeroSized = ValidateFluidSimShape(empty);
    REQUIRE_FALSE(zeroSized.has_value());
    CHECK(zeroSized.error().find("zero-sized") != string::npos);

    // A single-channel velocity field has no force or gradient variant that writes it.
    FluidSimShape wrongFormat = Valid();
    wrongFormat.Velocity.Format = Format::R16Sfloat;
    const VoidResult rejected = ValidateFluidSimShape(wrongFormat);
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().find("velocity field") != string::npos);

    // Both widths the plan offers the caller are accepted.
    FluidSimShape wide = Valid();
    wide.Velocity.Format = Format::RG32Sfloat;
    CHECK(ValidateFluidSimShape(wide).has_value());
}

TEST_CASE("fluid config: a dye must exist, match the grid, and be a float format the store writes")
{
    FluidSimShape missing = Valid();
    missing.Dyes[0] = {};
    const VoidResult noImage = ValidateFluidSimShape(missing);
    REQUIRE_FALSE(noImage.has_value());
    CHECK(noImage.error().find("dye 0 has no image") != string::npos);

    FluidSimShape mismatched = Valid();
    mismatched.Dyes[0].Extent = {Grid.x, Grid.y + 1};
    const VoidResult wrongExtent = ValidateFluidSimShape(mismatched);
    REQUIRE_FALSE(wrongExtent.has_value());
    CHECK(wrongExtent.error().find("not the grid's") != string::npos);

    FluidSimShape unorm = Valid();
    unorm.Dyes[0].Format = Format::RGBA8Unorm;
    CHECK_FALSE(ValidateFluidSimShape(unorm).has_value());

    // The three widths the store family covers all pass.
    for (const Format format : {Format::R16Sfloat, Format::RG16Sfloat, Format::RGBA16Sfloat})
    {
        FluidSimShape shape = Valid();
        shape.Dyes[0].Format = format;
        CHECK(ValidateFluidSimShape(shape).has_value());
    }

    FluidSimShape crowded = Valid();
    crowded.Dyes.assign(MaxFluidDyes + 1, Field(Grid, Format::R16Sfloat));
    const VoidResult tooMany = ValidateFluidSimShape(crowded);
    REQUIRE_FALSE(tooMany.has_value());
    CHECK(tooMany.error().find("exceeds the maximum") != string::npos);
}

TEST_CASE("fluid config: the optional fields are checked only when supplied")
{
    // Absent, they constrain nothing.
    const FluidSimShape bare = Valid();
    CHECK(ValidateFluidSimShape(bare).has_value());

    FluidSimShape target = Valid();
    target.RelaxationTarget = Field({Grid.x + 4, Grid.y}, Format::RG16Sfloat);
    const VoidResult wrongTarget = ValidateFluidSimShape(target);
    REQUIRE_FALSE(wrongTarget.has_value());
    CHECK(wrongTarget.error().find("relaxation target") != string::npos);

    FluidSimShape damping = Valid();
    damping.DampingMask = Field(Grid, Format::RGBA16Sfloat);
    const VoidResult wrongMask = ValidateFluidSimShape(damping);
    REQUIRE_FALSE(wrongMask.has_value());
    CHECK(wrongMask.error().find("damping mask") != string::npos);

    FluidSimShape good = Valid();
    good.RelaxationTarget = Field(Grid, Format::RG32Sfloat);
    good.DampingMask = Field(Grid, Format::R16Sfloat);
    CHECK(ValidateFluidSimShape(good).has_value());
}

TEST_CASE("fluid config: the row metric is empty or one entry per row")
{
    FluidSimShape shape = Valid();
    CHECK(ValidateFluidSimShape(shape).has_value());

    shape.RowMetricCount = Grid.y;
    CHECK(ValidateFluidSimShape(shape).has_value());

    shape.RowMetricCount = Grid.y - 1;
    const VoidResult shortMetric = ValidateFluidSimShape(shape);
    REQUIRE_FALSE(shortMetric.has_value());
    CHECK(shortMetric.error().find("one per row") != string::npos);
}

TEST_CASE("fluid config: the projection and the timestep must be usable")
{
    FluidSimShape noIterations = Valid();
    noIterations.JacobiIterations = 0;
    CHECK_FALSE(ValidateFluidSimShape(noIterations).has_value());

    for (const f32 timeStep : {0.0f, -1.0f, std::numeric_limits<f32>::infinity()})
    {
        FluidSimShape shape = Valid();
        shape.TimeStep = timeStep;
        CHECK_FALSE(ValidateFluidSimShape(shape).has_value());
    }
}

TEST_CASE("fluid reference: the fold is periodic one way and clamped the other")
{
    CHECK(FoldFluidTexel(0, 8, FluidWrap::Periodic) == 0);
    CHECK(FoldFluidTexel(8, 8, FluidWrap::Periodic) == 0);
    CHECK(FoldFluidTexel(-1, 8, FluidWrap::Periodic) == 7);
    CHECK(FoldFluidTexel(-9, 8, FluidWrap::Periodic) == 7);
    CHECK(FoldFluidTexel(19, 8, FluidWrap::Periodic) == 3);

    CHECK(FoldFluidTexel(-4, 8, FluidWrap::Clamped) == 0);
    CHECK(FoldFluidTexel(3, 8, FluidWrap::Clamped) == 3);
    CHECK(FoldFluidTexel(12, 8, FluidWrap::Clamped) == 7);
}

TEST_CASE("fluid reference: the back-trace steps back by one timestep, scaled by the row metric")
{
    // A texel's centre is at i + 0.5, and the trace goes *back* along the velocity.
    const vec2 plain = FluidBackTrace({4, 2}, {2.0f, -1.0f}, 0.5f, 1.0f);
    CHECK(plain.x == doctest::Approx(4.5f - 1.0f));
    CHECK(plain.y == doctest::Approx(2.5f + 0.5f));

    // The metric scales the x axis alone: a row of metric 0.5 travels half as far in x and
    // exactly as far in y.
    const vec2 stretched = FluidBackTrace({4, 2}, {2.0f, -1.0f}, 0.5f, 0.5f);
    CHECK(stretched.x == doctest::Approx(4.5f - 0.5f));
    CHECK(stretched.y == doctest::Approx(plain.y));
}

TEST_CASE("fluid reference: one bilinear tap combines four folded loads")
{
    constexpr uvec2 Extent{4, 4};

    // A field whose value is its x coordinate, so a tap reads back the position it sampled.
    auto ramp = [](const ivec2 texel) { return vec4(static_cast<f32>(texel.x), 0, 0, 0); };

    // Dead on a texel centre, the tap is that texel.
    const vec4 centre =
        SampleFluidBilinear({2.5f, 1.5f}, Extent, FluidWrap::Clamped, FluidWrap::Clamped, ramp);
    CHECK(centre.x == doctest::Approx(2.0f));

    // Half a texel along, it is the midpoint of the two.
    const vec4 between =
        SampleFluidBilinear({3.0f, 1.5f}, Extent, FluidWrap::Clamped, FluidWrap::Clamped, ramp);
    CHECK(between.x == doctest::Approx(2.5f));

    // Past the right wall a clamped axis holds the last texel; a periodic one folds to 0, so the
    // tap sits between the two ends.
    const vec4 clamped =
        SampleFluidBilinear({4.5f, 1.5f}, Extent, FluidWrap::Clamped, FluidWrap::Clamped, ramp);
    CHECK(clamped.x == doctest::Approx(3.0f));
    const vec4 periodic =
        SampleFluidBilinear({4.0f, 1.5f}, Extent, FluidWrap::Periodic, FluidWrap::Clamped, ramp);
    CHECK(periodic.x == doctest::Approx(1.5f));
}
