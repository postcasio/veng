// Primitives::ProjectionShell, end to end: the illusion, on the GPU. One pattern document is
// rendered two ways and the two captures are compared *geometrically*, which is the only comparison
// the two paths admit:
//
//  - as a screen-space overlay — a GuiScenePass draw list recorded straight into the screen
//    sub-rectangle the shell is meant to cover, composited over the scene image (GetOutput);
//  - on the shell — a GuiSurface drives the same pattern into its premultiplied-alpha HDR
//    RenderTarget, a Translucent panel material un-premultiplies it (rgb / a) and returns it as the
//    surface's radiance, and the whole thing travels through the translucent scene pass on the
//    generated mesh, viewed from the reference pose.
//
// **Reconciling the two capture surfaces.** They differ in three ways and all three are neutralized
// rather than tolerated. The scene both composite over is cleared to *black*, so the overlay's
// premultiplied-over blend and the shell material's straight-alpha blend both reduce to the same
// premultiplied value — the alpha divide in the fragment cancels the multiply in the blend. The
// viewport's tonemapper is set to None at exposure 1, so the shell's trip through the post chain is
// the identity on values in [0,1] (the pattern is authored below 1 and bloom is off, so nothing
// clips or spreads). And the document's canvas is sized so one logical point is one screen pixel
// inside the rect, which puts every document texel centre exactly on a screen pixel centre — the
// shell's bilinear tap is then the identity too, and what is left to measure is geometry alone.
//
// The measurement is sub-pixel edge localization, not a pixel diff: the pattern is hard-edged bars,
// each capture is normalized against its own two levels, and the 50%-coverage crossings are located
// by linear interpolation along scanlines and columns chosen to miss the crossing bars. The
// threshold is Primitives::ProjectionShellReprojectionBound at this exact configuration — the
// derived number, not a tuned constant.
//
// The offset capture is the control. It translates the eye — laterally *and* forward, never a
// rotation: a rotation about the eye is degenerate for a cap centred on it (it maps to a rigid
// in-plane rotation of the document, which a screen-space cheat reproduces), so only a translation
// shows the parallax that proves the shell is geometry.

#include <doctest/doctest.h>

#include <array>
#include <cmath>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Primitives.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Gui/Document.h>
#include <Veng/Gui/DrawList.h>
#include <Veng/Gui/Element.h>
#include <Veng/Gui/RenderTarget.h>
#include <Veng/Gui/Style.h>
#include <Veng/Gui/Surface.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/Viewport.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

#include <Renderer/Passes/GuiScenePass.h>

#include <gpu/fixture.h>
#include "support/TempPath.h"

// After the Veng headers, so Veng.h's GLM_FORCE_DEPTH_ZERO_TO_ONE is set before glm.
#include <glm/gtc/packing.hpp>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    // The shape inputs, once. The rect is the centered half of each axis and the canvas matches its
    // screen size in points, so a document point is a screen pixel and the shell's texture tap is
    // the identity — see the reconciliation note at the top.
    constexpr uvec2 Extent{640, 480};
    constexpr uvec2 Canvas{320, 240};
    constexpr vec2 RectCenter{0.5f, 0.5f};
    constexpr vec2 RectSize{0.5f, 0.5f};
    constexpr uvec2 Subdivisions{32, 32};
    constexpr f32 ShellRadius = 2.0f;
    const f32 FovY = glm::radians(60.0f);
    constexpr f32 Aspect = static_cast<f32>(Extent.x) / static_cast<f32>(Extent.y);

    // The eye displacement the control capture uses: 2% of the shell's radius sideways and 1.5% of it
    // forward. Both matter — the lateral component slides the cap across the view, the forward one
    // changes its subtended size — and neither is expressible as a rotation about the eye. It is
    // deliberately small enough that every sampled scanline still crosses the same bars, so the
    // control reports a *measured* displacement rather than degenerating into "the pattern moved
    // somewhere else entirely".
    constexpr vec3 OffsetEye{0.04f, 0.0f, -0.03f};

    // How many times the reprojection threshold the offset capture must exceed. A bare "must differ"
    // passes on a single pixel of noise; the displacement a translated eye actually produces here is
    // tens of pixels.
    constexpr f32 OffsetMultiple = 50.0f;

    // The pattern: four full-height bars and four full-width bars, hard-edged and opaque, on a
    // transparent field. Their edges are what the comparison localizes, and they are spread across
    // the rect so the corners — where the reprojection error is largest — are sampled too.
    constexpr std::array<f32, 4> BarFractions{0.125f, 0.375f, 0.625f, 0.875f};
    constexpr f32 BarThickness = 12.0f;

    // Below 1.0 on every channel: nothing clips through the None tonemapper's saturate and nothing
    // crosses a bloom threshold (bloom is off regardless).
    constexpr vec4 BarColor{0.72f, 0.80f, 0.88f, 1.0f};

    // The rect's screen rectangle, in pixels.
    vec2 RectScreenMin()
    {
        return (RectCenter - RectSize * 0.5f) * vec2(Extent);
    }
    vec2 RectScreenSize()
    {
        return RectSize * vec2(Extent);
    }

    // Builds the pattern as a document laid out against `available`, with the bars placed absolutely
    // inside a `size` region at `origin`. The overlay instance takes the whole screen and the rect's
    // offset; the surface instance takes the canvas and a zero offset — one authoring path, so the
    // two cannot drift apart.
    Unique<Gui::Document> BuildPatternDocument(const vec2 origin, const vec2 size)
    {
        auto document = CreateUnique<Gui::Document>();

        Gui::Style root;
        root.Background = vec4(0.0f);
        document->SetStyle(document->Root(), root);

        const auto addBar = [&](const vec2 barOrigin, const vec2 barSize)
        {
            Gui::Element& bar = document->Add(document->Root(), Gui::ElementKind::Panel);
            Gui::Style style;
            style.Position = Gui::PositionType::Absolute;
            style.Inset.Left = barOrigin.x;
            style.Inset.Top = barOrigin.y;
            style.Width = Gui::Length::Points(barSize.x);
            style.Height = Gui::Length::Points(barSize.y);
            style.Background = BarColor;
            document->SetStyle(bar, style);
        };

        for (const f32 fraction : BarFractions)
        {
            const f32 centerX = size.x * fraction;
            addBar(origin + vec2(centerX - BarThickness * 0.5f, 0.0f), vec2(BarThickness, size.y));
        }
        for (const f32 fraction : BarFractions)
        {
            const f32 centerY = size.y * fraction;
            addBar(origin + vec2(0.0f, centerY - BarThickness * 0.5f), vec2(size.x, BarThickness));
        }

        return document;
    }

    // The luminance field of an RGBA16Sfloat download, one f32 per pixel.
    vector<f32> DecodeLuminance(const vector<u8>& rgba16f, const uvec2 extent)
    {
        const auto* halves = reinterpret_cast<const u16*>(rgba16f.data());
        vector<f32> out;
        out.reserve(static_cast<usize>(extent.x) * extent.y);
        for (u32 pixel = 0; pixel < extent.x * extent.y; ++pixel)
        {
            const f32 r = glm::unpackHalf1x16(halves[pixel * 4 + 0]);
            const f32 g = glm::unpackHalf1x16(halves[pixel * 4 + 1]);
            const f32 b = glm::unpackHalf1x16(halves[pixel * 4 + 2]);
            out.push_back(r * 0.2126f + g * 0.7152f + b * 0.0722f);
        }
        return out;
    }

    // The sub-pixel positions where a sampled profile crosses the midpoint between its own two
    // levels. Normalizing against the profile's own min and max is what makes captures that carry
    // different absolute intensities comparable: a bar edge is where coverage is half, whatever the
    // level either side of it happens to be.
    vector<f32> Crossings(const vector<f32>& profile)
    {
        vector<f32> out;
        const auto [low, high] = std::ranges::minmax_element(profile);
        // A profile with no contrast has no edges — the caller's count check reports that.
        if (*high - *low < 0.05f)
        {
            return out;
        }
        const f32 level = (*low + *high) * 0.5f;
        for (usize i = 0; i + 1 < profile.size(); ++i)
        {
            const bool rising = profile[i] < level && profile[i + 1] >= level;
            const bool falling = profile[i] >= level && profile[i + 1] < level;
            if (rising || falling)
            {
                const f32 span = profile[i + 1] - profile[i];
                out.push_back(static_cast<f32>(i) + (level - profile[i]) / span);
            }
        }
        return out;
    }

    // One image row, as a profile.
    vector<f32> Row(const vector<f32>& luminance, const uvec2 extent, const u32 y)
    {
        vector<f32> out;
        out.reserve(extent.x);
        for (u32 x = 0; x < extent.x; ++x)
        {
            out.push_back(luminance[static_cast<usize>(y) * extent.x + x]);
        }
        return out;
    }

    // One image column, as a profile.
    vector<f32> Column(const vector<f32>& luminance, const uvec2 extent, const u32 x)
    {
        vector<f32> out;
        out.reserve(extent.y);
        for (u32 y = 0; y < extent.y; ++y)
        {
            out.push_back(luminance[static_cast<usize>(y) * extent.x + x]);
        }
        return out;
    }

    // The scanlines and columns the crossings are read along: document coordinates chosen to fall in
    // the gaps between the crossing bars, so a row sees exactly the vertical bars' eight edges and a
    // column exactly the horizontal bars'.
    constexpr std::array<f32, 3> ClearDocumentX{80.0f, 160.0f, 240.0f};
    constexpr std::array<f32, 3> ClearDocumentY{60.0f, 120.0f, 180.0f};

    // The worst displacement between two captures' edge positions, over every sampled scanline and
    // column. A scanline whose two captures disagree on how many edges exist is not a small
    // displacement, so it reports the frame width instead of being skipped.
    f32 WorstEdgeDisplacement(const vector<f32>& reference, const vector<f32>& candidate)
    {
        f32 worst = 0.0f;
        const auto compare = [&](const vector<f32>& a, const vector<f32>& b, const f32 fallback)
        {
            if (a.size() != b.size() || a.empty())
            {
                worst = glm::max(worst, fallback);
                return;
            }
            for (usize i = 0; i < a.size(); ++i)
            {
                worst = glm::max(worst, std::abs(a[i] - b[i]));
            }
        };

        const vec2 rectMin = RectScreenMin();
        for (const f32 documentY : ClearDocumentY)
        {
            const u32 y = static_cast<u32>(rectMin.y + documentY);
            compare(Crossings(Row(reference, Extent, y)), Crossings(Row(candidate, Extent, y)),
                    static_cast<f32>(Extent.x));
        }
        for (const f32 documentX : ClearDocumentX)
        {
            const u32 x = static_cast<u32>(rectMin.x + documentX);
            compare(Crossings(Column(reference, Extent, x)),
                    Crossings(Column(candidate, Extent, x)), static_cast<f32>(Extent.y));
        }
        return worst;
    }

    // Clears an image to a solid color through a one-pass graph — the stand-in scene output the
    // overlay composites over.
    void ClearImage(Context& context, const Ref<ImageView>& view, const ClearColor& clear)
    {
        context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                RenderGraph graph(context);
                const ResourceId target = graph.Import("Scene Clear");
                graph.AddPass("clear")
                    .Color({.Resource = target,
                            .Load = LoadOp::Clear,
                            .Store = StoreOp::Store,
                            .Clear = clear})
                    .Execute([](PassContext&) {});
                const RenderGraph::ImportBinding binding{.Id = target, .View = view};
                graph.Compile()->Execute(cmd, {&binding, 1});
            });
    }

    // Cooks the gui_surface fixture pack in-process for its Translucent panel material; its shaders
    // `#include "Veng/..."`, so the engine core shader dir is threaded onto the Slang search path.
    path CookPanelPack()
    {
        const path fixtureDir = path(GPU_GBUFFER_FIXTURE_DIR);
        const path outArchive = Veng::TestSupport::TempDir() / "veng_projection_shell.vengpack";

        Cook::Cooker cooker;
        Cook::RegisterBuiltinImporters(cooker);
        const VoidResult cooked =
            cooker.CookPack(fixtureDir / "gui_surface_pack.json", outArchive, {}, nullptr, nullptr,
                            nullptr, nullptr, {}, path(VENG_CORE_SHADER_DIR));
        REQUIRE_MESSAGE(cooked.has_value(), cooked.error());
        return outArchive;
    }

    // The gui_surface fixture pack's Translucent panel material instance.
    constexpr AssetId TranslucentInstance{0x2417};
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "projection shell: the document on the shell lands where the screen-space "
                  "overlay puts it, and a translated eye breaks it")
{
    RegisterBuiltinTypes(Types);

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(CookPanelPack()).has_value());

    const AssetResult<AssetHandle<MaterialInstance>> panelMaterial =
        assets.LoadSync<MaterialInstance>(TranslucentInstance);
    REQUIRE(panelMaterial.has_value());
    REQUIRE(panelMaterial->Get()->GetDomain() == MaterialDomain::Translucent);

    // ---- The reference: the pattern recorded straight into its screen sub-rectangle ----------
    const Ref<Image> overlayScene =
        Image::Create(Context, {
                                   .Name = "Shell Overlay Scene",
                                   .Extent = {Extent.x, Extent.y, 1},
                                   .Format = Format::RGBA16Sfloat,
                                   .Usage = ImageUsage::ColorAttachment | ImageUsage::Sampled |
                                            ImageUsage::TransferSrc,
                               });
    const Ref<ImageView> overlaySceneView =
        ImageView::Create(Context, {.Name = "Shell Overlay Scene View", .Image = overlayScene});
    // Black, so premultiplied-over and straight-alpha-over agree — see the reconciliation note.
    ClearImage(Context, overlaySceneView, ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f});

    const Unique<Gui::Document> overlayDocument =
        BuildPatternDocument(RectScreenMin(), RectScreenSize());
    overlayDocument->Solve(vec2(Extent));
    Gui::DrawList overlayList;
    overlayDocument->Build(overlayList);

    const Unique<GuiScenePass> overlayPass = GuiScenePass::Create({
        .Context = Context,
        .Assets = assets,
        .Extent = Extent,
        .OutputFormat = Format::RGBA16Sfloat,
    });
    overlayPass->SetDrawList(overlayList);
    Context.ImmediateCommands([&](CommandBuffer& cmd)
                              { overlayPass->Render(cmd, overlaySceneView); });
    const vector<f32> overlay =
        DecodeLuminance(overlayPass->GetOutput()->GetImage()->Download(), Extent);

    // ---- The shell: the same pattern on the generated mesh, through the translucent pass -----
    const Unique<Scene> scene = Scene::Create(Types);
    const Ref<Mesh> shell =
        Mesh::BuildSync(Context,
                        Primitives::ProjectionShell(FovY, Aspect, RectCenter, RectSize, ShellRadius,
                                                    Subdivisions, *panelMaterial),
                        "Projection Shell");

    // The shell is generated in camera space, so at the reference pose the entity's transform is the
    // identity: the eye is the world origin looking down -Z.
    const Entity shellEntity = scene->CreateEntity();
    scene->Add<Transform>(shellEntity);
    scene->Add<MeshRenderer>(shellEntity).Mesh = assets.Adopt(shell);

    auto& surface = scene->Add<GuiSurface>(shellEntity);
    surface.Resolution = Canvas;
    surface.Domain = GuiSurfaceDomain::Translucent;
    surface.SetDocument(BuildPatternDocument(vec2(0.0f), vec2(Canvas)));

    // Every post effect that would move a pixel or spread intensity is off, and the tone curve is
    // the identity: what is compared is geometry.
    const Unique<Viewport> viewport = Viewport::Create({
        .Context = Context,
        .Assets = assets,
        .Region = {.Offset = {0, 0}, .Extent = Extent},
        .ColorFormat = Format::RGBA16Sfloat,
        .Settings =
            {.Bloom = false, .TAA = false, .Shadows = false, .PunctualShadows = false, .AO = false},
        .Role = ViewportRole::Offscreen,
    });

    const auto capture = [&](const vec3 eye)
    {
        CameraView camera;
        camera.SetPerspective(FovY, Aspect, 0.05f, 100.0f);
        camera.SetViewFromWorld(glm::translate(mat4(1.0f), eye));
        viewport->SetViewState({.World = scene.get(),
                                .Camera = camera,
                                .Delta = 0.016f,
                                .Exposure = 1.0f,
                                .Tonemapper = Tonemapper::None});
        Context.ImmediateCommands([&](CommandBuffer& cmd) { viewport->Render(cmd); });
        return DecodeLuminance(viewport->GetOutput()->GetImage()->Download(), Extent);
    };

    const vector<f32> reference = capture(vec3(0.0f));
    REQUIRE(surface.GetTarget() != nullptr);
    CHECK(surface.GetTarget()->GetExtent() == Canvas);

    // ---- The measurement ---------------------------------------------------------------------
    const f32 threshold = Primitives::ProjectionShellReprojectionBound(
        FovY, Aspect, RectCenter, RectSize, Subdivisions, vec2(Extent));
    const f32 displacement = WorstEdgeDisplacement(overlay, reference);
    MESSAGE("projection shell: worst edge displacement ", displacement, " points, bound ",
            threshold, " points");
    CHECK(displacement <= threshold);

    // ---- The control: a translated eye, which must break it by a wide margin ------------------
    const vector<f32> offset = capture(OffsetEye);
    const f32 offsetDisplacement = WorstEdgeDisplacement(overlay, offset);
    MESSAGE("projection shell: translated-eye displacement ", offsetDisplacement, " points (",
            offsetDisplacement / threshold, "x the bound)");
    CHECK(offsetDisplacement >= threshold * OffsetMultiple);
}
