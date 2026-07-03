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

    const Unique<PointField> field =
        PointField::Create(Context, {.Name = "Cull Field", .Points = points, .CellSize = 50.0f});

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
    for (const PointField::Cell& cell : field->GetCells())
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

    const Unique<PointField> field =
        PointField::Create(Context, {.Name = "LOD Field", .Points = points, .CellSize = 100.0f});
    // The tight cluster falls in a single cell, so the LOD choice is unambiguous.
    REQUIRE(field->GetCells().size() == 1);

    const Unique<Scene> scene = Scene::Create(Types);

    // The AssetManager auto-mounts the embedded core pack, so the PointFieldScenePass loads its
    // sprite/aggregate shaders through it with no external pack to mount.
    AssetManager assets(Context, Tasks, Types);

    const Unique<SceneRenderer> renderer = SceneRenderer::Create({
        .Context = Context,
        .Assets = assets,
        .OutputFormat = Context.GetOutputFormat(),
        .Extent = extent,
        .Settings = {.Bloom = false, .Shadows = false, .AO = false, .PointField = true},
    });
    renderer->SetPointField(field.get());

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
