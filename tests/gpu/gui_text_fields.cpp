// Per-glyph field-type render cases. A run drawn from an MSDF font and a run drawn from an SDF font
// pack their field type into the per-glyph draw params (params.y: 0 median-of-rgb, 1 red) with no
// vertex-format change, and both render coverage from the one shared distance-range constant through
// GuiScenePass — the SDF branch anti-aliasing from that range exactly as the MSDF branch does. A
// glyph a font's own face lacks renders through its fallback face in the same run. The fonts are
// cooked in process and loaded through an AssetManager wired to a shared GlyphSource + GlyphAtlas.

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <string>

#include "support/TempPath.h"
#include "support/TestCook.h"

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Font.h>
#include <Veng/Asset/HexId.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Gui/Document.h>
#include <Veng/Gui/DrawList.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/Types.h>
#include <Veng/Text/GlyphAtlas.h>
#include <Veng/Text/GlyphSource.h>

#include <Renderer/Passes/GuiScenePass.h>

#include <gpu/fixture.h>
#include <gpu/golden_image.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    constexpr uvec2 Extent{256, 96};
    constexpr AssetId MsdfFontId{0x000000000000F0E0ULL};
    constexpr AssetId SdfFontId{0x000000000000F0E1ULL};
    constexpr AssetId BaseFontId{0x000000000000F0E2ULL};
    constexpr AssetId FallbackFontId{0x000000000000F0E3ULL};

    void WriteFile(const path& file, const std::string& text)
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << text;
    }

    path CaseDir(const char* name)
    {
        const path dir =
            Veng::TestSupport::TempDir() / (std::string("veng_gpu_text_fields_") + name);
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        return dir;
    }

    // A single-font pack from the default face at the given field type.
    void AddFont(const path& dir, std::string& assets, AssetId id, const char* field,
                 const char* file, const optional<AssetId>& fallback)
    {
        const path ttf = path(VENG_DEFAULT_FONT_TTF);
        std::string src = "{\n  \"font\": \"" + ttf.generic_string() + "\",\n  \"field\": \"" +
                          field + "\",\n  \"hotset\": \"ascii\"";
        if (fallback)
        {
            src += ",\n  \"fallback\": [\"" + FormatAssetId(*fallback) + "\"]";
        }
        src += "\n}\n";
        WriteFile(dir / file, src);
        if (!assets.empty())
        {
            assets += ",";
        }
        assets += "{\"id\": \"" + FormatAssetId(id) + "\", \"type\": \"Font\", \"source\": \"" +
                  file + "\"}";
    }

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

    // Renders a built draw list through a GuiScenePass over a black scene and returns the composite
    // as 8-bit RGB. The glyph uploads ride the pass's own RecordUploads at the top of Render.
    vector<u8> RenderList(Context& context, AssetManager& assets, const Gui::DrawList& list)
    {
        const Ref<Image> sceneImage =
            Image::Create(context, {
                                       .Name = "Text Fields Scene",
                                       .Extent = {Extent.x, Extent.y, 1},
                                       .Format = Format::RGBA16Sfloat,
                                       .Usage = ImageUsage::ColorAttachment | ImageUsage::Sampled |
                                                ImageUsage::TransferSrc,
                                   });
        const Ref<ImageView> sceneView =
            ImageView::Create(context, {.Name = "Text Fields Scene View", .Image = sceneImage});
        ClearImage(context, sceneView, ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f});

        const Unique<GuiScenePass> pass = GuiScenePass::Create({
            .Context = context,
            .Assets = assets,
            .Extent = Extent,
            .OutputFormat = Format::RGBA16Sfloat,
        });
        pass->SetDrawList(list);
        context.ImmediateCommands([&](CommandBuffer& cmd) { pass->Render(cmd, sceneView); });

        const vector<u8> raw = pass->GetOutput()->GetImage()->Download();
        REQUIRE(raw.size() == static_cast<usize>(Extent.x) * Extent.y * 8);
        return Veng::Test::DecodeHalfRgb(raw, Extent);
    }

    // Whether any pixel in a horizontal band exceeds a per-channel brightness, and whether the band
    // carries a partial-coverage (anti-aliased) edge value between the background and full text.
    struct BandStats
    {
        bool AnyLit = false;
        bool AnyPartial = false;
    };

    BandStats ScanBand(const vector<u8>& rgb, u32 y0, u32 y1)
    {
        BandStats stats;
        for (u32 y = y0; y < y1; ++y)
        {
            for (u32 x = 0; x < Extent.x; ++x)
            {
                const int g = rgb[(static_cast<usize>(y) * Extent.x + x) * 3 + 1];
                if (g > 40)
                {
                    stats.AnyLit = true;
                }
                if (g > 30 && g < 200)
                {
                    stats.AnyPartial = true;
                }
            }
        }
        return stats;
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "gui text fields: a run tags each glyph with its font's field type in the params")
{
    const path dir = CaseDir("params");
    std::string assetList;
    AddFont(dir, assetList, MsdfFontId, "msdf", "msdf.font.json", std::nullopt);
    AddFont(dir, assetList, SdfFontId, "sdf", "sdf.font.json", std::nullopt);
    WriteFile(dir / "pack.json", "{\"version\": 1, \"assets\": [" + assetList + "]}");
    const path archive = dir / "fields.vengpack";
    Cook::Cooker cooker;
    Cook::RegisterBuiltinImporters(cooker);
    REQUIRE(Veng::TestSupport::CookCached(cooker, dir / "pack.json", archive).has_value());

    Text::GlyphSource source;
    Text::GlyphAtlas atlas(Context, source);
    AssetManager assets(Context, Tasks, Types);
    assets.SetGlyphSystems(&source, &atlas);
    REQUIRE(assets.Mount(archive).has_value());

    const AssetResult<AssetHandle<Font>> msdf = assets.LoadSync<Font>(MsdfFontId);
    const AssetResult<AssetHandle<Font>> sdf = assets.LoadSync<Font>(SdfFontId);
    REQUIRE(msdf.has_value());
    REQUIRE(sdf.has_value());

    Gui::DrawList list;
    list.Text({8.0f, 8.0f}, *msdf->Get(), "A", 40.0f, vec4(1.0f));
    list.Text({8.0f, 56.0f}, *sdf->Get(), "A", 40.0f, vec4(1.0f));

    // The field type rides params.y (0 Msdf, 1 Sdf), the page rides params.z, the sampler params.w,
    // and the shared distance range params.x — all on the existing vec4, no vertex-format change.
    const f32 range = atlas.GetDistanceRange();
    bool sawMsdf = false;
    bool sawSdf = false;
    for (const Gui::GuiVertex& vertex : list.GetVertices())
    {
        CHECK(vertex.Params.x == doctest::Approx(range));
        CHECK(vertex.Params.z >= 0.0f);
        CHECK(vertex.Params.w == doctest::Approx(static_cast<f32>(atlas.GetSamplerHandle().Index)));
        if (vertex.Params.y < 0.5f)
        {
            sawMsdf = true;
        }
        else
        {
            sawSdf = true;
        }
    }
    CHECK(sawMsdf);
    CHECK(sawSdf);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "gui text fields: MSDF and SDF glyphs both render coverage from the shared range")
{
    const path dir = CaseDir("render");
    std::string assetList;
    AddFont(dir, assetList, MsdfFontId, "msdf", "msdf.font.json", std::nullopt);
    AddFont(dir, assetList, SdfFontId, "sdf", "sdf.font.json", std::nullopt);
    WriteFile(dir / "pack.json", "{\"version\": 1, \"assets\": [" + assetList + "]}");
    const path archive = dir / "fields.vengpack";
    Cook::Cooker cooker;
    Cook::RegisterBuiltinImporters(cooker);
    REQUIRE(Veng::TestSupport::CookCached(cooker, dir / "pack.json", archive).has_value());

    Text::GlyphSource source;
    Text::GlyphAtlas atlas(Context, source);
    AssetManager assets(Context, Tasks, Types);
    assets.SetGlyphSystems(&source, &atlas);
    REQUIRE(assets.Mount(archive).has_value());

    const AssetResult<AssetHandle<Font>> msdf = assets.LoadSync<Font>(MsdfFontId);
    const AssetResult<AssetHandle<Font>> sdf = assets.LoadSync<Font>(SdfFontId);
    REQUIRE(msdf.has_value());
    REQUIRE(sdf.has_value());

    Gui::DrawList list;
    // The MSDF run in the top band, the SDF run in the bottom band, so a per-band scan can tell the
    // two coverage paths apart in the readback.
    list.Text({8.0f, 6.0f}, *msdf->Get(), "Ag", 36.0f, vec4(1.0f));
    list.Text({8.0f, 52.0f}, *sdf->Get(), "Ag", 36.0f, vec4(1.0f));

    const vector<u8> rgb = RenderList(Context, assets, list);

    // Both bands carry lit text, and both carry an anti-aliased edge value — the SDF branch resolves
    // its screen-pixel range from the same shared constant the MSDF branch does, not a hard 0/1 cut.
    const BandStats top = ScanBand(rgb, 0, Extent.y / 2);
    const BandStats bottom = ScanBand(rgb, Extent.y / 2, Extent.y);
    CHECK(top.AnyLit);
    CHECK(top.AnyPartial);
    CHECK(bottom.AnyLit);
    CHECK(bottom.AnyPartial);

    std::filesystem::remove(archive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "gui text fields: a glyph from a fallback face renders in the run")
{
    const path dir = CaseDir("fallback");
    std::string assetList;
    // The base is the minimal synthetic face (space/A/V/T only); it falls back to the broad default
    // face, so a letter the base lacks resolves through the fallback and still draws.
    const path baseTtf = path(GPU_COOKER_FIXTURE_DIR) / "fonts" / "VengTestKern.ttf";
    WriteFile(dir / "base.font.json",
              "{\n  \"font\": \"" + baseTtf.generic_string() +
                  "\",\n  \"field\": \"msdf\",\n  \"hotset\": \"ascii\",\n  \"fallback\": [\"" +
                  FormatAssetId(FallbackFontId) + "\"]\n}\n");
    assetList = "{\"id\": \"" + FormatAssetId(BaseFontId) +
                "\", \"type\": \"Font\", \"source\": \"base.font.json\"}";
    AddFont(dir, assetList, FallbackFontId, "msdf", "fallback.font.json", std::nullopt);
    WriteFile(dir / "pack.json", "{\"version\": 1, \"assets\": [" + assetList + "]}");
    const path archive = dir / "fields.vengpack";
    Cook::Cooker cooker;
    Cook::RegisterBuiltinImporters(cooker);
    REQUIRE(Veng::TestSupport::CookCached(cooker, dir / "pack.json", archive).has_value());

    Text::GlyphSource source;
    Text::GlyphAtlas atlas(Context, source);
    AssetManager assets(Context, Tasks, Types);
    assets.SetGlyphSystems(&source, &atlas);
    REQUIRE(assets.Mount(archive).has_value());

    const AssetResult<AssetHandle<Font>> base = assets.LoadSync<Font>(BaseFontId);
    REQUIRE(base.has_value());
    const Font& font = *base->Get();

    // A lowercase letter the base face lacks but the fallback covers.
    u32 crossFace = 0;
    for (u32 cp = 'a'; cp <= 'z' && crossFace == 0; cp++)
    {
        if (!source.HasGlyph(font.GetFaceId(), cp) && font.HasGlyph(cp))
        {
            crossFace = cp;
        }
    }
    REQUIRE_MESSAGE(crossFace != 0, "no codepoint the base lacks and the fallback covers");

    const std::array<u32, 1> run = {crossFace};
    Gui::DrawList list;
    const string text(1, static_cast<char>(crossFace));
    list.Text({8.0f, 20.0f}, font, text, 40.0f, vec4(1.0f));

    // The fallback glyph shaped to a visible quad on a resident page.
    const ShapeResult shaped = font.ShapeRun(run, 40.0f, std::nullopt, TextShapeMode::Draw);
    REQUIRE(shaped.Glyphs.size() == 1);
    CHECK(shaped.Glyphs[0].Page.IsValid());

    const vector<u8> rgb = RenderList(Context, assets, list);
    const BandStats band = ScanBand(rgb, 0, Extent.y);
    CHECK(band.AnyLit);

    std::filesystem::remove(archive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "gui text fields: a document shapes an unchanged run once, for its measure and "
                  "its paint")
{
    const path dir = CaseDir("shaped_once");
    std::string assetList;
    AddFont(dir, assetList, MsdfFontId, "msdf", "msdf.font.json", std::nullopt);
    WriteFile(dir / "pack.json", "{\"version\": 1, \"assets\": [" + assetList + "]}");
    const path archive = dir / "fields.vengpack";
    Cook::Cooker cooker;
    Cook::RegisterBuiltinImporters(cooker);
    REQUIRE(Veng::TestSupport::CookCached(cooker, dir / "pack.json", archive).has_value());

    Text::GlyphSource source;
    Text::GlyphAtlas atlas(Context, source);
    AssetManager assets(Context, Tasks, Types);
    assets.SetGlyphSystems(&source, &atlas);
    REQUIRE(assets.Mount(archive).has_value());
    const AssetResult<AssetHandle<Font>> font = assets.LoadSync<Font>(MsdfFontId);
    REQUIRE(font.has_value());

    // A readout under a root that names the font once, as a document authors it; the size is the
    // readout's own, since only the font inherits.
    Gui::Document doc;
    Gui::Style root;
    root.TextFont = *font;
    root.AlignItems = Gui::Align::FlexStart;
    doc.SetStyle(doc.Root(), root);
    Gui::Element& readout = doc.Add(doc.Root(), Gui::ElementKind::Text);
    doc.SetText(readout, "SPEED 120");

    const vec2 available(Extent);
    Gui::DrawList out;
    const auto frame = [&]
    {
        atlas.BeginFrame();
        out.Clear();
        doc.Drive(available, 0.016f, out);
    };

    // The first frame shapes the run once: the measure shapes it and the paint finds it.
    frame();
    CHECK(doc.GetStats().ShapedRuns == 1);

    // Unchanged, it is never shaped again.
    frame();
    frame();
    CHECK(doc.GetStats().ShapedRuns == 1);

    // The paint is the run a fresh shaping of the same string draws at the same pen.
    Gui::DrawList fresh;
    fresh.Text(readout.Layout.Min, *font->Get(), "SPEED 120", readout.ComputedStyle.TextSize,
               vec4(1.0f));
    REQUIRE(out.GetVertices().size() == fresh.GetVertices().size());
    for (usize i = 0; i < fresh.GetVertices().size(); ++i)
    {
        CHECK(out.GetVertices()[i].Position == fresh.GetVertices()[i].Position);
        CHECK(out.GetVertices()[i].Uv == fresh.GetVertices()[i].Uv);
    }

    // A ticking readout reshapes itself, once.
    doc.SetText(readout, "SPEED 121");
    frame();
    CHECK(doc.GetStats().ShapedRuns == 2);

    std::filesystem::remove(archive);
}
