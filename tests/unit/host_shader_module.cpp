// The host build of a Slang module (tests/shaders/host/gradient_noise.slang through
// veng_add_host_shader_module), checked on the CPU alone: properties of the function itself, in
// both of the instantiations the exports file declares. Agreement with the GPU is
// tests/gpu/host_shader_agreement.cpp. The aggregate cases (tests/shaders/host/aggregates_host.slang)
// hold exports taking and returning structs, arrays and matrices to references computed here, and a
// prepared struct to the converted one it stands for.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>

#include <doctest/doctest.h>

#include <GradientNoise.h>
#include <HostAggregates.h>

using namespace Veng;

namespace
{
    // 64 points stratified over [-16, 16)^3: one per cell of a 4^3 grid, offset inside the cell by
    // irrational fractions so no coordinate lands on a lattice plane.
    vector<dvec3> StratifiedPoints()
    {
        vector<dvec3> points;
        for (int x = 0; x < 4; ++x)
        {
            for (int y = 0; y < 4; ++y)
            {
                for (int z = 0; z < 4; ++z)
                {
                    const dvec3 cell(x, y, z);
                    const dvec3 jitter(std::fmod((x + 1) * 0.6180339887, 1.0),
                                       std::fmod((y + 2) * 0.4142135623, 1.0),
                                       std::fmod((z + 3) * 0.7320508075, 1.0));
                    points.push_back((cell + jitter) * 8.0 - 16.0);
                }
            }
        }
        return points;
    }

    f64 MaxComponent(const dvec3& v)
    {
        return std::max({std::abs(v.x), std::abs(v.y), std::abs(v.z)});
    }
}

TEST_CASE("host shader module: gradient noise is zero at every lattice point")
{
    f64 worst = 0.0;
    for (int x = -3; x <= 3; ++x)
    {
        for (int y = -3; y <= 3; ++y)
        {
            for (int z = -3; z <= 3; ++z)
            {
                const dvec3 p = dvec3(x, y, z) * 37.0;
                worst = std::max(worst, std::abs(GradientNoise::NoiseF64(p).x));
                worst =
                    std::max(worst, static_cast<f64>(std::abs(GradientNoise::NoiseF32(vec3(p)).x)));
                // Every octave's lattice is a refinement of the first, so the sum vanishes too.
                worst = std::max(worst, std::abs(GradientNoise::FbmF64(p, 16).x));
                worst = std::max(worst,
                                 static_cast<f64>(std::abs(GradientNoise::FbmF32(vec3(p), 16).x)));
            }
        }
    }
    CHECK(worst == 0.0);
}

TEST_CASE("host shader module: the analytic gradient matches a central difference")
{
    f64 worstDouble = 0.0;
    f64 worstFloat = 0.0;
    for (const dvec3& p : StratifiedPoints())
    {
        const f64 h = 1e-4;
        const dvec3 analytic = dvec3(GradientNoise::FbmF64(p, 6).y, GradientNoise::FbmF64(p, 6).z,
                                     GradientNoise::FbmF64(p, 6).w);
        dvec3 numeric;
        for (int axis = 0; axis < 3; ++axis)
        {
            dvec3 step(0.0);
            step[axis] = h;
            numeric[axis] =
                (GradientNoise::FbmF64(p + step, 6).x - GradientNoise::FbmF64(p - step, 6).x) /
                (2.0 * h);
        }
        worstDouble = std::max(worstDouble, MaxComponent(analytic - numeric));

        const f32 hf = 1e-2f;
        const vec3 pf(p);
        const vec4 noise = GradientNoise::NoiseF32(pf);
        vec3 numericFloat;
        for (int axis = 0; axis < 3; ++axis)
        {
            vec3 step(0.0f);
            step[axis] = hf;
            numericFloat[axis] =
                (GradientNoise::NoiseF32(pf + step).x - GradientNoise::NoiseF32(pf - step).x) /
                (2.0f * hf);
        }
        worstFloat = std::max(worstFloat,
                              MaxComponent(dvec3(vec3(noise.y, noise.z, noise.w) - numericFloat)));
    }
    // The difference's own truncation error grows with the finest octave's frequency; a wrong
    // gradient term is off by the gradient's own magnitude, of order one.
    INFO("worst double error " << worstDouble << ", worst float error " << worstFloat);
    CHECK(worstDouble < 1e-3);
    CHECK(worstFloat < 1e-2);
}

TEST_CASE("host shader module: the float and double instantiations agree")
{
    f64 worst = 0.0;
    for (const dvec3& p : StratifiedPoints())
    {
        // The float instantiation reads the point as float, so both evaluate the same point.
        const dvec3 pf = dvec3(vec3(p));
        for (const u32 octaves : {1u, 6u, 16u})
        {
            worst = std::max(
                worst, std::abs(GradientNoise::FbmF64(pf, octaves).x -
                                static_cast<f64>(GradientNoise::FbmF32(vec3(pf), octaves).x)));
        }
    }
    CHECK(worst < 1e-5);
}

namespace
{
    // Four values of distinct small integers, a basis with every entry distinct, and a range that
    // clips the weighted sum's extremes.
    HostAggregates::Block TestBlock()
    {
        HostAggregates::Block block;
        block.Values = {vec4(1.0f, -2.0f, 3.0f, 4.0f), vec4(5.0f, 6.0f, -7.0f, 8.0f),
                        vec4(9.0f, 10.0f, 11.0f, -12.0f), vec4(-13.0f, 14.0f, 15.0f, 16.0f)};
        block.Range = {.Low = -20.0f, .High = 20.0f};
        // glm names columns first: two columns of three rows, Slang's float3x2.
        block.Basis = glm::mat2x3(vec3(1.0f, 2.0f, 3.0f), vec3(4.0f, 5.0f, 6.0f));
        block.Count = 3;
        return block;
    }
}

TEST_CASE("host shader module: a struct and an array cross by value, nested and in order")
{
    const HostAggregates::Block block = TestBlock();
    const std::array<f32, 4> weights{2.0f, -1.0f, 3.0f, 100.0f};
    vec4 expected(0.0f);
    for (u32 i = 0; i < block.Count; ++i)
    {
        expected += block.Values[i] * weights[i];
    }
    expected = glm::clamp(expected, vec4(block.Range.Low), vec4(block.Range.High));
    CHECK(HostAggregates::SumBlock(block, weights) == expected);
}

TEST_CASE("host shader module: a matrix crosses as glm's, columns first")
{
    const HostAggregates::Block block = TestBlock();
    const vec2 v(3.0f, -2.0f);
    // Slang's mul(m, v) over the column-major convention is glm's m * v.
    CHECK(HostAggregates::ApplyBasis(block, v) == block.Basis * v);
    CHECK(HostAggregates::ScaledTranspose(block.Basis, 2.0f) == glm::transpose(block.Basis) * 2.0f);
}

TEST_CASE("host shader module: out arrays and inout structs are written back, structs returned")
{
    const HostAggregates::Block block = TestBlock();
    std::array<f32, 4> sums{};
    HostAggregates::Span range{.Low = 0.0f, .High = 0.0f};
    HostAggregates::Summarise(block, sums, range);
    f32 low = 0.0f;
    f32 high = 0.0f;
    for (u32 i = 0; i < 4; ++i)
    {
        const vec4 value = block.Values[i];
        CHECK(sums[i] == value.x + value.y + value.z + value.w);
        low = std::min(low, sums[i]);
        high = std::max(high, sums[i]);
    }
    CHECK(range.Low == low);
    CHECK(range.High == high);

    const std::array<HostAggregates::Span, 3> spans{
        {{.Low = 1.0f, .High = 4.0f}, {.Low = -2.0f, .High = 0.0f}, {.Low = 5.0f, .High = 5.5f}}};
    CHECK(HostAggregates::TotalWidth(spans) == 5.5f);

    const HostAggregates::Span made = HostAggregates::MakeSpan(4.0f, -1.0f);
    CHECK(made.Low == -1.0f);
    CHECK(made.High == 4.0f);
}

TEST_CASE("host shader module: a prepared struct passes bit-identically to a converted one")
{
    // Fractional values, so a conversion that dropped or reordered a bit would show.
    HostAggregates::Block block = TestBlock();
    for (u32 i = 0; i < 4; ++i)
    {
        block.Values[i] = block.Values[i] * 0.3141592f + vec4(0.1f, 0.2f, 0.3f, 0.7f);
    }
    block.Basis = block.Basis * 0.7071068f;
    block.Range = {.Low = -3.3f, .High = 3.7f};
    const auto prepared = HostAggregates::Prepared<HostAggregates::Block>::Prepare(block);
    const auto bitEqual = [](const auto& a, const auto& b)
    { return std::memcmp(&a, &b, sizeof(a)) == 0; };

    const std::array<f32, 4> weights{0.25f, -1.5f, 3.125f, 0.4f};
    CHECK(bitEqual(HostAggregates::SumBlock(prepared, weights),
                   HostAggregates::SumBlock(block, weights)));
    const vec2 v(1.3f, -0.6f);
    CHECK(bitEqual(HostAggregates::ApplyBasis(prepared, v), HostAggregates::ApplyBasis(block, v)));

    std::array<f32, 4> sums{};
    std::array<f32, 4> preparedSums{};
    HostAggregates::Span range{.Low = 0.0f, .High = 0.0f};
    HostAggregates::Span preparedRange = range;
    HostAggregates::Summarise(block, sums, range);
    HostAggregates::Summarise(prepared, preparedSums, preparedRange);
    CHECK(bitEqual(sums, preparedSums));
    CHECK(bitEqual(range, preparedRange));
}

TEST_CASE("host shader module: a call writing its copy of a prepared struct leaves it unchanged")
{
    const HostAggregates::Block block = TestBlock();
    const auto prepared = HostAggregates::Prepared<HostAggregates::Block>::Prepare(block);
    const vec4 first = HostAggregates::DrainBlock(prepared, 2.0f);
    CHECK(first == block.Values[0] * 2.0f);
    // The export zeroes its copy's Count and scales its first value; neither reaches the handle.
    CHECK(HostAggregates::DrainBlock(prepared, 2.0f) == first);
    CHECK(HostAggregates::SumBlock(prepared, {1.0f, 1.0f, 1.0f, 1.0f}) ==
          HostAggregates::SumBlock(block, {1.0f, 1.0f, 1.0f, 1.0f}));

    // A copy holds its own value, which outlives the handle it was copied from.
    auto copy = std::make_unique<HostAggregates::Prepared<HostAggregates::Block>>(prepared);
    const HostAggregates::Prepared<HostAggregates::Block> held = *copy;
    copy.reset();
    CHECK(HostAggregates::DrainBlock(held, 2.0f) == first);
}
