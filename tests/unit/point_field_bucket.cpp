// PointField::Bucket cases — the device-free CPU half of a field build. Bucket groups a point
// set into a uniform cull grid and rewrites it cell-sorted, so the prebucketed Create is
// upload-only (the seam that lets a consumer bucket on a TaskSystem worker). These cases pin the
// build's invariants without a Context: the sorted set is a permutation of the input, each cell
// indexes a contiguous run whose points share that cell's grid coordinate and sit inside its
// bounds, the whole-build bounds union the cells, and the per-cell flux sums the constant-
// surface-brightness color * Size^2 the aggregate splat spreads.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

#include <Veng/Renderer/PointField.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    u32 PackRgba8(const u8 r, const u8 g, const u8 b)
    {
        return static_cast<u32>(r) | (static_cast<u32>(g) << 8) | (static_cast<u32>(b) << 16) |
               (0xFFu << 24);
    }

    // The grid coordinate Bucket buckets a position into (floor per axis at the cell size).
    ivec3 GridCoord(const vec3 position, const f32 cellSize)
    {
        return ivec3{static_cast<i32>(std::floor(position.x / cellSize)),
                     static_cast<i32>(std::floor(position.y / cellSize)),
                     static_cast<i32>(std::floor(position.z / cellSize))};
    }
}

TEST_CASE("PointField::Bucket cell-sorts a permutation of the input into disjoint cell runs")
{
    constexpr f32 CellSize = 10.0f;

    // Two spatially separated clusters plus a lone point, interleaved in input order so the
    // cell-sorting genuinely reorders.
    vector<FieldPoint> points;
    for (u32 i = 0; i < 8; ++i)
    {
        const f32 t = static_cast<f32>(i);
        points.push_back(FieldPoint{.Position = vec3{1.0f + t * 0.5f, 2.0f, 3.0f},
                                    .ColorRgba8 = PackRgba8(255, 0, 0),
                                    .Size = 1.0f});
        points.push_back(FieldPoint{.Position = vec3{101.0f + t * 0.5f, 2.0f, 3.0f},
                                    .ColorRgba8 = PackRgba8(0, 255, 0),
                                    .Size = 2.0f});
    }
    points.push_back(FieldPoint{.Position = vec3{-55.0f, -55.0f, -55.0f},
                                .ColorRgba8 = PackRgba8(0, 0, 255),
                                .Size = 3.0f});

    const PointField::BuildData build = PointField::Bucket(points, CellSize);

    // Same points, reordered: sizes match and every input position appears exactly once.
    REQUIRE(build.Points.size() == points.size());
    for (const FieldPoint& point : points)
    {
        const auto matches = std::ranges::count_if(build.Points, [&](const FieldPoint& p)
                                                   { return p.Position == point.Position; });
        CHECK(matches == 1);
    }

    // The cell runs are contiguous, disjoint, and cover the whole sorted set.
    u64 covered = 0;
    for (const PointField::Cell& cell : build.Cells)
    {
        CHECK(cell.PointCount > 0);
        CHECK(cell.FirstPoint + cell.PointCount <= build.Points.size());
        covered += cell.PointCount;

        // Every point in the run shares the cell's grid coordinate and sits inside its bounds.
        const ivec3 coord = GridCoord(build.Points[cell.FirstPoint].Position, CellSize);
        for (u32 i = cell.FirstPoint; i < cell.FirstPoint + cell.PointCount; ++i)
        {
            const vec3 position = build.Points[i].Position;
            CHECK(GridCoord(position, CellSize) == coord);
            CHECK(glm::all(glm::greaterThanEqual(position, cell.Bounds.Min)));
            CHECK(glm::all(glm::lessThanEqual(position, cell.Bounds.Max)));
        }
    }
    CHECK(covered == build.Points.size());
}

TEST_CASE("PointField::Bucket bounds union the cells and the flux sums color * Size^2")
{
    constexpr f32 CellSize = 4.0f;

    // One cell's worth of points with known colors and sizes, so the flux sum is checkable by hand.
    const vector<FieldPoint> points{
        FieldPoint{
            .Position = vec3{1.0f, 1.0f, 1.0f}, .ColorRgba8 = PackRgba8(255, 0, 0), .Size = 2.0f},
        FieldPoint{
            .Position = vec3{2.0f, 1.0f, 1.0f}, .ColorRgba8 = PackRgba8(0, 255, 0), .Size = 3.0f},
    };

    const PointField::BuildData build = PointField::Bucket(points, CellSize);
    REQUIRE(build.Cells.size() == 1);

    const PointField::Cell& cell = build.Cells.front();
    // Red at Size 2 contributes (4, 0, 0); green at Size 3 contributes (0, 9, 0).
    CHECK(cell.SummedFlux.x == doctest::Approx(4.0f));
    CHECK(cell.SummedFlux.y == doctest::Approx(9.0f));
    CHECK(cell.SummedFlux.z == doctest::Approx(0.0f));

    // The centroid averages the two positions; the build bounds equal the one cell's bounds.
    CHECK(cell.Centroid.x == doctest::Approx(1.5f));
    CHECK(build.Bounds.Min == cell.Bounds.Min);
    CHECK(build.Bounds.Max == cell.Bounds.Max);
}

TEST_CASE("PointField::Bucket of an empty set is an empty build")
{
    const PointField::BuildData build = PointField::Bucket({}, 8.0f);
    CHECK(build.Points.empty());
    CHECK(build.Cells.empty());
}
