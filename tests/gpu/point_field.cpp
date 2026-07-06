// Point-field cases. A PointField is a large GPU-resident set of colored points drawn by the
// SceneRenderer's PointFieldScenePass, frustum-culled per spatial cell and transitioning from
// individual sprites to an aggregate density splat past a screen-density threshold. These cases
// assert the two headline behaviors on a real device: (1) frustum cull — a field split into two
// far-apart cells, viewed so only one cell is on screen, lights only that cell's side of the
// frame; and (2) the LOD switch — the same dense cell resolves into bright sprites viewed up
// close but collapses to the smoother aggregate splat viewed from far away, so the far capture's
// bright coverage is far lower (the million sprites never draw).
//
// The point field composites over the tonemapped color with no geometry, so these cases need no
// cooked material — they build the field from the public API and render an empty scene.

#include <algorithm>
#include <cmath>

#include <glm/gtc/packing.hpp>

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Math/AABB.h>
#include <Veng/Math/Frustum.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/PointField.h>
#include <Veng/Renderer/SceneRenderer.h>

#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Scene.h>

#include <gpu/fixture.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    u32 PackRgba8(u8 r, u8 g, u8 b, u8 a)
    {
        return static_cast<u32>(r) | (static_cast<u32>(g) << 8) | (static_cast<u32>(b) << 16) |
               (static_cast<u32>(a) << 24);
    }

    // Fraction of pixels in the [x0,x1)×[y0,y1) rect whose luminance exceeds a small threshold —
    // the field's emissive points read bright over the near-black empty scene.
    f32 BrightFraction(const vector<u8>& rgba16f, u32 width, u32 x0, u32 x1, u32 y0, u32 y1)
    {
        const auto* halves = reinterpret_cast<const u16*>(rgba16f.data());
        u32 bright = 0;
        u32 total = 0;
        for (u32 y = y0; y < y1; ++y)
        {
            for (u32 x = x0; x < x1; ++x)
            {
                const usize base = (static_cast<usize>(y) * width + x) * 4;
                const f32 r = glm::unpackHalf1x16(halves[base + 0]);
                const f32 g = glm::unpackHalf1x16(halves[base + 1]);
                const f32 b = glm::unpackHalf1x16(halves[base + 2]);
                if (0.2126f * r + 0.7152f * g + 0.0722f * b > 0.02f)
                {
                    ++bright;
                }
                ++total;
            }
        }
        return total == 0 ? 0.0f : static_cast<f32>(bright) / static_cast<f32>(total);
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "point field: the frustum cull contributes only in-frustum cells")
{
    // The CPU cull correctness is a pure test over the field's cells and the camera frustum: a
    // cell whose bounds miss the frustum must not intersect. Build a field in two tight clusters
    // 400 units apart on X, then confirm a camera facing only the left cluster frustum-tests every
    // left cell in and every right cell out — the exact contract the pass culls on. Each cluster
    // may span a handful of grid cells; the test asserts over whatever cells the bucketing yields.
    vector<FieldPoint> points;
    for (u32 i = 0; i < 64; ++i)
    {
        const f32 t = static_cast<f32>(i) * 0.03f;
        points.push_back({.Position = vec3(-200.0f + std::sin(t), std::cos(t), std::sin(t * 2.0f)),
                          .ColorRgba8 = PackRgba8(255, 255, 255, 255),
                          .Size = 0.5f});
        points.push_back({.Position = vec3(200.0f + std::sin(t), std::cos(t), std::sin(t * 2.0f)),
                          .ColorRgba8 = PackRgba8(255, 255, 255, 255),
                          .Size = 0.5f});
    }

    const Unique<Renderer::PointField> field = Renderer::PointField::Create(
        Context, {.Name = "Cull Field", .Points = points, .CellSize = 50.0f});

    // The clusters bucket into at least two cells (one on each side of the origin).
    REQUIRE(field->GetCells().size() >= 2);

    // A camera at the left cluster looking further left along -X: the left cluster is in front,
    // the right cluster (400 units the other way) is well behind the camera.
    CameraView camera;
    camera.SetPerspective(glm::radians(50.0f), 1.0f, 0.1f, 500.0f);
    camera.SetView(vec3(-190.0f, 0.0f, 0.0f), vec3(-260.0f, 0.0f, 0.0f), vec3(0.0f, 1.0f, 0.0f));
    const Frustum frustum = Frustum::FromViewProjection(camera.ViewProjection());

    u32 leftIn = 0;
    u32 rightIn = 0;
    for (const Renderer::PointField::Cell& cell : field->GetCells())
    {
        const bool in = Intersects(frustum, cell.Bounds);
        if (cell.Bounds.Center().x < 0.0f)
        {
            leftIn += in ? 1 : 0;
        }
        else
        {
            rightIn += in ? 1 : 0;
        }
    }

    // At least one left cell is in-frustum; no right cell (all behind the camera) is — the field
    // contributes only its in-frustum cells.
    CHECK(leftIn >= 1);
    CHECK(rightIn == 0);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "point field: a zoomed-out view uses the aggregate LOD, bounding the cost")
{
    constexpr uvec2 extent{200, 200};

    // A dense cluster of points fully inside one grid cell (centered at (50,50,50), radius ~3,
    // cell size 100 → all within the [0,100)³ cell). Up close its points resolve into many bright
    // sprites covering much of the frame; far away the whole cell projects into a few pixels whose
    // density passes the aggregate threshold, so it draws as one compact splat — far less bright
    // coverage. The two captures' bright fractions witness the LOD switch.
    const vec3 clusterCenter(50.0f, 50.0f, 50.0f);
    vector<FieldPoint> points;
    for (u32 i = 0; i < 2000; ++i)
    {
        const f32 a = static_cast<f32>(i) * 0.161f;
        const f32 b = static_cast<f32>(i) * 0.379f;
        points.push_back({.Position = clusterCenter + vec3(std::sin(a) * 3.0f, std::cos(b) * 3.0f,
                                                           std::sin(a + b) * 3.0f),
                          .ColorRgba8 = PackRgba8(255, 240, 220, 255),
                          .Size = 0.5f});
    }

    auto field = Renderer::PointField::Create(
        Context, {.Name = "LOD Field", .Points = points, .CellSize = 100.0f});
    // The tight cluster falls in a single cell, so the LOD choice is unambiguous.
    REQUIRE(field->GetCells().size() == 1);

    // The scene component is looked up by TypeId when added and walked, so register it (the empty
    // fixture registry carries no builtins).
    Types.Register<Veng::PointField>();
    const Unique<Scene> scene = Scene::Create(Types);

    // The point field is scene-authored: a PointField component carrying the built field, which the
    // renderer walks each Execute and whose presence inserts the point-field pass.
    const Entity fieldEntity = scene->CreateEntity();
    scene->Add<Veng::PointField>(fieldEntity).Field = std::move(field);

    // The AssetManager auto-mounts the embedded core pack, so the PointFieldScenePass loads its
    // sprite/aggregate shaders through it with no external pack to mount.
    AssetManager assets(Context, Tasks, Types);

    const Unique<SceneRenderer> renderer = SceneRenderer::Create({
        .Context = Context,
        .Assets = assets,
        .OutputFormat = Context.GetOutputFormat(),
        .Extent = extent,
        .Settings = {.Bloom = false, .Shadows = false, .AO = false},
    });

    auto RenderFrom = [&](vec3 eye) -> vector<u8>
    {
        CameraView camera;
        camera.SetPerspective(glm::radians(50.0f), 1.0f, 0.1f, 2000.0f);
        camera.SetView(eye, clusterCenter, vec3(0.0f, 1.0f, 0.0f));

        Context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                renderer->Execute(
                    cmd, Renderer::SceneView{.World = *scene, .Camera = camera, .Delta = 0.0f});
            });
        vector<u8> pixels = renderer->GetOutput()->GetImage()->Download();
        REQUIRE(pixels.size() == static_cast<size_t>(extent.x) * extent.y * 8);
        return pixels;
    };

    const vector<u8> near = RenderFrom(clusterCenter + vec3(0.0f, 0.0f, 12.0f));
    const vector<u8> far = RenderFrom(clusterCenter + vec3(0.0f, 0.0f, 800.0f));

    const f32 nearBright = BrightFraction(near, extent.x, 0, extent.x, 0, extent.y);
    const f32 farBright = BrightFraction(far, extent.x, 0, extent.x, 0, extent.y);

    // Up close, the resolved sprites cover a meaningful chunk of the frame.
    CHECK(nearBright > 0.05f);
    // Zoomed out, the cell collapses to the compact aggregate splat: far less coverage than the
    // resolved sprites, proving the sprite→aggregate transition kept the cost bounded.
    CHECK(farBright < nearBright * 0.5f);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "point field: component presence drives the pass and multiple fields all draw")
{
    // Component presence, not a settings toggle, inserts and removes the point-field pass, and the
    // renderer walks every PointField component: a scene with two field entities (a bright cluster
    // on the left and one on the right of the frame) lights both sides; deleting a field's
    // component removes it next frame with no consumer call and no teardown ordering.
    constexpr uvec2 extent{200, 200};

    auto MakeCluster = [this](vec3 center) -> Unique<Renderer::PointField>
    {
        vector<FieldPoint> points;
        for (u32 i = 0; i < 2000; ++i)
        {
            const f32 a = static_cast<f32>(i) * 0.161f;
            const f32 b = static_cast<f32>(i) * 0.379f;
            points.push_back({.Position = center + vec3(std::sin(a) * 2.0f, std::cos(b) * 2.0f,
                                                        std::sin(a + b) * 2.0f),
                              .ColorRgba8 = PackRgba8(255, 240, 220, 255),
                              .Size = 0.5f});
        }
        // The cell size hugs the cluster: an aggregate splat's kernel spans a full cell edge
        // anchored at the cell's point centroid, so a loose cell would draw a frame-scale
        // kernel whose core crosses the frame midline the halves assertions split on.
        return Renderer::PointField::Create(
            Context, {.Name = "Cluster", .Points = points, .CellSize = 4.0f});
    };

    const vec3 leftCenter(-6.0f, 0.0f, 0.0f);
    const vec3 rightCenter(6.0f, 0.0f, 0.0f);

    // The scene component is looked up by TypeId when added and walked, so register it.
    Types.Register<Veng::PointField>();
    const Unique<Scene> scene = Scene::Create(Types);
    AssetManager assets(Context, Tasks, Types);

    // No PointField component authored yet: the presence-driven pass is absent, and Execute still
    // produces a valid output whose GetOutput() ref survives the later presence recompile.
    const Unique<SceneRenderer> renderer = SceneRenderer::Create({
        .Context = Context,
        .Assets = assets,
        .OutputFormat = Context.GetOutputFormat(),
        .Extent = extent,
        .Settings = {.Bloom = false, .Shadows = false, .AO = false},
    });
    const Ref<Renderer::ImageView> output = renderer->GetOutput();

    CameraView camera;
    camera.SetPerspective(glm::radians(50.0f), 1.0f, 0.1f, 2000.0f);
    camera.SetView(vec3(0.0f, 0.0f, 40.0f), vec3(0.0f, 0.0f, 0.0f), vec3(0.0f, 1.0f, 0.0f));

    auto Render = [&]() -> vector<u8>
    {
        Context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                renderer->Execute(
                    cmd, Renderer::SceneView{.World = *scene, .Camera = camera, .Delta = 0.0f});
            });
        return renderer->GetOutput()->GetImage()->Download();
    };

    // Empty scene: neither half of the frame lights (the pass is not even inserted).
    {
        const vector<u8> pixels = Render();
        CHECK(BrightFraction(pixels, extent.x, 0, extent.x / 2, 0, extent.y) < 0.01f);
        CHECK(BrightFraction(pixels, extent.x, extent.x / 2, extent.x, 0, extent.y) < 0.01f);
    }

    // Add two field entities: presence inserts the pass (an internal recompile that reuses the
    // imported output, so the cached GetOutput() ref stays the same image) and both sides light.
    const Entity leftEntity = scene->CreateEntity();
    scene->Add<Veng::PointField>(leftEntity).Field = MakeCluster(leftCenter);
    const Entity rightEntity = scene->CreateEntity();
    scene->Add<Veng::PointField>(rightEntity).Field = MakeCluster(rightCenter);
    {
        const vector<u8> pixels = Render();
        CHECK(BrightFraction(pixels, extent.x, 0, extent.x / 2, 0, extent.y) > 0.02f);
        CHECK(BrightFraction(pixels, extent.x, extent.x / 2, extent.x, 0, extent.y) > 0.02f);
    }
    // The presence recompile preserved the output's identity (only Resize/Configure recreate it).
    CHECK(renderer->GetOutput() == output);

    // Drop the right field's component: the renderer walks one field next frame with no teardown
    // ordering (the field retires with its component), so only the left half lights.
    scene->Remove<Veng::PointField>(rightEntity);
    {
        const vector<u8> pixels = Render();
        CHECK(BrightFraction(pixels, extent.x, 0, extent.x / 2, 0, extent.y) > 0.02f);
        CHECK(BrightFraction(pixels, extent.x, extent.x / 2, extent.x, 0, extent.y) < 0.01f);
    }

    // Drop the last field too: presence removes the pass again and the frame goes dark, cleanly.
    scene->Remove<Veng::PointField>(leftEntity);
    {
        const vector<u8> pixels = Render();
        CHECK(BrightFraction(pixels, extent.x, 0, extent.x, 0, extent.y) < 0.01f);
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "point field: a wide many-cell view records the pass's draw statistics")
{
    // The stress shape the point-field pass is built to bound: a wide, shallow field bucketed into
    // hundreds of small cells, viewed whole so every cell is in-frustum at once. It pins two things
    // — that the wide view still reads bright where the field is and dark where it is not, and the
    // pass's per-frame counter shape today: with aggregation switched fully off, every in-frustum
    // cell resolves into its own sprite draw (ResolvedDraws == CellsInFrustum) and every in-frustum
    // cell is measured (CellsMeasured == CellsInFrustum); with it switched fully on, every
    // in-frustum cell collapses to one splat (Splats == CellsInFrustum, no resolved draw). These
    // counters are the baseline later draw-pipeline work is verified against.
    constexpr uvec2 extent{600, 150};

    // A grid of bright points filling a wide slab at z = 0: 151 columns 2 units apart on X and 91
    // rows 2 units apart on Y, sized so its projection fills the wide target vertically and spans
    // it horizontally. A small cull cell (4 units) buckets it into thousands of cells, each holding
    // a handful of points — the many-cells-in-frustum shape under test.
    vector<FieldPoint> points;
    for (i32 xi = -75; xi <= 75; ++xi)
    {
        for (i32 yi = -45; yi <= 45; ++yi)
        {
            points.push_back(
                {.Position = vec3(static_cast<f32>(xi) * 2.0f, static_cast<f32>(yi) * 2.0f, 0.0f),
                 .ColorRgba8 = PackRgba8(255, 255, 255, 255),
                 .Size = 2.0f});
        }
    }

    auto field = Renderer::PointField::Create(
        Context, {.Name = "Stress Field", .Points = points, .CellSize = 4.0f});
    // Hundreds of cells is the shape under test — the whole point of the wide, small-cell field.
    REQUIRE(field->GetCells().size() > 100);

    Types.Register<Veng::PointField>();
    const Unique<Scene> scene = Scene::Create(Types);
    const Entity fieldEntity = scene->CreateEntity();
    auto& component = scene->Add<Veng::PointField>(fieldEntity);
    component.Field = std::move(field);

    AssetManager assets(Context, Tasks, Types);

    const Unique<SceneRenderer> renderer = SceneRenderer::Create({
        .Context = Context,
        .Assets = assets,
        .OutputFormat = Context.GetOutputFormat(),
        .Extent = extent,
        .Settings = {.Bloom = false, .Shadows = false, .AO = false},
    });

    // A camera looking down -Z at the slab from far enough that the whole X span is in view (the
    // wide 4:1 target gives the horizontal reach); every cell is in-frustum and in front of the eye.
    CameraView camera;
    camera.SetPerspective(glm::radians(40.0f), static_cast<f32>(extent.x) / extent.y, 0.1f,
                          2000.0f);
    camera.SetView(vec3(0.0f, 0.0f, 250.0f), vec3(0.0f, 0.0f, 0.0f), vec3(0.0f, 1.0f, 0.0f));

    auto Render = [&]() -> vector<u8>
    {
        Context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                renderer->Execute(
                    cmd, Renderer::SceneView{.World = *scene, .Camera = camera, .Delta = 0.0f});
            });
        return renderer->GetOutput()->GetImage()->Download();
    };

    // Resolved case: an aggregate threshold far above any cell's on-screen density pins every cell
    // to the sprite path, so the whole field resolves.
    {
        component.Lod = Renderer::PointFieldLod{.AggregateThreshold = 1.0e6f};
        const vector<u8> pixels = Render();

        // Bright in the central band the field's sprites fill; dark in the left strip beyond its
        // horizontal reach.
        CHECK(BrightFraction(pixels, extent.x, extent.x * 2 / 5, extent.x * 3 / 5, extent.y * 2 / 5,
                             extent.y * 3 / 5) > 0.05f);
        CHECK(BrightFraction(pixels, extent.x, 0, extent.x / 6, 0, extent.y) < 0.01f);

        const Renderer::PointFieldStats stats = renderer->GetPointFieldStats();
        // One field walked, hundreds of cells in-frustum, all resolved as sprites. The threshold is
        // unreachable, so the never-aggregate fast path skips the density measure entirely
        // (CellsMeasured == 0) and the resolved cells merge into a handful of contiguous runs — the
        // draw count collapses far below the cell count instead of one draw per cell.
        CHECK(stats.Fields == 1);
        CHECK(stats.CellsInFrustum > 100);
        CHECK(stats.CellsMeasured == 0);
        CHECK(stats.ResolvedDraws > 0);
        CHECK(stats.ResolvedDraws * 20 < stats.CellsInFrustum);
        CHECK(stats.SpritePoints == static_cast<u64>(points.size()));
        CHECK(stats.Splats == 0);
        MESSAGE("never-aggregate: CellsInFrustum=", stats.CellsInFrustum,
                " ResolvedDraws=", stats.ResolvedDraws, " CellsMeasured=", stats.CellsMeasured,
                " SpritePoints=", stats.SpritePoints);
    }

    // Inverse case: a zero threshold aggregates every cell, so nothing resolves and each in-frustum
    // cell draws one splat.
    {
        component.Lod = Renderer::PointFieldLod{.AggregateThreshold = 0.0f};
        const vector<u8> pixels = Render();

        // The aggregate splats still light the central band the field fills.
        CHECK(BrightFraction(pixels, extent.x, extent.x * 2 / 5, extent.x * 3 / 5, extent.y * 2 / 5,
                             extent.y * 3 / 5) > 0.05f);

        const Renderer::PointFieldStats stats = renderer->GetPointFieldStats();
        // Zero threshold aggregates every cell: nothing resolves, and each in-frustum cell draws one
        // splat. The always-aggregate path still measures the footprint — the splat sizing reads its
        // Pixels term — so CellsMeasured tracks CellsInFrustum here.
        CHECK(stats.Fields == 1);
        CHECK(stats.CellsInFrustum > 100);
        CHECK(stats.CellsMeasured == stats.CellsInFrustum);
        CHECK(stats.ResolvedDraws == 0);
        CHECK(stats.Splats == stats.CellsInFrustum);
        MESSAGE("always-aggregate: CellsInFrustum=", stats.CellsInFrustum,
                " CellsMeasured=", stats.CellsMeasured, " Splats=", stats.Splats);
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "point field: DepthFade off matches on over an empty scene")
{
    // The depth-fade knob picks between two fragment permutations: the default (on) samples the
    // g-buffer depth and dims an occluded sprite; the trimmed (off) drops that sample entirely.
    // Over an empty scene nothing occludes, so the fade can never trigger and the two permutations
    // must produce a pixel-identical image — the field a consumer composites over background gets
    // the same picture whether or not it pays the per-fragment depth sample.
    constexpr uvec2 extent{200, 200};

    const vec3 clusterCenter(50.0f, 50.0f, 50.0f);
    vector<FieldPoint> points;
    for (u32 i = 0; i < 2000; ++i)
    {
        const f32 a = static_cast<f32>(i) * 0.161f;
        const f32 b = static_cast<f32>(i) * 0.379f;
        points.push_back({.Position = clusterCenter + vec3(std::sin(a) * 3.0f, std::cos(b) * 3.0f,
                                                           std::sin(a + b) * 3.0f),
                          .ColorRgba8 = PackRgba8(255, 240, 220, 255),
                          .Size = 0.5f});
    }

    auto field = Renderer::PointField::Create(
        Context, {.Name = "Fade Field", .Points = points, .CellSize = 100.0f});
    REQUIRE(field->GetCells().size() == 1);

    Types.Register<Veng::PointField>();
    const Unique<Scene> scene = Scene::Create(Types);
    const Entity fieldEntity = scene->CreateEntity();
    auto& component = scene->Add<Veng::PointField>(fieldEntity);
    // Never aggregate: every visible cell resolves into sprites — the fragment path under test.
    component.Lod = Renderer::PointFieldLod{.AggregateThreshold = 1.0e6f};
    component.Field = std::move(field);

    AssetManager assets(Context, Tasks, Types);
    const Unique<SceneRenderer> renderer = SceneRenderer::Create({
        .Context = Context,
        .Assets = assets,
        .OutputFormat = Context.GetOutputFormat(),
        .Extent = extent,
        .Settings = {.Bloom = false, .Shadows = false, .AO = false},
    });

    CameraView camera;
    camera.SetPerspective(glm::radians(50.0f), 1.0f, 0.1f, 2000.0f);
    camera.SetView(clusterCenter + vec3(0.0f, 0.0f, 12.0f), clusterCenter, vec3(0.0f, 1.0f, 0.0f));

    auto Render = [&]() -> vector<u8>
    {
        Context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                renderer->Execute(
                    cmd, Renderer::SceneView{.World = *scene, .Camera = camera, .Delta = 0.0f});
            });
        return renderer->GetOutput()->GetImage()->Download();
    };

    component.Lod.DepthFade = true;
    const vector<u8> onImage = Render();
    component.Lod.DepthFade = false;
    const vector<u8> offImage = Render();

    const f32 onBright = BrightFraction(onImage, extent.x, 0, extent.x, 0, extent.y);
    const f32 offBright = BrightFraction(offImage, extent.x, 0, extent.x, 0, extent.y);

    // The field lights the frame under both permutations, and — nothing occluding, the fade can
    // never trigger over the cleared far depth — with identical brightness (the trimmed permutation
    // that skips the depth sample and the depth-fade one whose compare always fails agree; the two
    // differ only in sub-f16-ULP fragment codegen at sprite edges, below the brightness threshold).
    CHECK(onBright > 0.05f);
    CHECK(offBright == onBright);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "point field: the compute sprite path matches the direct path and compacts")
{
    // The compute expansion path runs the per-point sprite work once and compacts out
    // zero-contribution points before they raster, drawing the survivors through one indirect draw.
    // Two things are pinned: (1) for a field of visible points it produces a brightness-equivalent
    // image to the direct path (the A/B reference forced via SetPointFieldForceDirect); (2) for a
    // field placed wholly behind the eye it compacts every point out (survivors == 0), where the
    // direct path draws each behind-eye point as a harmless degenerate quad. On a device without the
    // compute path both arms draw direct, so the comparison is trivial and compaction soft-passes.
    constexpr uvec2 extent{256, 256};

    // A grid of resolvable points on a plane in front of the camera, all in one cull cell (CellSize
    // covers the whole span). Placed at positive coordinates so the single-cell bucketing is
    // unambiguous, and sized generously so each projects to several pixels (a robust brightness).
    auto MakeGrid = [](f32 zPlane) -> vector<FieldPoint>
    {
        vector<FieldPoint> points;
        for (i32 xi = -20; xi <= 20; ++xi)
        {
            for (i32 yi = -20; yi <= 20; ++yi)
            {
                points.push_back({.Position = vec3(100.0f + static_cast<f32>(xi) * 2.0f,
                                                   100.0f + static_cast<f32>(yi) * 2.0f, zPlane),
                                  .ColorRgba8 = PackRgba8(255, 255, 255, 255),
                                  .Size = 2.0f});
            }
        }
        return points;
    };
    const vector<FieldPoint> points = MakeGrid(0.0f); // a plane at z=0
    const u32 totalPoints = static_cast<u32>(points.size());

    auto field = Renderer::PointField::Create(
        Context, {.Name = "AB Field", .Points = points, .CellSize = 4000.0f});
    REQUIRE(field->GetCells().size() == 1);

    Types.Register<Veng::PointField>();
    const Unique<Scene> scene = Scene::Create(Types);
    const Entity fieldEntity = scene->CreateEntity();
    auto& component = scene->Add<Veng::PointField>(fieldEntity);
    // Never aggregate: every visible cell resolves into sprites (the compute path's subject).
    component.Lod = Renderer::PointFieldLod{.AggregateThreshold = 1.0e6f};
    component.Field = std::move(field);

    AssetManager assets(Context, Tasks, Types);
    const Unique<SceneRenderer> renderer = SceneRenderer::Create({
        .Context = Context,
        .Assets = assets,
        .OutputFormat = Context.GetOutputFormat(),
        .Extent = extent,
        .Settings = {.Bloom = false, .Shadows = false, .AO = false},
    });

    // The camera sits at (100, 0, 100) looking toward the plane at z=0.
    CameraView frontCamera;
    frontCamera.SetPerspective(glm::radians(60.0f), 1.0f, 0.1f, 2000.0f);
    frontCamera.SetView(vec3(100.0f, 100.0f, 100.0f), vec3(100.0f, 100.0f, 0.0f),
                        vec3(0.0f, 1.0f, 0.0f));

    auto Render = [&](const CameraView& cam) -> vector<u8>
    {
        Context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                renderer->Execute(
                    cmd, Renderer::SceneView{.World = *scene, .Camera = cam, .Delta = 0.0f});
            });
        return renderer->GetOutput()->GetImage()->Download();
    };

    // --- Brightness A/B on the visible field ---
    // Compute path (automatic). Render twice so the one-frame-late CompactedPoints readback settles.
    renderer->SetPointFieldForceDirect(false);
    Render(frontCamera);
    const vector<u8> computeImage = Render(frontCamera);
    const Renderer::PointFieldStats computeStats = renderer->GetPointFieldStats();

    // Forced-direct reference.
    renderer->SetPointFieldForceDirect(true);
    Render(frontCamera);
    const vector<u8> directImage = Render(frontCamera);
    const Renderer::PointFieldStats directStats = renderer->GetPointFieldStats();

    const f32 computeBright = BrightFraction(computeImage, extent.x, 0, extent.x, 0, extent.y);
    const f32 directBright = BrightFraction(directImage, extent.x, 0, extent.x, 0, extent.y);

    // The direct reference lights the cluster and reports its honest path + submitted count.
    CHECK(directBright > 0.02f);
    CHECK(directStats.DrawSource == Renderer::SpriteDrawSource::Direct);
    CHECK(directStats.SpritePoints == totalPoints);
    CHECK(directStats.CompactedPoints == 0);

    if (computeStats.DrawSource == Renderer::SpriteDrawSource::Compute)
    {
        // Same pre-compaction total submitted (nothing aggregated); brightness matches the direct
        // reference within a coarse tolerance (well above the record's f16 quantization). No point
        // of a wholly-visible field compacts, so survivors == submitted here.
        CHECK(computeStats.SpritePoints == totalPoints);
        CHECK(computeBright > 0.02f);
        CHECK(std::abs(computeBright - directBright) < directBright * 0.35f);
        CHECK(computeStats.CompactedPoints <= computeStats.SpritePoints);
        MESSAGE("compute A/B (visible): submitted=", computeStats.SpritePoints,
                " compacted=", computeStats.CompactedPoints, " computeBright=", computeBright,
                " directBright=", directBright);

        // --- Compaction of behind-eye points that reach the dispatch ---
        // Replace the field with one whose single cull cell straddles the eye: a line of points from
        // in front of the camera (z between camera and 0) to well behind it (z past the camera),
        // packed in one huge cell so the cell survives the frustum cull and its whole range is
        // submitted. The compute pass then compacts the behind-eye half per point (clipCenter.w <=
        // 0), so survivors < submitted; the direct path draws them all (the behind ones degenerate).
        vector<FieldPoint> straddle;
        u32 behindCount = 0;
        for (i32 zi = -30; zi <= 30; ++zi)
        {
            // Camera sits at z=100; z below 100 is in front, above is behind. Span z in [20, 380].
            const f32 z = 200.0f + static_cast<f32>(zi) * 6.0f;
            if (z > 100.0f)
            {
                ++behindCount;
            }
            straddle.push_back({.Position = vec3(100.0f, 100.0f, z),
                                .ColorRgba8 = PackRgba8(255, 255, 255, 255),
                                .Size = 2.0f});
        }
        const u32 straddleTotal = static_cast<u32>(straddle.size());
        auto straddleField = Renderer::PointField::Create(
            Context, {.Name = "Straddle", .Points = straddle, .CellSize = 4000.0f});
        REQUIRE(straddleField->GetCells().size() == 1);
        auto& sc = scene->Get<Veng::PointField>(fieldEntity);
        sc.Field = std::move(straddleField);
        sc.Lod = Renderer::PointFieldLod{.AggregateThreshold = 1.0e6f};

        renderer->SetPointFieldForceDirect(false);
        Render(frontCamera);
        Render(frontCamera);
        const Renderer::PointFieldStats straddleStats = renderer->GetPointFieldStats();

        // The whole cell was submitted (front + behind), and the behind-eye points compacted out:
        // survivors <= submitted, and at least the behind-eye set was compacted.
        CHECK(straddleStats.SpritePoints == straddleTotal);
        const u64 survivors = straddleStats.SpritePoints - straddleStats.CompactedPoints;
        CHECK(survivors <= straddleStats.SpritePoints);
        CHECK(straddleStats.CompactedPoints >= behindCount);
        MESSAGE("compute compaction (straddle): submitted=", straddleStats.SpritePoints,
                " compacted=", straddleStats.CompactedPoints, " survivors=", survivors,
                " behindCount=", behindCount);
    }
    else
    {
        // No compute path on this device: both arms drew direct, so the comparison is trivial and
        // there is nothing compute-compacted to assert.
        CHECK(computeStats.DrawSource == Renderer::SpriteDrawSource::Direct);
        CHECK(computeStats.CompactedPoints == 0);
        MESSAGE("compute path unsupported on this device; direct-only A/B");
    }
}
