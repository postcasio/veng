// The device-free half of the depth-of-field battery: the tile reduction rule the dilation shader
// mirrors, and the two hard quality clamps applied where the per-frame values are pushed into the
// view state. Neither needs a device — the reduction is pure arithmetic and the clamps are the
// authoritative gate on values a cooked level supplies, so an out-of-range ring count can never
// reach the shader as an unbounded loop bound.

#include <doctest/doctest.h>

#include <Veng/Renderer/DofTile.h>
#include <Veng/Scene/Camera.h>

using namespace Veng;
using namespace Veng::Renderer;

TEST_CASE("dof tile: the empty record is the reduction identity")
{
    const DofTile empty = EmptyDofTile();
    CHECK(empty.MaxNearCoc == 0.0f);
    CHECK(empty.MaxFarCoc == 0.0f);

    // Merging the identity with any record leaves that record alone.
    const DofTile record{.MinDepth = 4.0f, .MaxNearCoc = 3.0f, .MaxFarCoc = 7.0f};
    const DofTile merged = MergeDofTiles(empty, record);
    CHECK(merged.MinDepth == doctest::Approx(4.0f));
    CHECK(merged.MaxNearCoc == doctest::Approx(3.0f));
    CHECK(merged.MaxFarCoc == doctest::Approx(7.0f));
}

TEST_CASE("dof tile: the sign of a sample selects the field it contributes to")
{
    DofTile tile = EmptyDofTile();
    tile = AccumulateDofTile(tile, -5.0f, 2.0f); // near field, in front of focus
    tile = AccumulateDofTile(tile, 3.0f, 8.0f);  // far field, behind focus

    CHECK(tile.MinDepth == doctest::Approx(2.0f));
    CHECK(tile.MaxNearCoc == doctest::Approx(5.0f));
    CHECK(tile.MaxFarCoc == doctest::Approx(3.0f));

    // Each side keeps the largest magnitude, and a smaller sample does not shrink it.
    tile = AccumulateDofTile(tile, -1.0f, 9.0f);
    tile = AccumulateDofTile(tile, 1.0f, 9.0f);
    CHECK(tile.MaxNearCoc == doctest::Approx(5.0f));
    CHECK(tile.MaxFarCoc == doctest::Approx(3.0f));
    CHECK(tile.MinDepth == doctest::Approx(2.0f));
}

TEST_CASE("dof tile: a non-positive depth carries no defocus and is dropped")
{
    DofTile tile = EmptyDofTile();
    tile = AccumulateDofTile(tile, 6.0f, 5.0f);

    const DofTile behind = AccumulateDofTile(tile, 40.0f, 0.0f);
    CHECK(behind.MinDepth == doctest::Approx(5.0f));
    CHECK(behind.MaxFarCoc == doctest::Approx(6.0f));

    const DofTile negative = AccumulateDofTile(tile, 40.0f, -3.0f);
    CHECK(negative.MinDepth == doctest::Approx(5.0f));
    CHECK(negative.MaxFarCoc == doctest::Approx(6.0f));
}

TEST_CASE("dof tile: the dilation merge is order independent")
{
    const DofTile a{.MinDepth = 9.0f, .MaxNearCoc = 1.0f, .MaxFarCoc = 12.0f};
    const DofTile b{.MinDepth = 2.0f, .MaxNearCoc = 8.0f, .MaxFarCoc = 4.0f};
    const DofTile c{.MinDepth = 5.0f, .MaxNearCoc = 3.0f, .MaxFarCoc = 6.0f};

    const DofTile left = MergeDofTiles(MergeDofTiles(a, b), c);
    const DofTile right = MergeDofTiles(a, MergeDofTiles(c, b));
    CHECK(left.MinDepth == doctest::Approx(right.MinDepth));
    CHECK(left.MaxNearCoc == doctest::Approx(right.MaxNearCoc));
    CHECK(left.MaxFarCoc == doctest::Approx(right.MaxFarCoc));
    CHECK(left.MinDepth == doctest::Approx(2.0f));
    CHECK(left.MaxNearCoc == doctest::Approx(8.0f));
    CHECK(left.MaxFarCoc == doctest::Approx(12.0f));
}

TEST_CASE("dof clamps: an out-of-range authored ring count never reaches the shader")
{
    // The gather's loop bound. A cooked level is untrusted input, so anything above the
    // supported ceiling is clamped rather than trusted.
    CHECK(ClampDofRingCount(4) == 4u);
    CHECK(ClampDofRingCount(MaxDofRings) == MaxDofRings);
    CHECK(ClampDofRingCount(MaxDofRings + 1) == MaxDofRings);
    CHECK(ClampDofRingCount(4096) == MaxDofRings);
    CHECK(ClampDofRingCount(0) == 1u);
}

TEST_CASE("dof clamps: an out-of-range authored max circle of confusion is bounded")
{
    CHECK(ClampDofMaxCoc(16.0f) == doctest::Approx(16.0f));
    CHECK(ClampDofMaxCoc(MaxDofCoc) == doctest::Approx(MaxDofCoc));
    CHECK(ClampDofMaxCoc(MaxDofCoc + 1.0f) == doctest::Approx(MaxDofCoc));
    CHECK(ClampDofMaxCoc(1.0e9f) == doctest::Approx(MaxDofCoc));
    CHECK(ClampDofMaxCoc(-4.0f) == doctest::Approx(0.0f));
}

TEST_CASE("dof tile: the reduction agrees with the reference circle-of-confusion curve")
{
    // The tile records what the prefilter wrote, and the prefilter mirrors
    // ComputeCircleOfConfusion. Reducing a focus-bracketing pair therefore reproduces the curve's
    // two sides: a nearer-than-focus sample lands in the near field, a farther one in the far
    // field, and the far side converges below CocScale * Aperture.
    const DofParams params{.Aperture = 0.0179f, .FocusDistance = 10.0f, .CocScale = 45000.0f};
    const f32 nearCoc = 0.5f * ComputeCircleOfConfusion(params, 4.0f);
    const f32 farCoc = 0.5f * ComputeCircleOfConfusion(params, 40.0f);
    REQUIRE(nearCoc < 0.0f);
    REQUIRE(farCoc > 0.0f);

    DofTile tile = EmptyDofTile();
    tile = AccumulateDofTile(tile, nearCoc, 4.0f);
    tile = AccumulateDofTile(tile, farCoc, 40.0f);

    CHECK(tile.MinDepth == doctest::Approx(4.0f));
    CHECK(tile.MaxNearCoc == doctest::Approx(-nearCoc));
    CHECK(tile.MaxFarCoc == doctest::Approx(farCoc));
    CHECK(tile.MaxFarCoc < 0.5f * params.CocScale * params.Aperture);
}
