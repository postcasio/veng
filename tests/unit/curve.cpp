// Curve1D: each interpolation between two keys, the hold outside the keys, the empty curve, and two
// keys sharing a time authoring an instant jump. Pure arithmetic, so it runs in the default band.

#include <doctest/doctest.h>

#include <Veng/Math/Curve.h>

using namespace Veng;

namespace
{
    Curve1D Ramp(const CurveInterp interp)
    {
        return Curve1D{.Keys = {{.Time = 1.0f, .Value = 2.0f, .Interp = interp},
                                {.Time = 3.0f, .Value = 6.0f}}};
    }
}

TEST_CASE("curve: an empty curve evaluates to zero everywhere")
{
    const Curve1D curve;
    CHECK(curve.IsEmpty());
    CHECK(curve.Evaluate(-1.0f) == 0.0f);
    CHECK(curve.Evaluate(0.0f) == 0.0f);
    CHECK(curve.Evaluate(5.0f) == 0.0f);
}

TEST_CASE("curve: values outside the keys hold the nearest key")
{
    const Curve1D curve = Ramp(CurveInterp::Linear);
    CHECK(curve.Evaluate(-10.0f) == 2.0f);
    CHECK(curve.Evaluate(1.0f) == 2.0f);
    CHECK(curve.Evaluate(3.0f) == 6.0f);
    CHECK(curve.Evaluate(100.0f) == 6.0f);
}

TEST_CASE("curve: linear interpolates proportionally")
{
    const Curve1D curve = Ramp(CurveInterp::Linear);
    CHECK(curve.Evaluate(2.0f) == doctest::Approx(4.0f));
    CHECK(curve.Evaluate(1.5f) == doctest::Approx(3.0f));
}

TEST_CASE("curve: step holds the earlier key until the next")
{
    const Curve1D curve = Ramp(CurveInterp::Step);
    CHECK(curve.Evaluate(1.0f) == 2.0f);
    CHECK(curve.Evaluate(2.999f) == 2.0f);
    CHECK(curve.Evaluate(3.0f) == 6.0f);
}

TEST_CASE("curve: smooth eases with flat ends and passes the midpoint")
{
    const Curve1D curve = Ramp(CurveInterp::Smooth);
    CHECK(curve.Evaluate(2.0f) == doctest::Approx(4.0f));

    // Flat tangents: a quarter of the way in, the curve has covered less than a linear ramp would,
    // and three quarters in, more.
    const f32 quarter = curve.Evaluate(1.5f);
    const f32 threeQuarter = curve.Evaluate(2.5f);
    CHECK(quarter < 3.0f);
    CHECK(quarter > 2.0f);
    CHECK(threeQuarter > 5.0f);
    CHECK(threeQuarter < 6.0f);
    CHECK(quarter + threeQuarter == doctest::Approx(8.0f));
}

TEST_CASE("curve: two keys at one time jump to the later key's value")
{
    const Curve1D curve{.Keys = {{.Time = 0.0f, .Value = 0.0f},
                                 {.Time = 1.0f, .Value = 1.0f},
                                 {.Time = 1.0f, .Value = 0.25f},
                                 {.Time = 2.0f, .Value = 0.25f}}};
    CHECK(curve.Evaluate(0.5f) == doctest::Approx(0.5f));
    CHECK(curve.Evaluate(1.0f) == 0.25f);
    CHECK(curve.Evaluate(1.5f) == 0.25f);
}
