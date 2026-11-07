// Sphere terrain's vertex placement and morph, through the C++ twin the terrain stages mirror
// (src/Renderer/SphereTerrainVertex.h): placement holds millimetres under the eye on a large sphere
// and its relative precision far from it; a fully morphed tile is its parent, vertex for vertex and
// triangle for triangle; the morph is continuous in the eye distance; and in the steady state every
// drawn edge is placed by both its tiles within the skirt each hangs.

#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include <doctest/doctest.h>

#include <Veng/Math/CubeSphere.h>

#include <Renderer/SphereTerrainDraw.h>
#include <Renderer/SphereTerrainResidency.h>
#include <Renderer/SphereTerrainSelection.h>
#include <Renderer/SphereTerrainVertex.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    constexpr f64 Radius = 6.4e6;
    constexpr f32 Quads = static_cast<f32>(SphereTerrainGridQuads);

    SphereTerrainDrawView OpenView()
    {
        SphereTerrainDrawView view;
        view.Planes.fill(glm::dvec4(0.0, 0.0, 0.0, 1.0));
        return view;
    }

    // Records for drawn tiles of a sphere centred on the origin with an unrotated frame, seen from
    // an eye; every tile is kept, in drawn order.
    vector<SphereTerrainDrawRecord> Records(std::span<const SphereTerrainDrawnTile> drawn,
                                            const dvec3& eye)
    {
        const SphereTerrainFrameInput input{
            .Drawn = drawn,
            .Radius = Radius,
            .Eye = eye,
            .CenterFromEye = -eye,
            .PrevCenterFromCamera = -eye,
            .Main = OpenView(),
        };
        return BuildSphereTerrainFrame(input).Records;
    }

    CubeSphereTileKey TileAt(const dvec3& direction, const u32 level)
    {
        const CubeSphere::FacePoint point = CubeSphere::DirectionToFace(direction);
        const u32 n = 1u << level;
        const auto cell = [n](const f64 c)
        { return std::min(static_cast<u32>((c + 1.0) * 0.5 * n), n - 1); };
        return CubeSphereTileKey::Make(point.OnFace, level, cell(point.U), cell(point.V));
    }

    SphereTerrainDrawnTile Drawn(const CubeSphereTileKey key, const f32 morphStart,
                                 const f32 morphEnd)
    {
        return {.Key = key,
                .Layer = 0,
                .ParentLayer = 1,
                .MorphStart = morphStart,
                .MorphEnd = morphEnd};
    }

    // A smooth height over the sphere, in metres.
    f64 Height(const dvec3& direction)
    {
        return 40.0 * std::sin(900.0 * direction.x) + 25.0 * std::cos(700.0 * direction.y + 0.4);
    }

    // The height a tile's grid vertex holds: the field at its direction.
    f32 GridHeight(const CubeSphereTileKey key, const ivec2 grid)
    {
        const dvec3 direction = CubeSphereTileDirection(GetCubeSphereTileGeometry(key),
                                                        grid.x / static_cast<f64>(Quads),
                                                        grid.y / static_cast<f64>(Quads));
        return static_cast<f32>(Height(direction));
    }

    dvec3 Direction()
    {
        return glm::normalize(dvec3(0.83, 0.31, -0.47));
    }
}

TEST_CASE("sphere terrain vertex: placement holds millimetres under the eye on a large sphere")
{
    const std::array<f32, 3> params = {0.03f, 0.5f, 0.97f};
    const auto WorstError = [&](const u32 level, const dvec3& eye, const f32 height)
    {
        const CubeSphereTileKey key = TileAt(Direction(), level);
        const SphereTerrainDrawnTile tile = Drawn(key, 1e30f, 1e30f);
        const SphereTerrainDrawRecord record = Records({&tile, 1}, eye)[0];
        const CubeSphereTileGeometry geometry = GetCubeSphereTileGeometry(key);
        f64 worst = 0.0;
        for (const f32 s : params)
        {
            for (const f32 t : params)
            {
                const vec3 placed = PlaceSphereTerrainVertex(record, {s, t}, height).Position;
                const dvec3 exact =
                    (CubeSphereTileDirection(geometry, s, t) * (Radius + height)) - eye;
                worst = std::max(worst, glm::length(dvec3(placed) - exact));
            }
        }
        return worst;
    };

    // Level 22 is about 2.4 m across; the eye stands 2 m above the tile.
    const f64 under = WorstError(22, Direction() * (Radius + 2.0), 0.0f);
    CHECK(under < 1e-3);

    // Level 8, from 10,000 km: the f32 rounding of the eye-relative corners, relative to the eye
    // distance.
    const dvec3 far = Direction() * (Radius + 1.0e7);
    CHECK(WorstError(8, far, 120.0f) / 1.0e7 < 1e-6);
}

TEST_CASE("sphere terrain vertex: a fully morphed tile is its parent")
{
    const CubeSphereTileKey child = TileAt(Direction(), 10).GetChild(3).GetChild(1);
    const CubeSphereTileKey parent = child.GetParent();
    const dvec3 eye = Direction() * (Radius + 2000.0);
    // A morph that has ended at any distance: the child is wholly its parent's shape.
    const std::array<SphereTerrainDrawnTile, 2> tiles = {Drawn(child, 0.0f, 1e-3f),
                                                         Drawn(parent, 1e30f, 1e30f)};
    const vector<SphereTerrainDrawRecord> records = Records(tiles, eye);
    REQUIRE(records.size() == 2);

    // Every child vertex lands on a parent grid vertex, at the parent's height, where the parent
    // places that vertex.
    f64 worst = 0.0;
    u32 offGrid = 0;
    const auto own = [&](const ivec2 g) { return GridHeight(child, g); };
    const auto parentHeight = [&](const ivec2 g) { return GridHeight(parent, g); };
    for (u32 index = 0; index < SphereTerrainSurfaceVertices; index++)
    {
        const SphereTerrainGridVertex vertex = DecodeSphereTerrainGridVertex(index);
        const SphereTerrainEvaluated evaluated =
            EvaluateSphereTerrainVertex(records[0], vertex, own, parentHeight);
        const vec2 onParent = SphereTerrainParentGrid(evaluated.St * Quads, child.GetQuadrant());
        offGrid += glm::all(glm::equal(onParent, glm::round(onParent))) ? 0u : 1u;
        const ivec2 p(glm::round(onParent));
        const vec3 placed =
            PlaceSphereTerrainVertex(records[1], vec2(p) / Quads, parentHeight(p)).Position;
        worst =
            std::max(worst, static_cast<f64>(glm::length(evaluated.Placement.Position - placed)));
    }
    // Two records round their corners to f32 independently: about 1e-6 of the eye distance plus
    // the tile's span.
    CHECK(offGrid == 0u);
    CHECK(worst < 1e-6 * 4000.0);

    // The child's non-degenerate triangles are exactly the parent's over its quadrant.
    const std::array<u16, SphereTerrainGridIndexCount> indices = BuildSphereTerrainGridIndices();
    const auto Canonical = [](std::array<i32, 3> t)
    {
        std::ranges::rotate(t, std::ranges::min_element(t));
        return t;
    };
    const auto ParentVertex = [](const vec2 g)
    { return static_cast<i32>(g.y) * 33 + static_cast<i32>(g.x); };
    std::set<std::array<i32, 3>> fromChild;
    std::set<std::array<i32, 3>> fromParent;
    const ivec2 low(static_cast<i32>(child.GetQuadrant() & 1u) * 16,
                    static_cast<i32>(child.GetQuadrant() >> 1) * 16);
    for (u32 i = 0; i < SphereTerrainGridIndexCount; i += 3)
    {
        std::array<i32, 3> mapped{};
        std::array<i32, 3> plain{};
        bool surface = true;
        bool inQuadrant = true;
        for (u32 c = 0; c < 3; c++)
        {
            const SphereTerrainGridVertex v = DecodeSphereTerrainGridVertex(indices[i + c]);
            surface = surface && !v.Skirt;
            mapped[c] = ParentVertex(
                SphereTerrainParentGrid(MorphSphereTerrainGrid(v.Grid, 1.0f), child.GetQuadrant()));
            plain[c] = ParentVertex(vec2(v.Grid));
            inQuadrant = inQuadrant && glm::all(glm::greaterThanEqual(v.Grid, low)) &&
                         glm::all(glm::lessThanEqual(v.Grid, low + ivec2(16)));
        }
        if (!surface)
        {
            continue;
        }
        if (mapped[0] != mapped[1] && mapped[1] != mapped[2] && mapped[0] != mapped[2])
        {
            fromChild.insert(Canonical(mapped));
        }
        if (inQuadrant)
        {
            fromParent.insert(Canonical(plain));
        }
    }
    CHECK(fromChild.size() == 512u);
    CHECK(fromChild == fromParent);
}

TEST_CASE("sphere terrain vertex: the morph is continuous in the eye distance")
{
    const CubeSphereTileKey key = TileAt(Direction(), 12);
    const dvec3 eye = Direction() * (Radius + 3000.0);
    const SphereTerrainDrawnTile tile = Drawn(key, 1e30f, 1e30f);
    SphereTerrainDrawRecord record = Records({&tile, 1}, eye)[0];
    const auto own = [&](const ivec2 g) { return GridHeight(key, g); };
    const auto parent = [&](const ivec2 g) { return GridHeight(key.GetParent(), g); };

    // Sweep the morph window across each vertex's own distance: the window moves and the vertex
    // stays, which is the eye distance sweeping through the window.
    const std::array<ivec2, 4> grids = {ivec2(1, 0), ivec2(7, 13), ivec2(31, 31), ivec2(16, 9)};
    f64 worstRatio = 0.0;
    for (const ivec2 grid : grids)
    {
        const SphereTerrainGridVertex vertex{.Grid = grid};
        const f32 distance =
            glm::length(PlaceSphereTerrainVertex(record, vec2(grid) / Quads, own(grid)).Position);
        const f32 span = 0.25f * distance;
        record.Morph = {0.0f, 1.0f, 0.0f, 0.0f};
        const auto At = [&](const f32 offset)
        {
            // Start S = distance − offset, end S + span: the factor runs 0 → 1 as offset does
            // 0 → span.
            record.Morph.x = distance - offset;
            record.Morph.y = record.Morph.x + span;
            return EvaluateSphereTerrainVertex(record, vertex, own, parent).Placement.Position;
        };
        const vec3 start = At(0.0f);
        const vec3 end = At(span);
        // The largest slope: the whole travel over the window, with room for the height blend's
        // curvature along it.
        const f64 slope = 2.0 * glm::length(end - start) / span + 1e-6;
        constexpr u32 Steps = 400;
        const f32 step = 1.2f * span / Steps;
        vec3 previous = At(-0.1f * span);
        f64 worstJump = 0.0;
        for (u32 i = 1; i <= Steps; i++)
        {
            const vec3 now = At((-0.1f * span) + (static_cast<f32>(i) * step));
            worstJump = std::max(worstJump, static_cast<f64>(glm::length(now - previous)));
            previous = now;
        }
        worstRatio = std::max(worstRatio, worstJump / (slope * step + 1e-4));
    }
    CHECK(worstRatio <= 1.0);
}

namespace
{
    class FlatPool final : public SphereTerrainTileOracle
    {
    public:
        const SphereTerrainTileRecord* Find(const CubeSphereTileKey key) const override
        {
            m_Record.Key = key;
            return &m_Record;
        }

    private:
        mutable SphereTerrainTileRecord m_Record{
            .State = SphereTerrainTileState::Resident,
            .Bound = {.Measured = true, .Min = 0.0f, .Max = 0.0f, .MorphError = 0.0f},
        };
    };

    // The distance from a point to a polyline.
    f64 PolylineDistance(const vec3& point, const vector<vec3>& line)
    {
        f64 best = 1e30;
        for (usize i = 0; i + 1 < line.size(); i++)
        {
            const dvec3 a(line[i]);
            const dvec3 b(line[i + 1]);
            const dvec3 ab = b - a;
            const f64 t = std::clamp(glm::dot(dvec3(point) - a, ab) / glm::dot(ab, ab), 0.0, 1.0);
            best = std::min(best, glm::length(dvec3(point) - (a + (t * ab))));
        }
        return best;
    }

    // A tile's evaluated vertices along one edge, in CubeSphere::Edge order.
    vector<vec3> EdgeLine(const SphereTerrainDrawRecord& record, const u32 edge)
    {
        const auto zero = [](const ivec2) { return 0.0f; };
        vector<vec3> line;
        for (u32 k = 0; k <= SphereTerrainGridQuads; k++)
        {
            const u32 index = SphereTerrainSurfaceVertices + (edge * 33) + k;
            SphereTerrainGridVertex vertex = DecodeSphereTerrainGridVertex(index);
            vertex.Skirt = false;
            line.push_back(
                EvaluateSphereTerrainVertex(record, vertex, zero, zero).Placement.Position);
        }
        return line;
    }
}

TEST_CASE("sphere terrain vertex: drawn edges agree within the skirts that cover them")
{
    const FlatPool pool;
    u32 pairs = 0;
    u32 uncovered = 0;
    f64 worstGap = 0.0;
    for (const f64 altitude : {2.0e7, 1.0e4, 2.0})
    {
        const dvec3 eye = Direction() * (Radius + altitude);
        std::unordered_set<u64> splits;
        SphereTerrainSelectionResult selection;
        for (u32 frame = 0; frame < 4; frame++)
        {
            selection = SelectSphereTerrain({.Radius = Radius,
                                             .Eye = eye,
                                             .PixelsPerRadian = 1000.0,
                                             .Tiles = &pool,
                                             .PreviousSplits = &splits});
            splits = selection.Splits;
        }
        const vector<SphereTerrainDrawRecord> records = Records(selection.Drawn, eye);
        REQUIRE(records.size() == selection.Drawn.size());
        std::unordered_map<u64, usize> byKey;
        for (usize i = 0; i < selection.Drawn.size(); i++)
        {
            byKey[selection.Drawn[i].Key.Value] = i;
        }

        // The tiles nearest the eye: where every level meets its neighbours.
        vector<usize> order(selection.Drawn.size());
        for (usize i = 0; i < order.size(); i++)
        {
            order[i] = i;
        }
        const auto distance = [&](const usize i)
        {
            const SphereTerrainDrawnTile& t = selection.Drawn[i];
            return SphereTerrainBoxDistance(
                ComputeSphereTerrainTileBox(t.Key, Radius, t.MinHeight, t.MaxHeight), eye);
        };
        std::ranges::sort(order, {}, distance);
        order.resize(std::min<usize>(order.size(), 60));

        for (const usize a : order)
        {
            const SphereTerrainDrawnTile& tile = selection.Drawn[a];
            for (u32 edge = 0; edge < 4; edge++)
            {
                CubeSphereTileKey other =
                    tile.Key.GetNeighbour(static_cast<CubeSphere::Edge>(edge));
                while (!byKey.contains(other.Value) && other.GetLevel() > 0)
                {
                    other = other.GetParent();
                }
                if (!byKey.contains(other.Value))
                {
                    continue;
                }
                const usize b = byKey.at(other.Value);
                const vector<vec3> mine = EdgeLine(records[a], edge);
                // The neighbour's edge nearest this one's midpoint is the shared one.
                vector<vec3> theirs;
                f64 nearest = 1e30;
                for (u32 e = 0; e < 4; e++)
                {
                    vector<vec3> line = EdgeLine(records[b], e);
                    const f64 d = PolylineDistance(mine[SphereTerrainGridQuads / 2], line);
                    if (d < nearest)
                    {
                        nearest = d;
                        theirs = std::move(line);
                    }
                }
                const f64 cover = std::min(records[a].Morph.w, records[b].Morph.w);
                for (const vec3& vertex : mine)
                {
                    const f64 gap = PolylineDistance(vertex, theirs);
                    worstGap = std::max(worstGap, gap);
                    uncovered += gap <= cover ? 0u : 1u;
                }
                pairs++;
            }
        }
    }
    CHECK(pairs > 100u);
    CHECK(uncovered == 0u);
    CHECK(worstGap > 0.0);
}

TEST_CASE("sphere terrain vertex: the record's arc terms give exact corner weights at the edges")
{
    const dvec3 up = glm::normalize(dvec3(0.83, 0.31, -0.47));
    const dvec3 eye = up * (Radius + 10.0);
    u32 inexact = 0;
    for (const u32 level : {0u, 7u, 16u, 24u})
    {
        const SphereTerrainDrawnTile tile{.Key = TileAt(up, level)};
        const SphereTerrainDrawRecord record = Records({&tile, 1}, eye)[0];
        const vec4 warp = record.Tile.Warp;
        for (const f32 s : {0.0f, 1.0f})
        {
            inexact +=
                CubeSphere::TileFractionF32(s, warp.x, warp.y, {record.Arc.x, record.Arc.y}) == s
                    ? 0u
                    : 1u;
            inexact +=
                CubeSphere::TileFractionF32(s, warp.z, warp.w, {record.Arc.z, record.Arc.w}) == s
                    ? 0u
                    : 1u;
        }
        // Corner i = sx + 2 sy takes the whole weight at its own parameters.
        for (u32 corner = 0; corner < 4; corner++)
        {
            const vec2 st(static_cast<f32>(corner & 1u), static_cast<f32>(corner >> 1));
            vec4 expected(0.0f);
            expected[static_cast<i32>(corner)] = 1.0f;
            inexact += SphereTerrainCornerWeights(record, st) == expected ? 0u : 1u;
        }
    }
    CHECK(inexact == 0u);
}
