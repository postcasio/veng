// Order-2 spherical-harmonics math: pure CPU, no Context, no Vulkan. Pins the
// real-SH basis convention numerically — the constant-ambient invariant, the
// analytic cosine-lobe band factors, a directional lobe peak, projection
// linearity, and a fixed coefficient set with hand-computed EvalIrradiance
// goldens — so a later GPU evaluator matches the CPU bit for bit by reproducing
// the same checked-in numbers, not a prose description of the basis.

#include <doctest/doctest.h>

#include <array>
#include <cmath>

#include <Veng/Math/SphericalHarmonics.h>

using namespace Veng;

namespace
{
    constexpr f32 Pi = 3.14159265358979323846f;

    bool Approx(f32 a, f32 b, f32 eps = 1e-4f)
    {
        return std::abs(a - b) <= eps;
    }

    bool VecApprox(const vec3& a, const vec3& b, f32 eps = 1e-4f)
    {
        return Approx(a.x, b.x, eps) && Approx(a.y, b.y, eps) && Approx(a.z, b.z, eps);
    }

    // A deterministic Fibonacci-sphere point set, uniform enough that the
    // Monte-Carlo projection of a constant function converges to its analytic
    // value within a loose tolerance — no RNG, so the test is reproducible.
    std::array<vec3, 4096> FibonacciSphere()
    {
        std::array<vec3, 4096> points{};
        const f32 count = static_cast<f32>(points.size());
        const f32 goldenAngle = Pi * (3.0f - std::sqrt(5.0f));
        for (u32 i = 0; i < points.size(); ++i)
        {
            const f32 fi = static_cast<f32>(i);
            const f32 y = 1.0f - (fi / (count - 1.0f)) * 2.0f;
            const f32 radius = std::sqrt(std::max(0.0f, 1.0f - y * y));
            const f32 theta = goldenAngle * fi;
            points[i] = vec3(std::cos(theta) * radius, y, std::sin(theta) * radius);
        }
        return points;
    }

    // Projects a uniform radiance over the whole sphere into a radiance SH set,
    // the constant-ambient setup the invariant and linearity cases share.
    Sh9 ProjectUniform(vec3 radiance)
    {
        const std::array<vec3, 4096> directions = FibonacciSphere();
        const f32 weight = (4.0f * Pi) / static_cast<f32>(directions.size());
        Sh9 sh = Sh9::Zero();
        for (const vec3& direction : directions)
        {
            ProjectSample(sh, direction, radiance, weight);
        }
        return sh;
    }
}

TEST_CASE("ShBasis is the constant-coefficient real SH basis at a known direction")
{
    // +X reads off the constant band, the l=1 x term, and the l=2 (x^2-y^2) term.
    const std::array<f32, 9> basis = ShBasis(vec3(1.0f, 0.0f, 0.0f));
    CHECK(Approx(basis[0], 0.282095f));
    CHECK(Approx(basis[1], 0.0f));
    CHECK(Approx(basis[2], 0.0f));
    CHECK(Approx(basis[3], 0.488603f));
    CHECK(Approx(basis[4], 0.0f));
    CHECK(Approx(basis[5], 0.0f));
    CHECK(Approx(basis[6], 0.315392f * -1.0f));
    CHECK(Approx(basis[7], 0.0f));
    CHECK(Approx(basis[8], 0.546274f));
}

TEST_CASE("Sh9::Zero is all-zero")
{
    const Sh9 zero = Sh9::Zero();
    for (const vec3& coefficient : zero.Coefficients)
    {
        CHECK(VecApprox(coefficient, vec3(0.0f)));
    }
}

TEST_CASE("Constant ambient reconstructs pi*L irradiance independent of normal")
{
    const Sh9 radiance = ProjectUniform(vec3(1.0f, 1.0f, 1.0f));
    const Sh9 irradiance = ConvolveCosine(radiance);

    // The cosine-weighted hemisphere integral of constant radiance L is pi*L,
    // the same for every normal; the higher bands integrate to ~0.
    const vec3 expected = vec3(Pi);
    CHECK(VecApprox(EvalIrradiance(irradiance, vec3(0.0f, 1.0f, 0.0f)), expected, 2e-3f));
    CHECK(VecApprox(EvalIrradiance(irradiance, vec3(1.0f, 0.0f, 0.0f)), expected, 2e-3f));
    CHECK(VecApprox(EvalIrradiance(irradiance, vec3(0.0f, 0.0f, 1.0f)), expected, 2e-3f));

    const vec3 diag = glm::normalize(vec3(1.0f, 1.0f, 1.0f));
    CHECK(VecApprox(EvalIrradiance(irradiance, diag), expected, 2e-3f));
}

TEST_CASE("Constant ambient carries on L0; the higher bands are ~0")
{
    const Sh9 radiance = ProjectUniform(vec3(0.7f, 0.4f, 0.2f));

    // The L0 coefficient of a uniform radiance L is L * Y0 * (the 4pi sphere
    // measure) = L * 0.282095 * 4pi; the second 0.282095 enters only at eval.
    const f32 l0Scale = 0.282095f * 4.0f * Pi;
    // A finite Fibonacci-sphere projection of a constant carries Monte-Carlo
    // error in the coefficients, so the tolerance is loose relative to the
    // analytic ideal.
    CHECK(VecApprox(radiance.Coefficients[0], vec3(0.7f, 0.4f, 0.2f) * l0Scale, 5e-3f));

    for (u32 i = 1; i < ShCoefficientCount; ++i)
    {
        CHECK(VecApprox(radiance.Coefficients[i], vec3(0.0f), 5e-3f));
    }
}

TEST_CASE("ConvolveCosine applies the published per-band A_l factors")
{
    constexpr f32 A0 = Pi;
    constexpr f32 A1 = (2.0f * Pi) / 3.0f;
    constexpr f32 A2 = Pi / 4.0f;

    // A radiance set of all-ones isolates the band factor on each coefficient.
    Sh9 ones = Sh9::Zero();
    for (vec3& coefficient : ones.Coefficients)
    {
        coefficient = vec3(1.0f);
    }
    const Sh9 convolved = ConvolveCosine(ones);

    CHECK(VecApprox(convolved.Coefficients[0], vec3(A0)));
    CHECK(VecApprox(convolved.Coefficients[1], vec3(A1)));
    CHECK(VecApprox(convolved.Coefficients[2], vec3(A1)));
    CHECK(VecApprox(convolved.Coefficients[3], vec3(A1)));
    CHECK(VecApprox(convolved.Coefficients[4], vec3(A2)));
    CHECK(VecApprox(convolved.Coefficients[5], vec3(A2)));
    CHECK(VecApprox(convolved.Coefficients[6], vec3(A2)));
    CHECK(VecApprox(convolved.Coefficients[7], vec3(A2)));
    CHECK(VecApprox(convolved.Coefficients[8], vec3(A2)));
}

TEST_CASE("A single bright direction reconstructs an irradiance lobe peaking at that direction")
{
    // One analytic sample from +Z, cosine-convolved into the diffuse lobe: the
    // irradiance is largest along +Z and falls off smoothly away from it.
    Sh9 sh = Sh9::Zero();
    const vec3 peak = vec3(0.0f, 0.0f, 1.0f);
    ProjectSample(sh, peak, vec3(1.0f), 1.0f);
    const Sh9 lobe = ConvolveCosine(sh);

    const f32 atPeak = EvalIrradiance(lobe, peak).x;
    const f32 atSide = EvalIrradiance(lobe, vec3(1.0f, 0.0f, 0.0f)).x;
    const f32 atOpposite = EvalIrradiance(lobe, vec3(0.0f, 0.0f, -1.0f)).x;

    CHECK(atPeak > atSide);
    CHECK(atSide > atOpposite);
    CHECK(atOpposite >= 0.0f);
}

TEST_CASE("Projection is linear: project(a) + project(b) == project(a + b)")
{
    const vec3 directionA = glm::normalize(vec3(0.3f, 0.8f, -0.5f));
    const vec3 directionB = glm::normalize(vec3(-0.6f, 0.1f, 0.7f));
    const vec3 radianceA = vec3(0.4f, 0.9f, 0.2f);
    const vec3 radianceB = vec3(0.7f, 0.1f, 0.5f);

    Sh9 separate = Sh9::Zero();
    ProjectSample(separate, directionA, radianceA, 1.0f);
    ProjectSample(separate, directionB, radianceB, 1.0f);

    Sh9 first = Sh9::Zero();
    ProjectSample(first, directionA, radianceA, 1.0f);
    Sh9 second = Sh9::Zero();
    ProjectSample(second, directionB, radianceB, 1.0f);

    for (u32 i = 0; i < ShCoefficientCount; ++i)
    {
        const vec3 summed = first.Coefficients[i] + second.Coefficients[i];
        CHECK(VecApprox(separate.Coefficients[i], summed));
    }
}

TEST_CASE("EvalIrradiance matches hand-computed goldens for a fixed coefficient set")
{
    // A fixed, arbitrary single-channel coefficient set, replicated across RGB.
    // The expected values are computed by hand against the same basis polynomials
    // ShBasis emits; a GPU evaluator must reproduce these exact numbers.
    constexpr std::array<f32, 9> C{0.5f, 0.1f, -0.2f, 0.3f, 0.05f, -0.1f, 0.15f, -0.07f, 0.02f};
    Sh9 sh{};
    for (u32 i = 0; i < ShCoefficientCount; ++i)
    {
        sh.Coefficients[i] = vec3(C[i]);
    }

    CHECK(Approx(EvalIrradiance(sh, vec3(1.0f, 0.0f, 0.0f)).x, 0.25124508f));
    CHECK(Approx(EvalIrradiance(sh, vec3(0.0f, 1.0f, 0.0f)).x, 0.13167352f));
    CHECK(Approx(EvalIrradiance(sh, vec3(0.0f, 0.0f, 1.0f)).x, 0.13794450f));

    const vec3 diag = glm::normalize(vec3(1.0f, 1.0f, 1.0f));
    CHECK(Approx(EvalIrradiance(sh, diag).x, 0.15376459f));
}
