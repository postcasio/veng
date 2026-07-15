// Volume-march pure math (no ICD): the ray/AABB segment clipped against the reconstructed scene
// depth (mirrored by volume_field.frag) and the far-to-near draw-order comparator the multi-field
// composite relies on. Pinned inside / outside / behind-geometry / miss cases so the shader's own
// slab test has a CPU oracle to agree with.

#include <doctest/doctest.h>

#include <Veng/Math/AABB.h>
#include <Veng/Renderer/VolumeMarch.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    // The unit box centered at the origin, the fixture every segment case aims a ray at.
    AABB UnitBox()
    {
        return AABB{.Min = vec3(-1.0f), .Max = vec3(1.0f)};
    }

    constexpr f32 Far = 1e30f; // the cleared-background "no geometry" scene distance
}

TEST_CASE("ComputeMarchSegment: a ray from outside enters and exits the far/near faces")
{
    // Origin 5 units in front of a box spanning z in [-1, 1]: enters at t=4, exits at t=6.
    const auto seg = ComputeMarchSegment(vec3(0, 0, -5), vec3(0, 0, 1), UnitBox(), Far);
    REQUIRE(seg.has_value());
    CHECK(seg->Enter == doctest::Approx(4.0f));
    CHECK(seg->Exit == doctest::Approx(6.0f));
}

TEST_CASE(
    "ComputeMarchSegment: a camera inside the box marches from the origin (Enter clamped to 0)")
{
    const auto seg = ComputeMarchSegment(vec3(0, 0, 0), vec3(0, 0, 1), UnitBox(), Far);
    REQUIRE(seg.has_value());
    CHECK(seg->Enter == doctest::Approx(0.0f));
    CHECK(seg->Exit == doctest::Approx(1.0f));
}

TEST_CASE("ComputeMarchSegment: geometry in front of the near face occludes the field entirely")
{
    // The box's near face is at t=4; opaque geometry at t=3 (in front) leaves an empty segment.
    const auto seg = ComputeMarchSegment(vec3(0, 0, -5), vec3(0, 0, 1), UnitBox(), 3.0f);
    CHECK_FALSE(seg.has_value());
}

TEST_CASE("ComputeMarchSegment: geometry inside the box shortens the march to the surface")
{
    // Geometry at t=5 (between the near face at 4 and the far face at 6): march [4, 5].
    const auto seg = ComputeMarchSegment(vec3(0, 0, -5), vec3(0, 0, 1), UnitBox(), 5.0f);
    REQUIRE(seg.has_value());
    CHECK(seg->Enter == doctest::Approx(4.0f));
    CHECK(seg->Exit == doctest::Approx(5.0f));
}

TEST_CASE("ComputeMarchSegment: a ray that misses the box returns nullopt")
{
    // Offset in x so the ray never crosses the [-1, 1] x-slab.
    CHECK_FALSE(ComputeMarchSegment(vec3(5, 0, -5), vec3(0, 0, 1), UnitBox(), Far).has_value());

    // A ray parallel to the box but outside it (z stays at -5, outside the z-slab) also misses.
    CHECK_FALSE(ComputeMarchSegment(vec3(0, 0, -5), vec3(0, 1, 0), UnitBox(), Far).has_value());
}

TEST_CASE("VolumeFieldFartherFirst: orders two boxes far-to-near by camera distance to center")
{
    const vec3 camera(0.0f);
    const AABB near{.Min = vec3(1, -1, -1), .Max = vec3(3, 1, 1)}; // center at (2,0,0)
    const AABB far{.Min = vec3(9, -1, -1), .Max = vec3(11, 1, 1)}; // center at (10,0,0)

    // The far box precedes the near box under the strict-weak "less" predicate (back-to-front).
    CHECK(VolumeFieldFartherFirst(camera, far, near));
    CHECK_FALSE(VolumeFieldFartherFirst(camera, near, far));
    // Irreflexive: a box is never farther than itself.
    CHECK_FALSE(VolumeFieldFartherFirst(camera, near, near));
}
