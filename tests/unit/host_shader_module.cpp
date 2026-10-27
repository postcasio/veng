// The host build of a Slang module (tests/shaders/host/gradient_noise.slang through
// veng_add_host_shader_module), checked on the CPU alone: properties of the function itself, in
// both of the instantiations the exports file declares. Agreement with the GPU is
// tests/gpu/host_shader_agreement.cpp.

#include <algorithm>
#include <cmath>

#include <doctest/doctest.h>

#include <GradientNoise.h>

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
