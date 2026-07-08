// Gui::Placement — device-free rect-placement helpers. Pure glm value math, no GPU and no
// Document, so this is CPU-only: ClampIntoBounds' interior/edge/oversized/degenerate cases, and
// AnchorBeside composing an offset with the clamp.

#include <doctest/doctest.h>

#include <Veng/Gui/Placement.h>

#include <cmath>

using namespace Veng;
using namespace Veng::Gui;

TEST_CASE("ClampIntoBounds leaves an interior rect untouched")
{
    const vec2 result =
        ClampIntoBounds(vec2(40.0f, 30.0f), vec2(20.0f, 10.0f), vec2(200.0f, 100.0f), 4.0f);
    CHECK(result == vec2(40.0f, 30.0f));
}

TEST_CASE("ClampIntoBounds slides a rect crossing the near edge to the margin")
{
    const vec2 result =
        ClampIntoBounds(vec2(-10.0f, -20.0f), vec2(20.0f, 10.0f), vec2(200.0f, 100.0f), 4.0f);
    CHECK(result == vec2(4.0f, 4.0f));
}

TEST_CASE("ClampIntoBounds slides a rect crossing the far edge to bounds - margin - size")
{
    const vec2 result =
        ClampIntoBounds(vec2(190.0f, 95.0f), vec2(20.0f, 10.0f), vec2(200.0f, 100.0f), 4.0f);
    // 200 - 4 - 20 = 176; 100 - 4 - 10 = 86.
    CHECK(result == vec2(176.0f, 86.0f));
}

TEST_CASE("ClampIntoBounds clamps each axis independently")
{
    // x sits inside the valid range while y overshoots past the far edge.
    const vec2 result =
        ClampIntoBounds(vec2(50.0f, 200.0f), vec2(20.0f, 10.0f), vec2(200.0f, 100.0f), 4.0f);
    CHECK(result == vec2(50.0f, 86.0f));
}

TEST_CASE("ClampIntoBounds collapses to the margin corner when the rect oversizes the bounds")
{
    // A 300x300 rect can never fit inside a 200x100 area even ignoring the margin: the valid
    // range on both axes collapses to a single point at (margin, margin), not an inverted range.
    const vec2 result =
        ClampIntoBounds(vec2(1000.0f, -1000.0f), vec2(300.0f, 300.0f), vec2(200.0f, 100.0f), 4.0f);
    CHECK(result == vec2(4.0f, 4.0f));
    CHECK_FALSE(std::isnan(result.x));
    CHECK_FALSE(std::isnan(result.y));
}

TEST_CASE("ClampIntoBounds collapses to the margin corner against a zero-size bounds")
{
    const vec2 result =
        ClampIntoBounds(vec2(5.0f, 5.0f), vec2(10.0f, 10.0f), vec2(0.0f, 0.0f), 2.0f);
    CHECK(result == vec2(2.0f, 2.0f));
    CHECK_FALSE(std::isnan(result.x));
    CHECK_FALSE(std::isnan(result.y));
}

TEST_CASE("AnchorBeside offsets from the anchor when the result stays interior")
{
    const vec2 result = AnchorBeside(vec2(50.0f, 50.0f), vec2(20.0f, 10.0f), vec2(8.0f, -4.0f),
                                     vec2(200.0f, 100.0f), 4.0f);
    CHECK(result == vec2(58.0f, 46.0f));
}

TEST_CASE("AnchorBeside clamps the offset result exactly as ClampIntoBounds would")
{
    // An anchor near the top-left with a negative offset pushes the proposed top-left past the
    // margin; AnchorBeside clamps it back rather than drawing off-bounds.
    const vec2 offsetResult = AnchorBeside(vec2(2.0f, 2.0f), vec2(20.0f, 10.0f),
                                           vec2(-10.0f, -10.0f), vec2(200.0f, 100.0f), 4.0f);
    const vec2 clampResult =
        ClampIntoBounds(vec2(-8.0f, -8.0f), vec2(20.0f, 10.0f), vec2(200.0f, 100.0f), 4.0f);
    CHECK(offsetResult == clampResult);
    CHECK(offsetResult == vec2(4.0f, 4.0f));
}
