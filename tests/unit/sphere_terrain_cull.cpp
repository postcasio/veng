// Sphere terrain's per-view cull, device-free (BuildSphereTerrainFrame): the horizon cull never
// drops a tile with a point the eye can see; the lists a view draws do not depend on the scale
// about the eye, while a scaled terrain casts into no shadow view; a level layer draws a tile
// only where the ground dips below it; a translucent layer's tiles are in the translucent list
// alone; a record carries its tile's hold, and the tile overlay only when the frame asks for it;
// a near distance splits the main view's tiles between the two materials; and a tile's box rises to
// the layers it draws and no further.

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

#include <doctest/doctest.h>

#include <Veng/Math/CubeSphere.h>
#include <Veng/Math/Frustum.h>
#include <Veng/Scene/Camera.h>

#include <Renderer/SphereTerrainDraw.h>
#include <Renderer/SphereTerrainResidency.h>
#include <Renderer/SphereTerrainSelection.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    constexpr f64 Radius = 6.4e6;
    constexpr f32 MinHeight = -300.0f;
    constexpr f32 MaxHeight = 300.0f;

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
            .Bound = {.Measured = true, .Min = -50.0f, .Max = 50.0f, .MorphError = 0.0f},
        };
    };

    dvec3 Up()
    {
        return glm::normalize(dvec3(0.83, 0.31, -0.47));
    }

    vector<SphereTerrainDrawnTile> Select(const FlatPool& pool, const dvec3& eye)
    {
        std::unordered_set<u64> splits;
        SphereTerrainSelectionResult result;
        for (u32 frame = 0; frame < 4; frame++)
        {
            result = SelectSphereTerrain({.Radius = Radius,
                                          .MinHeight = MinHeight,
                                          .MaxHeight = MaxHeight,
                                          .Eye = eye,
                                          .PixelsPerRadian = 1000.0,
                                          .Tiles = &pool,
                                          .PreviousSplits = &splits});
            splits = result.Splits;
        }
        return result.Drawn;
    }

    CubeSphereTileKey RecordKey(const SphereTerrainDrawRecord& record)
    {
        return CubeSphereTileKey::Make(static_cast<CubeSphere::Face>(record.Tile.Tile.x & 0xFFu),
                                       (record.Tile.Tile.x >> 8) & 0xFFu, record.Tile.Tile.y,
                                       record.Tile.Tile.z);
    }

    // The tiles a view's list draws, by key.
    vector<u64> Keys(const SphereTerrainFrame& frame, const vector<u32>& list)
    {
        vector<u64> keys;
        for (const u32 index : list)
        {
            keys.push_back(RecordKey(frame.Records[index]).Value);
        }
        return keys;
    }

    vector<u64> ListKeys(const SphereTerrainFrame& frame, const usize view)
    {
        return Keys(frame, frame.Lists[view][0]);
    }

    SphereTerrainDrawView OpenView()
    {
        SphereTerrainDrawView view;
        view.Planes.fill(glm::dvec4(0.0, 0.0, 0.0, 1.0));
        return view;
    }

    // A view's planes about the eye: a camera at the eye looking along a direction.
    SphereTerrainDrawView ViewAlong(const dvec3& forward, const dvec3& up, const usize planes)
    {
        CameraView camera;
        camera.SetPerspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 1000.0f);
        camera.SetView(vec3(0.0f), vec3(forward), vec3(up));
        const Frustum frustum = Frustum::FromViewProjection(camera.ViewProjection());
        SphereTerrainDrawView view = OpenView();
        for (usize i = 0; i < planes; i++)
        {
            view.Planes[i] = glm::dvec4(dvec3(vec3(frustum.Planes[i])), frustum.Planes[i].w);
        }
        return view;
    }

    // Whether the segment from the eye to a point stays clear of the ball, by the ray's own
    // closest approach to the centre: an independent statement of what the eye can see.
    bool Sees(const dvec3& eye, const dvec3& point, const f64 radius)
    {
        const dvec3 d = point - eye;
        const f64 length = glm::length(d);
        const dvec3 u = d / length;
        const f64 along = -glm::dot(eye, u);
        if (along <= 0.0 || along >= length)
        {
            return glm::length(point) >= radius;
        }
        return glm::length(eye + (along * u)) >= radius;
    }
}

TEST_CASE("sphere terrain cull: no tile with a visible point is culled below the horizon")
{
    const FlatPool pool;
    u32 culled = 0;
    u32 wronglyCulled = 0;
    for (const f64 altitude : {2.0e7, 1.0e6, 1.0e4, 100.0, 2.0})
    {
        const dvec3 eye = Up() * (Radius + altitude);
        const vector<SphereTerrainDrawnTile> drawn = Select(pool, eye);
        SphereTerrainFrameInput input{
            .Drawn = drawn,
            .Radius = Radius,
            .MinHeight = MinHeight,
            .MaxHeight = MaxHeight,
            .Eye = eye,
            .CenterFromEye = -eye,
            .PrevCenterFromCamera = -eye,
            .Main = OpenView(),
        };
        input.Main.Horizon = true;
        const SphereTerrainFrame frame = BuildSphereTerrainFrame(input);
        culled += frame.CulledByHorizon;

        std::unordered_set<u64> kept;
        for (const SphereTerrainDrawRecord& record : frame.Records)
        {
            kept.insert(RecordKey(record).Value);
        }
        for (const SphereTerrainDrawnTile& tile : drawn)
        {
            if (kept.contains(tile.Key.Value))
            {
                continue;
            }
            const CubeSphereTileGeometry geometry = GetCubeSphereTileGeometry(tile.Key);
            bool visible = false;
            for (u32 i = 0; i <= 4 && !visible; i++)
            {
                for (u32 j = 0; j <= 4 && !visible; j++)
                {
                    const dvec3 direction = CubeSphereTileDirection(geometry, i / 4.0, j / 4.0);
                    for (const f32 height : {tile.MinHeight, tile.MaxHeight})
                    {
                        visible =
                            visible || Sees(eye, direction * (Radius + height), Radius + MinHeight);
                    }
                }
            }
            wronglyCulled += visible ? 1u : 0u;
        }
    }
    CHECK(culled > 0u);
    CHECK(wronglyCulled == 0u);
}

TEST_CASE("sphere terrain cull: a view draws the same tiles at any scale, and only scale one casts")
{
    const FlatPool pool;
    const dvec3 eye = Up() * (Radius + 50.0);
    const vector<SphereTerrainDrawnTile> drawn = Select(pool, eye);
    const dvec3 north = glm::normalize(glm::cross(Up(), dvec3(0.0, 0.0, 1.0)));
    // A shadow view looking down on the ground ahead, as a cascade over the near view would.
    const std::array<SphereTerrainDrawView, 1> shadows = {ViewAlong(-Up(), north, 6)};

    const auto Build = [&](const f32 scale)
    {
        SphereTerrainFrameInput input{
            .Drawn = drawn,
            .Radius = Radius,
            .MinHeight = MinHeight,
            .MaxHeight = MaxHeight,
            .Eye = eye,
            .CenterFromEye = -eye,
            .PrevCenterFromCamera = -eye,
            .Scale = scale,
            .Main = ViewAlong(north, Up(), 4),
            .Shadows = shadows,
        };
        input.Main.Horizon = true;
        return BuildSphereTerrainFrame(input);
    };
    const SphereTerrainFrame unscaled = Build(1.0f);
    const SphereTerrainFrame scaled = Build(1e-3f);
    REQUIRE(unscaled.Lists.size() == 2);
    REQUIRE(scaled.Lists.size() == 2);
    CHECK_FALSE(unscaled.Lists[0][0].empty());
    CHECK(ListKeys(unscaled, 0) == ListKeys(scaled, 0));
    CHECK(unscaled.CulledByFrustum == scaled.CulledByFrustum);
    CHECK_FALSE(unscaled.Lists[1][0].empty());
    CHECK(scaled.Lists[1][0].empty());
}

TEST_CASE("sphere terrain cull: a level layer draws only where the ground dips below it")
{
    const FlatPool pool;
    const dvec3 eye = Up() * (Radius + 1.0e4);
    vector<SphereTerrainDrawnTile> drawn = Select(pool, eye);
    REQUIRE(drawn.size() > 8);
    // Half the tiles hold ground wholly above the layer.
    for (usize i = 0; i < drawn.size(); i += 2)
    {
        drawn[i].MinHeight = 20.0f;
        drawn[i].MaxHeight = 60.0f;
    }
    const SphereTerrainFrameInput input{
        .Drawn = drawn,
        .Radius = Radius,
        .MinHeight = MinHeight,
        .MaxHeight = MaxHeight,
        .Eye = eye,
        .CenterFromEye = -eye,
        .PrevCenterFromCamera = -eye,
        .LayerHeights = {10.0f, -400.0f},
        .LayerCount = 2,
        .Main = OpenView(),
    };
    const SphereTerrainFrame frame = BuildSphereTerrainFrame(input);
    std::unordered_set<u64> dry;
    for (usize i = 0; i < drawn.size(); i += 2)
    {
        dry.insert(drawn[i].Key.Value);
    }
    u32 wetDrawn = 0;
    u32 dryDrawn = 0;
    for (const u32 index : frame.Lists[0][1])
    {
        (dry.contains(RecordKey(frame.Records[index]).Value) ? dryDrawn : wetDrawn)++;
    }
    CHECK(frame.Lists[0][0].size() == drawn.size());
    CHECK(wetDrawn == drawn.size() / 2);
    CHECK(dryDrawn == 0u);
    // A layer below every tile's ground draws nowhere.
    CHECK(frame.Lists[0][2].empty());
    CHECK(frame.Records[0].Heights.x == 10.0f);
}

TEST_CASE("sphere terrain cull: a translucent layer's tiles are in the translucent list alone")
{
    // Two layers at one height, one opaque and one translucent, over ground dipping below both: the
    // two draw the same tiles, the opaque one into the g-buffer and the shadow view, the translucent
    // one into the translucent pass and no shadow view.
    const FlatPool pool;
    const dvec3 eye = Up() * (Radius + 50.0);
    const vector<SphereTerrainDrawnTile> drawn = Select(pool, eye);
    const dvec3 north = glm::normalize(glm::cross(Up(), dvec3(0.0, 0.0, 1.0)));
    const std::array<SphereTerrainDrawView, 1> shadows = {ViewAlong(-Up(), north, 6)};
    SphereTerrainFrameInput input{
        .Drawn = drawn,
        .Radius = Radius,
        .MinHeight = MinHeight,
        .MaxHeight = MaxHeight,
        .Eye = eye,
        .CenterFromEye = -eye,
        .PrevCenterFromCamera = -eye,
        .LayerHeights = {10.0f, 10.0f},
        .LayerCount = 2,
        .TranslucentLayers = {false, true},
        .Main = ViewAlong(north, Up(), 4),
        .Shadows = shadows,
    };
    input.Main.Horizon = true;
    const SphereTerrainFrame frame = BuildSphereTerrainFrame(input);
    REQUIRE(frame.Lists.size() == 2);

    const vector<u64> opaque = Keys(frame, frame.Lists[0][1]);
    CHECK_FALSE(opaque.empty());
    CHECK(Keys(frame, frame.TranslucentLists[2]) == opaque);
    CHECK(frame.Lists[0][2].empty());
    CHECK_FALSE(frame.Lists[1][1].empty());
    CHECK(frame.Lists[1][2].empty());
    // The ground and the opaque layer have no translucent list.
    CHECK(frame.TranslucentLists[0].empty());
    CHECK(frame.TranslucentLists[1].empty());
    CHECK(ListKeys(frame, 0) == opaque);
}

TEST_CASE("sphere terrain cull: a record carries its hold, and the tile overlay only when asked")
{
    const FlatPool pool;
    const dvec3 eye = Up() * (Radius + 1.0e4);
    vector<SphereTerrainDrawnTile> drawn = Select(pool, eye);
    REQUIRE(drawn.size() > 8);
    std::unordered_set<u64> held;
    for (usize i = 0; i < drawn.size(); i += 3)
    {
        drawn[i].Held = true;
        held.insert(drawn[i].Key.Value);
    }
    SphereTerrainFrameInput input{
        .Drawn = drawn,
        .Radius = Radius,
        .MinHeight = MinHeight,
        .MaxHeight = MaxHeight,
        .Eye = eye,
        .CenterFromEye = -eye,
        .PrevCenterFromCamera = -eye,
        .Main = OpenView(),
    };
    CHECK_FALSE(input.TileOverlay);
    const SphereTerrainFrame off = BuildSphereTerrainFrame(input);
    input.TileOverlay = true;
    const SphereTerrainFrame on = BuildSphereTerrainFrame(input);
    REQUIRE(off.Records.size() == drawn.size());
    REQUIRE(on.Records.size() == drawn.size());

    u32 offOverlay = 0;
    u32 onOverlay = 0;
    u32 heldMismatch = 0;
    u32 otherFlagsDiffer = 0;
    for (usize i = 0; i < drawn.size(); i++)
    {
        const u32 offFlags = off.Records[i].Layers.w;
        const u32 onFlags = on.Records[i].Layers.w;
        offOverlay += (offFlags & SphereTerrainRecordTileOverlay) != 0u ? 1u : 0u;
        onOverlay += (onFlags & SphereTerrainRecordTileOverlay) != 0u ? 1u : 0u;
        const CubeSphereTileKey key = RecordKey(off.Records[i]);
        heldMismatch +=
            ((offFlags & SphereTerrainRecordHeld) != 0u) != held.contains(key.Value) ? 1u : 0u;
        otherFlagsDiffer += (offFlags ^ onFlags) != SphereTerrainRecordTileOverlay ? 1u : 0u;
    }
    CHECK(offOverlay == 0u);
    CHECK(onOverlay == drawn.size());
    CHECK(heldMismatch == 0u);
    CHECK(otherFlagsDiffer == 0u);
}

TEST_CASE("sphere terrain cull: a near distance splits the main view's tiles between two lists")
{
    const FlatPool pool;
    const dvec3 eye = Up() * (Radius + 50.0);
    const vector<SphereTerrainDrawnTile> drawn = Select(pool, eye);
    std::unordered_map<u64, const SphereTerrainDrawnTile*> byKey;
    for (const SphereTerrainDrawnTile& tile : drawn)
    {
        byKey[tile.Key.Value] = &tile;
    }
    const auto Build = [&](const f32 scale, const f32 nearDistance)
    {
        SphereTerrainFrameInput input{
            .Drawn = drawn,
            .Radius = Radius,
            .MinHeight = MinHeight,
            .MaxHeight = MaxHeight,
            .Eye = eye,
            .CenterFromEye = -eye,
            .PrevCenterFromCamera = -eye,
            .Scale = scale,
            .Main = OpenView(),
        };
        input.NearDistances[0] = nearDistance;
        return BuildSphereTerrainFrame(input);
    };
    constexpr f32 Near = 2000.0f;
    const SphereTerrainFrame whole = Build(1.0f, 0.0f);
    const SphereTerrainFrame split = Build(1.0f, Near);
    CHECK(whole.NearLists[0].empty());

    // Every tile drawn once: the two lists partition what the unsplit view draws.
    vector<u64> both = ListKeys(split, 0);
    const vector<u64> near = Keys(split, split.NearLists[0]);
    both.insert(both.end(), near.begin(), near.end());
    std::ranges::sort(both);
    vector<u64> all = ListKeys(whole, 0);
    std::ranges::sort(all);
    CHECK(both == all);
    CHECK(std::ranges::adjacent_find(both) == both.end());

    // A near tile's whole bound lies inside the distance; a far one reaches past it.
    const auto Farthest = [&](const u64 key)
    {
        const SphereTerrainDrawnTile& tile = *byKey.at(key);
        const SphereTerrainTileBox box =
            ComputeSphereTerrainTileBox(tile.Key, Radius, tile.MinHeight, tile.MaxHeight);
        f64 farthest = 0.0;
        for (const dvec3& corner : SphereTerrainBoxCorners(box))
        {
            farthest = std::max(farthest, glm::length(corner - eye));
        }
        return farthest;
    };
    u32 nearOutside = 0;
    u32 farInside = 0;
    for (const u64 key : near)
    {
        nearOutside += Farthest(key) > Near ? 1u : 0u;
    }
    for (const u64 key : ListKeys(split, 0))
    {
        farInside += Farthest(key) <= Near ? 1u : 0u;
    }
    CHECK_FALSE(near.empty());
    CHECK_FALSE(ListKeys(split, 0).empty());
    CHECK(nearOutside == 0u);
    CHECK(farInside == 0u);

    // A terrain drawn at a scale about the eye measures the distance in drawn units.
    const SphereTerrainFrame scaled = Build(0.5f, Near * 0.5f);
    CHECK(Keys(scaled, scaled.NearLists[0]) == near);
    // A surface without a distance, and the translucent pass, take no near list.
    CHECK(split.NearLists[1].empty());
    CHECK(split.NearTranslucentLists[0].empty());
}

TEST_CASE("sphere terrain cull: a drawn layer raises a tile's box and an undrawn one leaves it")
{
    const SphereTerrainDrawnTile tile{.MinHeight = 20.0f, .MaxHeight = 60.0f};
    CHECK(SphereTerrainCullHeights(tile, std::span<const f32>{}) == vec2(20.0f, 60.0f));
    // Above the ground's lowest point the layer is drawn on the tile, and the box reaches it.
    const std::array<f32, 1> above = {100.0f};
    CHECK(SphereTerrainCullHeights(tile, above) == vec2(20.0f, 100.0f));
    // Inside the ground's range it is drawn, and the box already holds it.
    const std::array<f32, 1> inside = {40.0f};
    CHECK(SphereTerrainCullHeights(tile, inside) == vec2(20.0f, 60.0f));
    // Below every height the tile holds it is not drawn, and the box does not reach down to it.
    const std::array<f32, 1> below = {-500.0f};
    CHECK(SphereTerrainCullHeights(tile, below) == vec2(20.0f, 60.0f));
}
