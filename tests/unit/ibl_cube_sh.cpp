// The cube→irradiance-SH projection (EnvironmentIbl::ProjectCubeToIrradianceSh): the cheap
// ambient arm's cube source. A device-free CPU projection of a radiance cube into the order-2,
// 9-coefficient irradiance SH the skylight arm evaluates, so it is tested here with synthetic
// analytic cubes rather than a live bake:
//
//   1. A constant cube (every texel L) projects to a constant irradiance pi*L in every normal —
//      the analytic Lambertian response of a uniform sky.
//   2. A directional bright cube (a bright band around +Y, dark elsewhere) projects to an
//      irradiance that peaks toward +Y and falls off toward -Y — the ambient follows the source.
//   3. The projection matches an independent analytic SH: the same constant/directional radiance
//      projected through the reference SphericalHarmonics.h path (ProjectSample + ConvolveCosine)
//      over a dense direction set reproduces the cube projection's evaluated irradiance.

#include <array>
#include <cmath>
#include <vector>

#include <doctest/doctest.h>

#include <glm/gtc/packing.hpp>

#include <Veng/Math/SphericalHarmonics.h>

#include "Renderer/EnvironmentIbl.h"

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    constexpr u32 CubeFaces = 6;

    // The world direction a face texel reconstructs — the same basis EnvironmentIbl projects
    // through and ibl_equirect_to_cube writes, so the synthetic cube's radiance is authored as a
    // function of this direction.
    vec3 FaceDirection(u32 face, f32 u, f32 v)
    {
        const f32 sx = u * 2.0f - 1.0f;
        const f32 sy = v * 2.0f - 1.0f;
        vec3 dir(0.0f);
        switch (face)
        {
        case 0:
            dir = vec3(1.0f, -sy, -sx);
            break; // +X
        case 1:
            dir = vec3(-1.0f, -sy, sx);
            break; // -X
        case 2:
            dir = vec3(sx, 1.0f, sy);
            break; // +Y
        case 3:
            dir = vec3(sx, -1.0f, -sy);
            break; // -Y
        case 4:
            dir = vec3(sx, -sy, 1.0f);
            break; // +Z
        default:
            dir = vec3(-sx, -sy, -1.0f);
            break; // -Z
        }
        return glm::normalize(dir);
    }

    // Builds a synthetic RGBA16F cube (six faces, layer-major) whose per-texel radiance is a
    // caller-supplied function of the reconstructed world direction.
    template <typename Fn>
    std::vector<u8> MakeCube(u32 faceSize, Fn radiance)
    {
        std::vector<u8> bytes(static_cast<usize>(faceSize) * faceSize * CubeFaces * 4 *
                              sizeof(u16));
        auto* halves = reinterpret_cast<u16*>(bytes.data());
        const f32 invFace = 1.0f / static_cast<f32>(faceSize);
        for (u32 face = 0; face < CubeFaces; ++face)
        {
            for (u32 y = 0; y < faceSize; ++y)
            {
                for (u32 x = 0; x < faceSize; ++x)
                {
                    const f32 u = (static_cast<f32>(x) + 0.5f) * invFace;
                    const f32 v = (static_cast<f32>(y) + 0.5f) * invFace;
                    const vec3 color = radiance(FaceDirection(face, u, v));
                    const usize base =
                        ((static_cast<usize>(face) * faceSize + y) * faceSize + x) * 4;
                    halves[base + 0] = glm::packHalf1x16(color.r);
                    halves[base + 1] = glm::packHalf1x16(color.g);
                    halves[base + 2] = glm::packHalf1x16(color.b);
                    halves[base + 3] = glm::packHalf1x16(1.0f);
                }
            }
        }
        return bytes;
    }
}

TEST_CASE("ProjectCubeToIrradianceSh: a constant cube yields constant pi*L irradiance")
{
    constexpr u32 FaceSize = 32;
    constexpr vec3 L(0.4f, 0.7f, 1.0f);
    const std::vector<u8> cube = MakeCube(FaceSize, [&](vec3) { return L; });

    const Sh9 sh = EnvironmentIbl::ProjectCubeToIrradianceSh(cube, FaceSize);

    // A uniform radiance L convolved with the cosine lobe integrates to pi*L, independent of the
    // evaluated normal, and the higher SH bands ring to zero. A generous epsilon covers the RGBA16F
    // round-trip and the finite cube resolution.
    constexpr f32 Pi = 3.14159265358979323846f;
    const std::array<vec3, 4> normals = {vec3(1, 0, 0), vec3(0, 1, 0), vec3(0, 0, 1),
                                         glm::normalize(vec3(1, 1, 1))};
    for (const vec3 n : normals)
    {
        const vec3 irradiance = EvalIrradiance(sh, n);
        CHECK(irradiance.r == doctest::Approx(Pi * L.r).epsilon(0.02));
        CHECK(irradiance.g == doctest::Approx(Pi * L.g).epsilon(0.02));
        CHECK(irradiance.b == doctest::Approx(Pi * L.b).epsilon(0.02));
    }
}

TEST_CASE("ProjectCubeToIrradianceSh: a directional bright cube yields directional irradiance")
{
    constexpr u32 FaceSize = 48;
    // Bright toward +Y, dark toward -Y: radiance = max(0, dir.y). The convolved irradiance must
    // peak for a +Y normal and be least for a -Y normal.
    const std::vector<u8> cube =
        MakeCube(FaceSize, [](vec3 dir) { return vec3(std::max(0.0f, dir.y)); });

    const Sh9 sh = EnvironmentIbl::ProjectCubeToIrradianceSh(cube, FaceSize);

    const f32 up = EvalIrradiance(sh, vec3(0, 1, 0)).r;
    const f32 down = EvalIrradiance(sh, vec3(0, -1, 0)).r;
    const f32 side = EvalIrradiance(sh, vec3(1, 0, 0)).r;

    // The ambient follows the source: a normal facing the bright pole receives the most irradiance,
    // a normal facing the dark pole the least, and a side normal falls in between.
    CHECK(up > side);
    CHECK(side > down);
    CHECK(up > 0.0f);
}

TEST_CASE("ProjectCubeToIrradianceSh: matches a reference analytic SH projection")
{
    constexpr u32 FaceSize = 48;
    // A smooth directional radiance with a non-trivial l=1 term.
    auto radiance = [](vec3 dir) { return vec3(0.5f + 0.5f * dir.z); };
    const std::vector<u8> cube = MakeCube(FaceSize, radiance);

    const Sh9 cubeSh = EnvironmentIbl::ProjectCubeToIrradianceSh(cube, FaceSize);

    // The reference: project the same radiance over a dense uniform Fibonacci sphere through the
    // SphericalHarmonics.h path, then convolve — the independent analytic ground truth the cube
    // projection must reproduce (both share ShBasis + ConvolveCosine, so agreement pins the cube
    // solid-angle weighting and direction basis).
    constexpr u32 SampleCount = 4096;
    Sh9 reference = Sh9::Zero();
    const f32 weight = 4.0f * glm::pi<f32>() / static_cast<f32>(SampleCount);
    const f32 goldenAngle = glm::pi<f32>() * (3.0f - std::sqrt(5.0f));
    for (u32 i = 0; i < SampleCount; ++i)
    {
        const f32 z = 1.0f - (2.0f * static_cast<f32>(i) + 1.0f) / static_cast<f32>(SampleCount);
        const f32 r = std::sqrt(std::max(0.0f, 1.0f - z * z));
        const f32 phi = goldenAngle * static_cast<f32>(i);
        const vec3 dir(r * std::cos(phi), z, r * std::sin(phi));
        ProjectSample(reference, dir, radiance(dir), weight);
    }
    const Sh9 referenceSh = ConvolveCosine(reference);

    // Compare the reconstructed irradiance at a spread of normals — the observable both feed. The
    // cube projection samples texel centers, the reference samples the sphere directly, so a
    // moderate epsilon covers the two quadrature schemes and the RGBA16F round-trip.
    const std::array<vec3, 6> normals = {vec3(1, 0, 0),  vec3(0, 1, 0),
                                         vec3(0, 0, 1),  vec3(-1, 0, 0),
                                         vec3(0, 0, -1), glm::normalize(vec3(1, 1, 1))};
    for (const vec3 n : normals)
    {
        const vec3 fromCube = EvalIrradiance(cubeSh, n);
        const vec3 fromRef = EvalIrradiance(referenceSh, n);
        CHECK(fromCube.r == doctest::Approx(fromRef.r).epsilon(0.03));
        CHECK(fromCube.g == doctest::Approx(fromRef.g).epsilon(0.03));
        CHECK(fromCube.b == doctest::Approx(fromRef.b).epsilon(0.03));
    }
}
