// The cube-sphere's device-free math: the face mapping round-trips, tile keys navigate and fold
// across face edges symmetrically, the warp fraction is exact at a tile's edges and precise at
// depth, and the f32 direction offset places a texel to the millimetre on a sphere thousands of kilometres across.

#include <array>
#include <cmath>
#include <limits>

#include <doctest/doctest.h>

#include <Veng/Math/CubeSphere.h>

using namespace Veng;

namespace
{
    f64 AngleBetween(const dvec3& a, const dvec3& b)
    {
        return std::atan2(glm::length(glm::cross(a, b)), glm::dot(a, b));
    }

    // The tiles a case walks: every edge and corner tile of every face at a level.
    vector<CubeSphereTileKey> EdgeTiles(const u32 level)
    {
        const u32 last = (1u << level) - 1;
        vector<CubeSphereTileKey> tiles;
        for (u32 face = 0; face < CubeSphere::FaceCount; face++)
        {
            const auto f = static_cast<CubeSphere::Face>(face);
            for (const u32 x : {0u, last / 2, last})
            {
                for (const u32 y : {0u, last / 2, last})
                {
                    if (x == 0 || y == 0 || x == last || y == last)
                    {
                        tiles.push_back(CubeSphereTileKey::Make(f, level, x, y));
                    }
                }
            }
        }
        return tiles;
    }

    f32 UlpDistance(const f32 a, const f64 reference)
    {
        const f32 r = static_cast<f32>(reference);
        const f32 ulp =
            std::nextafter(std::abs(r), std::numeric_limits<f32>::infinity()) - std::abs(r);
        return static_cast<f32>(std::abs(static_cast<f64>(a) - reference) / ulp);
    }
}

TEST_CASE("cube sphere: a direction round-trips through its face coordinates")
{
    const f64 nearCorner = 1e-7;
    const std::array<dvec3, 8> directions = {
        dvec3(1, 0, 0),
        dvec3(0, 0, -1),
        dvec3(1, 1, 0),
        dvec3(1, 1, 1),
        dvec3(-1, 0.3, 0.7),
        dvec3(0.2, -1, 0.999),
        glm::normalize(dvec3(1, 1, 1)) + dvec3(nearCorner, 0, 0),
        dvec3(-0.5, 0.25, -1),
    };
    f64 worst = 0.0;
    for (const dvec3& direction : directions)
    {
        const dvec3 unit = glm::normalize(direction);
        const CubeSphere::FacePoint point = CubeSphere::DirectionToFace(unit);
        CHECK(std::abs(point.U) <= 1.0);
        CHECK(std::abs(point.V) <= 1.0);
        worst = std::max(
            worst, AngleBetween(unit, CubeSphere::FaceToDirection(point.OnFace, point.U, point.V)));
    }
    CHECK(worst < 1e-12);
}

TEST_CASE("cube sphere: a face basis is right-handed")
{
    for (u32 face = 0; face < CubeSphere::FaceCount; face++)
    {
        const CubeSphere::FaceBasis& basis =
            CubeSphere::GetFaceBasis(static_cast<CubeSphere::Face>(face));
        CHECK(glm::length(glm::cross(basis.U, basis.V) - basis.Normal) == 0.0);
    }
}

TEST_CASE("cube sphere: a tile key round-trips and its children tile it")
{
    const CubeSphereTileKey key =
        CubeSphereTileKey::Make(CubeSphere::Face::NegY, 24, 0xABCDEF, 0x123456);
    CHECK(key.GetFace() == CubeSphere::Face::NegY);
    CHECK(key.GetLevel() == 24u);
    CHECK(key.GetX() == 0xABCDEFu);
    CHECK(key.GetY() == 0x123456u);
    CHECK(CubeSphereTileKey::FromValue(key.Value) == key);

    const CubeSphereTileKey parent = CubeSphereTileKey::Make(CubeSphere::Face::PosZ, 7, 37, 90);
    const CubeSphereTileGeometry whole = GetCubeSphereTileGeometry(parent);
    u32 quadrants = 0;
    for (u32 i = 0; i < 4; i++)
    {
        const CubeSphereTileKey child = parent.GetChild(i);
        CHECK(child.GetParent() == parent);
        CHECK(child.GetQuadrant() == i);
        CHECK(parent.IsAncestorOf(child));
        CHECK_FALSE(child.IsAncestorOf(parent));
        quadrants |= 1u << child.GetQuadrant();
        // Each child's outer corner is its parent's corner of the same index, to the bit.
        CHECK(GetCubeSphereTileGeometry(child).Corners[i] == whole.Corners[i]);
    }
    CHECK(quadrants == 0xFu);
    // The four children meet at one point: each child's corner opposite its own index.
    const dvec3 middle = GetCubeSphereTileGeometry(parent.GetChild(0)).Corners[3];
    for (u32 i = 1; i < 4; i++)
    {
        CHECK(GetCubeSphereTileGeometry(parent.GetChild(i)).Corners[3 - i] == middle);
    }
}

TEST_CASE("cube sphere: a neighbour's neighbour across the matching edge is the tile")
{
    u32 asymmetric = 0;
    u32 notTouching = 0;
    u32 checked = 0;
    for (const u32 level : {0u, 1u, 7u, 24u})
    {
        for (const CubeSphereTileKey tile : EdgeTiles(level))
        {
            const CubeSphereTileGeometry own = GetCubeSphereTileGeometry(tile);
            for (u32 edge = 0; edge < 4; edge++)
            {
                const CubeSphereTileKey neighbour =
                    tile.GetNeighbour(static_cast<CubeSphere::Edge>(edge));
                CHECK(neighbour.GetLevel() == level);
                bool back = false;
                for (u32 other = 0; other < 4; other++)
                {
                    back = back ||
                           neighbour.GetNeighbour(static_cast<CubeSphere::Edge>(other)) == tile;
                }
                asymmetric += back ? 0u : 1u;

                // The two share an edge: two corners in common, to rounding.
                const CubeSphereTileGeometry theirs = GetCubeSphereTileGeometry(neighbour);
                u32 shared = 0;
                for (const dvec3& a : own.Corners)
                {
                    for (const dvec3& b : theirs.Corners)
                    {
                        shared += glm::length(a - b) < 1e-12 ? 1u : 0u;
                    }
                }
                notTouching += shared == 2 ? 0u : 1u;
                checked++;
            }
        }
    }
    CHECK(checked > 0u);
    CHECK(asymmetric == 0u);
    CHECK(notTouching == 0u);
}

TEST_CASE("cube sphere: the warp fraction is exact at the edges, increasing, and precise at depth")
{
    for (const u32 level : {0u, 5u, 24u})
    {
        const CubeSphereTileKey key = CubeSphereTileKey::Make(CubeSphere::Face::PosX, level,
                                                              (1u << level) / 3, (1u << level) - 1);
        const CubeSphereTileGeometry geometry = GetCubeSphereTileGeometry(key);
        const auto x0 = static_cast<f32>(geometry.CubeStartU);
        const auto span = static_cast<f32>(geometry.ThetaSpan);
        CHECK(CubeSphere::TileFractionF32(0.0f, x0, span) == 0.0f);
        CHECK(CubeSphere::TileFractionF32(1.0f, x0, span) == 1.0f);
        CHECK(CubeSphere::TileFraction(0.0, geometry.ThetaStartU, geometry.ThetaSpan) == 0.0);
        CHECK(CubeSphere::TileFraction(1.0, geometry.ThetaStartU, geometry.ThetaSpan) == 1.0);

        u32 decreases = 0;
        f32 previous = CubeSphere::TileFractionF32(-0.02f, x0, span);
        for (u32 i = 1; i <= 104; i++)
        {
            const f32 s = -0.02f + (static_cast<f32>(i) * 0.01f);
            const f32 fraction = CubeSphere::TileFractionF32(s, x0, span);
            decreases += fraction > previous ? 0u : 1u;
            previous = fraction;
        }
        CHECK(decreases == 0u);
    }

    // At level 24 the f32 form holds within a few ulp of the double one.
    const CubeSphereTileKey deep =
        CubeSphereTileKey::Make(CubeSphere::Face::NegZ, 24, 11000000, 300);
    const CubeSphereTileGeometry geometry = GetCubeSphereTileGeometry(deep);
    f32 worst = 0.0f;
    for (const f32 s : {0.1f, 0.25f, 0.5f, 0.75f, 0.9f})
    {
        const f64 reference = CubeSphere::TileFraction(s, geometry.ThetaStartU, geometry.ThetaSpan);
        const f32 fraction = CubeSphere::TileFractionF32(s, static_cast<f32>(geometry.CubeStartU),
                                                         static_cast<f32>(geometry.ThetaSpan));
        worst = std::max(worst, UlpDistance(fraction, reference));
    }
    CHECK(worst <= 4.0f);
}

TEST_CASE("cube sphere: the f32 direction offset places a deep texel to the millimetre")
{
    constexpr f64 Radius = 6.4e6;
    f64 worst = 0.0;
    for (const CubeSphereTileKey key :
         {CubeSphereTileKey::Make(CubeSphere::Face::PosY, 22, 1234567, 3456789),
          CubeSphereTileKey::Make(CubeSphere::Face::NegX, 22, 0, 4194303),
          CubeSphereTileKey::Make(CubeSphere::Face::PosZ, 22, 2097152, 2097151)})
    {
        const CubeSphereTileGeometry geometry = GetCubeSphereTileGeometry(key);
        const CubeSphereTileOffsetInputs inputs = GetCubeSphereTileOffsetInputs(geometry);
        for (const f32 s : {0.0f, 0.37f, 1.0f, -0.015625f})
        {
            for (const f32 t : {0.0f, 0.5f, 1.0f})
            {
                const vec3 offset = CubeSphereTileOffsetF32(inputs, vec3(geometry.Center), s, t);
                const dvec3 placed = (geometry.Center + dvec3(offset)) * Radius;
                const dvec3 reference = CubeSphereTileDirection(geometry, s, t) * Radius;
                worst = std::max(worst, glm::length(placed - reference));
            }
        }
    }
    CHECK(worst < 1e-3);
}
