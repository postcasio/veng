// Gui render golden: builds a hand-authored DrawList — a rounded panel, a 9-slice frame,
// a tinted texture, and a line of text at two pixel sizes — renders it through GuiScenePass
// over a solid scene output, downloads the composited result, and fuzzy-compares it to a
// committed golden PNG. The image floor a change to what *builds* a draw list holds stable.
//
// Set VENG_GUI_GOLDEN_DUMP=<path.ppm> to write the capture instead of comparing — the way the
// golden is (re)generated: dump, sips the PPM to tests/golden/gui_overlay.png, commit. Each of the
// document-driven cases below carries its own dump variable and golden on the same pattern
// (VENG_GUI_ROTATED_GOLDEN_DUMP, VENG_GUI_IMAGE_GOLDEN_DUMP, VENG_GUI_BACKGROUND_GOLDEN_DUMP,
// VENG_GUI_SHADOW_GOLDEN_DUMP, VENG_GUI_MATERIAL_GOLDEN_DUMP, VENG_GUI_POPUP_GOLDEN_DUMP).
//
// The same font fixture backs one non-rendering case here: a TextInput built with a resident font
// emits its own value as a glyph run, which needs a real atlas and so cannot live in the
// device-free widget suite.

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include <doctest/doctest.h>

// stb_image's implementation is already compiled into libveng_cook (which this target links),
// so include only the header — defining the implementation here would duplicate its symbols.
#include <stb_image.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Font.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Gui/Document.h>
#include <Veng/Gui/DrawList.h>
#include <Veng/Gui/UIDocument.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Renderer/Types.h>

#include <Renderer/Passes/GuiScenePass.h>

#include <gpu/fixture.h>
#include "support/TempPath.h"

// After the Veng headers, so Veng.h's GLM_FORCE_DEPTH_ZERO_TO_ONE is set before glm.
#include <glm/gtc/packing.hpp>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    constexpr uvec2 Extent{256, 192};

    // The font fixture pack's Font AssetId (tests/cooker/fixtures/font_pack.json).
    constexpr AssetId FontId{0xFB6782CABF076640ULL};

    // Golden fuzziness: the same tolerance shape golden_compare uses, widened slightly for the
    // MSDF text edges whose derivative-based anti-aliasing can jitter a pixel between drivers.
    constexpr int MaxChannelDelta = 10;
    constexpr double MaxMismatchFraction = 0.02;

    // Clears an image to a solid color through a one-pass graph — the stand-in scene output the UI
    // composites over.
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

    // Builds a small resident RGBA8 checker texture and registers it into bindless. The caller
    // keeps the returned view/handle alive and releases the handle itself.
    struct CheckerTexture
    {
        Ref<Image> Image;
        Ref<ImageView> View;
        TextureHandle Handle;
    };

    CheckerTexture MakeChecker(Context& context)
    {
        constexpr u32 size = 16;
        std::array<u8, size * size * 4> pixels{};
        for (u32 y = 0; y < size; ++y)
        {
            for (u32 x = 0; x < size; ++x)
            {
                const bool on = ((x / 4) + (y / 4)) % 2 == 0;
                const usize i = (static_cast<usize>(y) * size + x) * 4;
                pixels[i + 0] = on ? 240 : 40;
                pixels[i + 1] = on ? 120 : 40;
                pixels[i + 2] = on ? 40 : 200;
                pixels[i + 3] = 255;
            }
        }

        const Ref<Renderer::Image> image =
            Image::Create(context, {
                                       .Name = "Gui Checker",
                                       .Extent = {size, size, 1},
                                       .Format = Format::RGBA8Unorm,
                                       .Usage = ImageUsage::Sampled | ImageUsage::TransferDst,
                                   });
        image->UploadSync(std::span<const u8>(pixels.data(), pixels.size()));
        const Ref<ImageView> view =
            ImageView::Create(context, {.Name = "Gui Checker View", .Image = image});
        const TextureHandle handle = context.GetBindlessRegistry().Register(view);
        return {.Image = image, .View = view, .Handle = handle};
    }

    // Converts an RGBA16Sfloat download into 8-bit RGB, clamped to [0,1].
    vector<u8> DecodeHalfRgb(const vector<u8>& halfBytes, uvec2 extent)
    {
        const auto* halves = reinterpret_cast<const u16*>(halfBytes.data());
        vector<u8> rgb;
        rgb.reserve(static_cast<usize>(extent.x) * extent.y * 3);
        for (u32 pixel = 0; pixel < extent.x * extent.y; ++pixel)
        {
            for (u32 channel = 0; channel < 3; ++channel)
            {
                const f32 value =
                    glm::clamp(glm::unpackHalf1x16(halves[pixel * 4 + channel]), 0.0f, 1.0f);
                rgb.push_back(static_cast<u8>(value * 255.0f + 0.5f));
            }
        }
        return rgb;
    }

    void WritePpm(const path& out, const vector<u8>& rgb, uvec2 extent)
    {
        std::ofstream stream(out, std::ios::binary);
        stream << "P6\n" << extent.x << " " << extent.y << "\n255\n";
        stream.write(reinterpret_cast<const char*>(rgb.data()),
                     static_cast<std::streamsize>(rgb.size()));
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "gui gradient: a linear ramp fill renders red at the top and blue at the bottom")
{
    AssetManager assets(Context, Tasks, Types);

    // A solid scene output to composite the gradient over.
    const Ref<Image> sceneImage =
        Image::Create(Context, {
                                   .Name = "Gui Gradient Scene",
                                   .Extent = {Extent.x, Extent.y, 1},
                                   .Format = Format::RGBA16Sfloat,
                                   .Usage = ImageUsage::ColorAttachment | ImageUsage::Sampled |
                                            ImageUsage::TransferSrc,
                               });
    const Ref<ImageView> sceneView =
        ImageView::Create(Context, {.Name = "Gui Gradient Scene View", .Image = sceneImage});
    ClearImage(Context, sceneView, ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f});

    // A 256×1 red→blue ramp LUT (linear straight-alpha), the shape the cook bakes.
    constexpr u32 rampWidth = 256;
    std::array<u8, rampWidth * 4> rampPixels{};
    for (u32 x = 0; x < rampWidth; ++x)
    {
        const f32 t = static_cast<f32>(x) / static_cast<f32>(rampWidth - 1);
        rampPixels[x * 4 + 0] = static_cast<u8>((1.0f - t) * 255.0f + 0.5f);
        rampPixels[x * 4 + 1] = 0;
        rampPixels[x * 4 + 2] = static_cast<u8>(t * 255.0f + 0.5f);
        rampPixels[x * 4 + 3] = 255;
    }
    const Ref<Image> rampImage =
        Image::Create(Context, {
                                   .Name = "Gui Gradient Ramp",
                                   .Extent = {rampWidth, 1, 1},
                                   .Format = Format::RGBA8Unorm,
                                   .Usage = ImageUsage::Sampled | ImageUsage::TransferDst,
                               });
    rampImage->UploadSync(std::span<const u8>(rampPixels.data(), rampPixels.size()));
    const Ref<ImageView> rampView =
        ImageView::Create(Context, {.Name = "Gui Gradient Ramp View", .Image = rampImage});
    const TextureHandle rampHandle = Context.GetBindlessRegistry().Register(rampView);
    const Ref<Sampler> sampler =
        Sampler::Create(Context, {
                                     .Name = "Gui Gradient Sampler",
                                     .MagFilter = Filter::Linear,
                                     .MinFilter = Filter::Linear,
                                     .AddressModeU = AddressMode::ClampToEdge,
                                     .AddressModeV = AddressMode::ClampToEdge,
                                     .AddressModeW = AddressMode::ClampToEdge,
                                 });
    const SamplerHandle samplerHandle = Context.GetBindlessRegistry().Register(sampler);

    // A vertical linear gradient from the box top (P0) to the bottom (P1): t = (p.y + 1) / 2.
    const Gui::Rect rect{.Min = {20.0f, 20.0f}, .Size = {120.0f, 120.0f}};
    Gui::DrawList list;
    list.Gradient(rect, Gui::GradientFill{.Kind = Gui::GradientKind::Linear,
                                          .P0 = vec2(0.0f, -1.0f),
                                          .P1 = vec2(0.0f, 1.0f),
                                          .Ramp = rampHandle,
                                          .Sampler = samplerHandle});

    const Unique<GuiScenePass> pass = GuiScenePass::Create({
        .Context = Context,
        .Assets = assets,
        .Extent = Extent,
        .OutputFormat = Format::RGBA16Sfloat,
    });
    pass->SetDrawList(list);
    Context.ImmediateCommands([&](CommandBuffer& cmd) { pass->Render(cmd, sceneView); });

    const vector<u8> raw = pass->GetOutput()->GetImage()->Download();
    REQUIRE(raw.size() == static_cast<usize>(Extent.x) * Extent.y * 8);
    const vector<u8> rgb = DecodeHalfRgb(raw, Extent);

    Context.GetBindlessRegistry().Release(samplerHandle);
    Context.GetBindlessRegistry().Release(rampHandle);

    const auto sampleRgb = [&](u32 x, u32 y)
    {
        const usize i = (static_cast<usize>(y) * Extent.x + x) * 3;
        return uvec3(rgb[i], rgb[i + 1], rgb[i + 2]);
    };
    // Top of the rect is red, the bottom is blue, the middle is a mix of both (a real gradient, not
    // a flat fill). Colors are linear (the frag outputs linear premultiplied), so red/blue read high.
    const u32 midX = static_cast<u32>(rect.Center().x);
    const uvec3 top = sampleRgb(midX, static_cast<u32>(rect.Min.y) + 6);
    const uvec3 middle = sampleRgb(midX, static_cast<u32>(rect.Center().y));
    const uvec3 bottom = sampleRgb(midX, static_cast<u32>(rect.Max().y) - 6);
    CHECK(top.r > 180);
    CHECK(top.b < 60);
    CHECK(bottom.b > 180);
    CHECK(bottom.r < 60);
    CHECK(middle.r > 40);
    CHECK(middle.b > 40);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "gui text input: a TextInput emits its own value as a glyph run")
{
    // The one case here that needs a resident font but no render: the device-free widget suite can
    // pin a TextInput's caret geometry, but a glyph run needs a real atlas, so the proof that the
    // field paints its own value — with no companion Text element in the tree — lives with the font
    // fixture.
    const path fixtureDir = path(GPU_COOKER_FIXTURE_DIR);
    const path packJson = fixtureDir / "font_pack.json";
    const path outArchive = Veng::TestSupport::TempDir() / "veng_gpu_gui_text_input.vengpack";

    Cook::Cooker cooker;
    Cook::RegisterBuiltinImporters(cooker);
    REQUIRE(cooker.CookPack(packJson, outArchive).has_value());

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(outArchive).has_value());

    const AssetResult<AssetHandle<Font>> fontHandle = assets.LoadSync<Font>(FontId);
    REQUIRE(fontHandle.has_value());

    Gui::Document document;
    document.SetInteractive(true);
    Gui::Element& root = document.Root();
    root.Layout = Gui::Rect{.Min = {0.0f, 0.0f}, .Size = {200.0f, 200.0f}};

    Gui::Element& field = document.Add(root, Gui::ElementKind::TextInput);
    field.Layout = Gui::Rect{.Min = {10.0f, 10.0f}, .Size = {160.0f, 28.0f}};
    // Both styles: the base is what a state change (focus) re-resolves the computed style from.
    field.BaseStyle.TextFont = *fontHandle;
    field.BaseStyle.TextSize = 20.0f;
    field.ComputedStyle = field.BaseStyle;
    document.SetText(field, "AVA");
    document.InitWidget(field);

    Gui::DrawList painted;
    document.Build(painted);

    // The field is a leaf — nothing else in the tree could have drawn the value.
    CHECK(field.Children.empty());
    CHECK(root.Children.size() == 1);
    const auto glyphRuns =
        std::ranges::count_if(painted.GetRuns(), [](const Gui::DrawRun& run)
                              { return run.Pipeline == Gui::GuiPipeline::Msdf; });
    CHECK(glyphRuns == 1);

    // Typing through the document's own text path grows the run it paints.
    const usize before = painted.GetVertices().size();
    document.SetFocus(&field);
    CHECK(document.DispatchText('V'));
    CHECK(field.Text == "AVAV");

    Gui::DrawList retyped;
    document.Build(retyped);
    CHECK(retyped.GetVertices().size() > before);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "gui typography: a text field inherits its font and reserves a line for it")
{
    // Typography inherits, so a control needs no font of its own: the font declared once on an
    // ancestor serves every text-bearing descendant. Resolving a font needs a real atlas, so this
    // pairs with the font fixture; the layout half it proves (an empty field still holds one line
    // open) is what keeps a field from clipping the value it paints.
    const path fixtureDir = path(GPU_COOKER_FIXTURE_DIR);
    const path packJson = fixtureDir / "font_pack.json";
    const path outArchive = Veng::TestSupport::TempDir() / "veng_gpu_gui_inherited_font.vengpack";

    Cook::Cooker cooker;
    Cook::RegisterBuiltinImporters(cooker);
    REQUIRE(cooker.CookPack(packJson, outArchive).has_value());

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(outArchive).has_value());

    const AssetResult<AssetHandle<Font>> fontHandle = assets.LoadSync<Font>(FontId);
    REQUIRE(fontHandle.has_value());

    Gui::Document document;
    document.SetInteractive(true);

    // The font is declared on the container only — nothing on the field or the label names one.
    Gui::Style rootStyle;
    rootStyle.TextFont = *fontHandle;
    rootStyle.TextSize = 20.0f;
    document.SetStyle(document.Root(), rootStyle);

    Gui::Element& field = document.Add(document.Root(), Gui::ElementKind::TextInput);
    document.SetText(field, "AVA");
    document.InitWidget(field);

    Gui::Element& label = document.Add(document.Root(), Gui::ElementKind::Text);
    document.SetText(label, "AVA");

    Gui::Element& empty = document.Add(document.Root(), Gui::ElementKind::TextInput);
    document.InitWidget(empty);

    document.Solve(vec2(200.0f, 200.0f));

    // The field sizes itself to the run it will paint, exactly as the Text leaf beside it does.
    CHECK_FALSE(field.ComputedStyle.TextFont.IsLoaded());
    CHECK(field.Layout.Size.y > 0.0f);
    CHECK(field.Layout.Size.y == doctest::Approx(label.Layout.Size.y));

    // A field with no value still holds one line of its typography open, so it does not collapse
    // and later grow as the first codepoint arrives.
    CHECK(empty.Layout.Size.y == doctest::Approx(field.Layout.Size.y));

    const auto glyphRuns = [](const Gui::DrawList& list)
    {
        return std::ranges::count_if(list.GetRuns(), [](const Gui::DrawRun& run)
                                     { return run.Pipeline == Gui::GuiPipeline::Msdf; });
    };

    Gui::DrawList painted;
    document.Build(painted);
    CHECK(glyphRuns(painted) >= 1);

    // The container's font is the only one in the tree: dropping it leaves both leaves unpainted,
    // so the run above came from the inheritance and nothing else.
    rootStyle.TextFont = {};
    document.SetStyle(document.Root(), rootStyle);
    Gui::DrawList unstyled;
    document.Build(unstyled);
    CHECK(glyphRuns(unstyled) == 0);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "gui golden: a hand-built draw list renders and matches the committed golden")
{
    const path fixtureDir = path(GPU_COOKER_FIXTURE_DIR);
    const path packJson = fixtureDir / "font_pack.json";
    const path outArchive = Veng::TestSupport::TempDir() / "veng_gpu_gui_font.vengpack";

    Cook::Cooker cooker;
    Cook::RegisterBuiltinImporters(cooker);
    REQUIRE(cooker.CookPack(packJson, outArchive).has_value());

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(outArchive).has_value());

    const AssetResult<AssetHandle<Font>> fontHandle = assets.LoadSync<Font>(FontId);
    REQUIRE(fontHandle.has_value());
    const Font& font = *fontHandle->Get();

    // A solid scene output to composite the UI over.
    const Ref<Image> sceneImage =
        Image::Create(Context, {
                                   .Name = "Gui Scene Output",
                                   .Extent = {Extent.x, Extent.y, 1},
                                   .Format = Format::RGBA16Sfloat,
                                   .Usage = ImageUsage::ColorAttachment | ImageUsage::Sampled |
                                            ImageUsage::TransferSrc,
                               });
    const Ref<ImageView> sceneView =
        ImageView::Create(Context, {.Name = "Gui Scene Output View", .Image = sceneImage});
    ClearImage(Context, sceneView, ClearColor{.R = 0.10f, .G = 0.12f, .B = 0.16f, .A = 1.0f});

    const CheckerTexture checker = MakeChecker(Context);
    const Ref<Sampler> sampler =
        Sampler::Create(Context, {
                                     .Name = "Gui Golden Sampler",
                                     .MagFilter = Filter::Linear,
                                     .MinFilter = Filter::Linear,
                                     .AddressModeU = AddressMode::ClampToEdge,
                                     .AddressModeV = AddressMode::ClampToEdge,
                                     .AddressModeW = AddressMode::ClampToEdge,
                                 });
    const SamplerHandle samplerHandle = Context.GetBindlessRegistry().Register(sampler);

    // The hand-authored draw list: a rounded panel, a 9-slice frame, a tinted texture, and text at
    // two pixel sizes. Colors are linear (the draw list's contract).
    Gui::DrawList list;
    list.Quad({.Min = {16.0f, 16.0f}, .Size = {150.0f, 90.0f}}, vec4(0.20f, 0.22f, 0.30f, 0.92f),
              Gui::CornerRadii::All(12.0f));
    list.Quad({.Min = {16.0f, 16.0f}, .Size = {150.0f, 90.0f}}, vec4(0.0f),
              Gui::CornerRadii::All(12.0f),
              Gui::Border{.Width = 2.0f, .Color = vec4(0.55f, 0.75f, 1.0f, 1.0f)});

    list.NineSlice({.Min = {182.0f, 16.0f}, .Size = {58.0f, 58.0f}}, checker.Handle, samplerHandle,
                   Gui::Insets::All(0.3f), Gui::Insets::All(14.0f), vec4(1.0f, 1.0f, 1.0f, 1.0f));

    list.Texture({.Min = {182.0f, 84.0f}, .Size = {58.0f, 58.0f}}, checker.Handle, samplerHandle,
                 {.Min = {0.0f, 0.0f}, .Size = {1.0f, 1.0f}}, vec4(1.0f, 0.6f, 0.3f, 1.0f));

    list.Text({28.0f, 40.0f}, font, "AVA", 40.0f, vec4(0.95f, 0.95f, 0.98f, 1.0f));
    list.Text({28.0f, 84.0f}, font, "AV.", 22.0f, vec4(0.70f, 0.90f, 0.75f, 1.0f));

    const Unique<GuiScenePass> pass = GuiScenePass::Create({
        .Context = Context,
        .Assets = assets,
        .Extent = Extent,
        .OutputFormat = Format::RGBA16Sfloat,
    });

    pass->SetDrawList(list);
    Context.ImmediateCommands([&](CommandBuffer& cmd) { pass->Render(cmd, sceneView); });

    const vector<u8> raw = pass->GetOutput()->GetImage()->Download();
    REQUIRE(raw.size() == static_cast<usize>(Extent.x) * Extent.y * 8);
    const vector<u8> actual = DecodeHalfRgb(raw, Extent);

    Context.GetBindlessRegistry().Release(samplerHandle);
    Context.GetBindlessRegistry().Release(checker.Handle);

    if (const char* dump = std::getenv("VENG_GUI_GOLDEN_DUMP"))
    {
        WritePpm(path(dump), actual, Extent);
        MESSAGE("gui golden: wrote capture to ", dump);
        std::filesystem::remove(outArchive);
        return;
    }

    const path golden = path(GUI_GOLDEN_DIR) / "gui_overlay.png";
    int gw = 0;
    int gh = 0;
    int gc = 0;
    u8* goldenPixels = stbi_load(golden.string().c_str(), &gw, &gh, &gc, 3);
    REQUIRE_MESSAGE(goldenPixels != nullptr, "gui golden: failed to load ", golden.string());
    REQUIRE(static_cast<u32>(gw) == Extent.x);
    REQUIRE(static_cast<u32>(gh) == Extent.y);

    const long pixelCount = static_cast<long>(Extent.x) * Extent.y;
    long mismatched = 0;
    int worst = 0;
    for (long i = 0; i < pixelCount; ++i)
    {
        int pixelDelta = 0;
        for (int c = 0; c < 3; ++c)
        {
            const int a = actual[i * 3 + c];
            const int g = goldenPixels[i * 3 + c];
            const int d = a > g ? a - g : g - a;
            pixelDelta = d > pixelDelta ? d : pixelDelta;
        }
        worst = pixelDelta > worst ? pixelDelta : worst;
        if (pixelDelta > MaxChannelDelta)
        {
            ++mismatched;
        }
    }
    stbi_image_free(goldenPixels);

    const double fraction = static_cast<double>(mismatched) / static_cast<double>(pixelCount);
    MESSAGE("gui golden: ", mismatched, "/", pixelCount, " pixels exceed delta ", MaxChannelDelta,
            " (worst ", worst, ")");
    CHECK(fraction <= MaxMismatchFraction);

    std::filesystem::remove(outArchive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "gui rotation golden: a rotated subtree renders rigidly and matches its golden")
{
    // A rotated subtree — a rounded bordered panel, a gradient inset, a tinted texture, and a text
    // run — all under a single 30° transform about the composition's center. The rounded-rect SDF,
    // gradient, texture, and MSDF glyphs rotate rigidly by construction (only positions transform),
    // which this pins at the pixel level.
    const path fixtureDir = path(GPU_COOKER_FIXTURE_DIR);
    const path packJson = fixtureDir / "font_pack.json";
    const path outArchive = Veng::TestSupport::TempDir() / "veng_gpu_gui_rotated.vengpack";

    Cook::Cooker cooker;
    Cook::RegisterBuiltinImporters(cooker);
    REQUIRE(cooker.CookPack(packJson, outArchive).has_value());

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(outArchive).has_value());

    const AssetResult<AssetHandle<Font>> fontHandle = assets.LoadSync<Font>(FontId);
    REQUIRE(fontHandle.has_value());
    const Font& font = *fontHandle->Get();

    const Ref<Image> sceneImage =
        Image::Create(Context, {
                                   .Name = "Gui Rotated Scene",
                                   .Extent = {Extent.x, Extent.y, 1},
                                   .Format = Format::RGBA16Sfloat,
                                   .Usage = ImageUsage::ColorAttachment | ImageUsage::Sampled |
                                            ImageUsage::TransferSrc,
                               });
    const Ref<ImageView> sceneView =
        ImageView::Create(Context, {.Name = "Gui Rotated Scene View", .Image = sceneImage});
    ClearImage(Context, sceneView, ClearColor{.R = 0.10f, .G = 0.12f, .B = 0.16f, .A = 1.0f});

    const CheckerTexture checker = MakeChecker(Context);
    const Ref<Sampler> sampler =
        Sampler::Create(Context, {
                                     .Name = "Gui Rotated Sampler",
                                     .MagFilter = Filter::Linear,
                                     .MinFilter = Filter::Linear,
                                     .AddressModeU = AddressMode::ClampToEdge,
                                     .AddressModeV = AddressMode::ClampToEdge,
                                     .AddressModeW = AddressMode::ClampToEdge,
                                 });
    const SamplerHandle samplerHandle = Context.GetBindlessRegistry().Register(sampler);

    // A 256×1 green→magenta ramp for the gradient inset.
    constexpr u32 rampWidth = 256;
    std::array<u8, rampWidth * 4> rampPixels{};
    for (u32 x = 0; x < rampWidth; ++x)
    {
        const f32 t = static_cast<f32>(x) / static_cast<f32>(rampWidth - 1);
        rampPixels[x * 4 + 0] = static_cast<u8>(t * 255.0f + 0.5f);
        rampPixels[x * 4 + 1] = static_cast<u8>((1.0f - t) * 255.0f + 0.5f);
        rampPixels[x * 4 + 2] = static_cast<u8>(t * 255.0f + 0.5f);
        rampPixels[x * 4 + 3] = 255;
    }
    const Ref<Image> rampImage =
        Image::Create(Context, {
                                   .Name = "Gui Rotated Ramp",
                                   .Extent = {rampWidth, 1, 1},
                                   .Format = Format::RGBA8Unorm,
                                   .Usage = ImageUsage::Sampled | ImageUsage::TransferDst,
                               });
    rampImage->UploadSync(std::span<const u8>(rampPixels.data(), rampPixels.size()));
    const Ref<ImageView> rampView =
        ImageView::Create(Context, {.Name = "Gui Rotated Ramp View", .Image = rampImage});
    const TextureHandle rampHandle = Context.GetBindlessRegistry().Register(rampView);

    // The subtree, authored unrotated, then wrapped in a single 30° transform about the center.
    const Gui::Rect panel{.Min = {53.0f, 46.0f}, .Size = {150.0f, 100.0f}};
    Gui::DrawList list;
    list.PushTransform(panel.Center(), glm::radians(30.0f));
    list.Quad(panel, vec4(0.20f, 0.22f, 0.30f, 0.95f), Gui::CornerRadii::All(14.0f));
    list.Quad(panel, vec4(0.0f), Gui::CornerRadii::All(14.0f),
              Gui::Border{.Width = 3.0f, .Color = vec4(0.55f, 0.75f, 1.0f, 1.0f)});
    list.Gradient({.Min = {66.0f, 60.0f}, .Size = {80.0f, 30.0f}},
                  Gui::GradientFill{.Kind = Gui::GradientKind::Linear,
                                    .P0 = vec2(-1.0f, 0.0f),
                                    .P1 = vec2(1.0f, 0.0f),
                                    .Ramp = rampHandle,
                                    .Sampler = samplerHandle},
                  Gui::CornerRadii::All(6.0f));
    list.Texture({.Min = {156.0f, 96.0f}, .Size = {36.0f, 36.0f}}, checker.Handle, samplerHandle,
                 {.Min = {0.0f, 0.0f}, .Size = {1.0f, 1.0f}}, vec4(1.0f, 0.85f, 0.55f, 1.0f),
                 Gui::CornerRadii::All(6.0f));
    list.Text({66.0f, 118.0f}, font, "AVA", 30.0f, vec4(0.95f, 0.95f, 0.98f, 1.0f));
    list.PopTransform();

    const Unique<GuiScenePass> pass = GuiScenePass::Create({
        .Context = Context,
        .Assets = assets,
        .Extent = Extent,
        .OutputFormat = Format::RGBA16Sfloat,
    });
    pass->SetDrawList(list);
    Context.ImmediateCommands([&](CommandBuffer& cmd) { pass->Render(cmd, sceneView); });

    const vector<u8> raw = pass->GetOutput()->GetImage()->Download();
    REQUIRE(raw.size() == static_cast<usize>(Extent.x) * Extent.y * 8);
    const vector<u8> actual = DecodeHalfRgb(raw, Extent);

    Context.GetBindlessRegistry().Release(samplerHandle);
    Context.GetBindlessRegistry().Release(rampHandle);
    Context.GetBindlessRegistry().Release(checker.Handle);

    if (const char* dump = std::getenv("VENG_GUI_ROTATED_GOLDEN_DUMP"))
    {
        WritePpm(path(dump), actual, Extent);
        MESSAGE("gui rotation golden: wrote capture to ", dump);
        std::filesystem::remove(outArchive);
        return;
    }

    const path golden = path(GUI_GOLDEN_DIR) / "gui_rotated.png";
    int gw = 0;
    int gh = 0;
    int gc = 0;
    u8* goldenPixels = stbi_load(golden.string().c_str(), &gw, &gh, &gc, 3);
    REQUIRE_MESSAGE(goldenPixels != nullptr, "gui rotation golden: failed to load ",
                    golden.string());
    REQUIRE(static_cast<u32>(gw) == Extent.x);
    REQUIRE(static_cast<u32>(gh) == Extent.y);

    const long pixelCount = static_cast<long>(Extent.x) * Extent.y;
    long mismatched = 0;
    int worst = 0;
    for (long i = 0; i < pixelCount; ++i)
    {
        int pixelDelta = 0;
        for (int c = 0; c < 3; ++c)
        {
            const int a = actual[i * 3 + c];
            const int g = goldenPixels[i * 3 + c];
            const int d = a > g ? a - g : g - a;
            pixelDelta = d > pixelDelta ? d : pixelDelta;
        }
        worst = pixelDelta > worst ? pixelDelta : worst;
        if (pixelDelta > MaxChannelDelta)
        {
            ++mismatched;
        }
    }
    stbi_image_free(goldenPixels);

    const double fraction = static_cast<double>(mismatched) / static_cast<double>(pixelCount);
    MESSAGE("gui rotation golden: ", mismatched, "/", pixelCount, " pixels exceed delta ",
            MaxChannelDelta, " (worst ", worst, ")");
    CHECK(fraction <= MaxMismatchFraction);

    std::filesystem::remove(outArchive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "gui image golden: an authored <Image> renders every fill shape into its "
                  "content box")
{
    // Cook a UI document of five <Image src=…> elements — a rounded, bordered one (the texture
    // fills the *content* box, inside the border), an unsized one (laid out at its texture's own
    // pixels by the intrinsic measure), an `object-fit: contain` one letterboxed in a wide box, an
    // `image-repeat: tile` one, and an `image-slice` frame — instantiate + solve + build them,
    // render through GuiScenePass, and pin the composite.
    const path fixtureDir = path(GPU_COOKER_FIXTURE_DIR);
    const path packJson = fixtureDir / "ui_image_pack.json";
    const path outArchive = Veng::TestSupport::TempDir() / "veng_gpu_ui_image.vengpack";

    Cook::Cooker cooker;
    Cook::RegisterBuiltinImporters(cooker);
    REQUIRE(cooker.CookPack(packJson, outArchive).has_value());

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(outArchive).has_value());

    const AssetResult<AssetHandle<Gui::UIDocument>> recipe =
        assets.LoadSync<Gui::UIDocument>(AssetId{0x2D5E729A12F26978ULL});
    REQUIRE_MESSAGE(recipe.has_value(),
                    "load failed: ", recipe ? "" : recipe.error().Detail.c_str());
    REQUIRE(recipe->IsLoaded());

    const Unique<Gui::Document> document = Gui::Document::Instantiate(*recipe->Get(), assets);
    REQUIRE(document != nullptr);

    // A solid scene output to composite the UI over.
    const Ref<Image> sceneImage =
        Image::Create(Context, {
                                   .Name = "Gui Image Scene",
                                   .Extent = {Extent.x, Extent.y, 1},
                                   .Format = Format::RGBA16Sfloat,
                                   .Usage = ImageUsage::ColorAttachment | ImageUsage::Sampled |
                                            ImageUsage::TransferSrc,
                               });
    const Ref<ImageView> sceneView =
        ImageView::Create(Context, {.Name = "Gui Image Scene View", .Image = sceneImage});
    ClearImage(Context, sceneView, ClearColor{.R = 0.10f, .G = 0.12f, .B = 0.16f, .A = 1.0f});

    document->Solve(vec2(static_cast<f32>(Extent.x), static_cast<f32>(Extent.y)));
    Gui::DrawList list;
    document->Build(list);

    const Unique<GuiScenePass> pass = GuiScenePass::Create({
        .Context = Context,
        .Assets = assets,
        .Extent = Extent,
        .OutputFormat = Format::RGBA16Sfloat,
    });
    pass->SetDrawList(list);
    Context.ImmediateCommands([&](CommandBuffer& cmd) { pass->Render(cmd, sceneView); });

    const vector<u8> raw = pass->GetOutput()->GetImage()->Download();
    REQUIRE(raw.size() == static_cast<usize>(Extent.x) * Extent.y * 8);
    const vector<u8> actual = DecodeHalfRgb(raw, Extent);

    if (const char* dump = std::getenv("VENG_GUI_IMAGE_GOLDEN_DUMP"))
    {
        WritePpm(path(dump), actual, Extent);
        MESSAGE("gui image golden: wrote capture to ", dump);
        std::filesystem::remove(outArchive);
        return;
    }

    const path golden = path(GUI_GOLDEN_DIR) / "gui_image.png";
    int gw = 0;
    int gh = 0;
    int gc = 0;
    u8* goldenPixels = stbi_load(golden.string().c_str(), &gw, &gh, &gc, 3);
    REQUIRE_MESSAGE(goldenPixels != nullptr, "gui image golden: failed to load ", golden.string());
    REQUIRE(static_cast<u32>(gw) == Extent.x);
    REQUIRE(static_cast<u32>(gh) == Extent.y);

    const long pixelCount = static_cast<long>(Extent.x) * Extent.y;
    long mismatched = 0;
    int worst = 0;
    for (long i = 0; i < pixelCount; ++i)
    {
        int pixelDelta = 0;
        for (int c = 0; c < 3; ++c)
        {
            const int a = actual[i * 3 + c];
            const int g = goldenPixels[i * 3 + c];
            const int d = a > g ? a - g : g - a;
            pixelDelta = d > pixelDelta ? d : pixelDelta;
        }
        worst = pixelDelta > worst ? pixelDelta : worst;
        if (pixelDelta > MaxChannelDelta)
        {
            ++mismatched;
        }
    }
    stbi_image_free(goldenPixels);

    const double fraction = static_cast<double>(mismatched) / static_cast<double>(pixelCount);
    MESSAGE("gui image golden: ", mismatched, "/", pixelCount, " pixels exceed delta ",
            MaxChannelDelta, " (worst ", worst, ")");
    CHECK(fraction <= MaxMismatchFraction);

    std::filesystem::remove(outArchive);
}

TEST_CASE_FIXTURE(
    Veng::Test::GpuFixture,
    "gui background golden: styled background-image fills slice, tile, and fit their boxes")
{
    // Cook a UI document whose panels carry `background-image` in three modes — a nine-slice frame
    // at two widths (proving the corners keep their source size while the edges and center
    // stretch), a tiled fill, and an aspect-fitted one — instantiate + solve + build it, render
    // through GuiScenePass, and pin the composite. The tiled panel is styled from a *stylesheet*
    // rule while the rest are inline, so the capture covers both residency paths a background
    // texture rides: the sheet loader's and the document loader's inline-style decode.
    const path fixtureDir = path(GPU_COOKER_FIXTURE_DIR);
    const path packJson = fixtureDir / "ui_background_pack.json";
    const path outArchive = Veng::TestSupport::TempDir() / "veng_gpu_ui_background.vengpack";

    Cook::Cooker cooker;
    Cook::RegisterBuiltinImporters(cooker);
    REQUIRE(cooker.CookPack(packJson, outArchive).has_value());

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(outArchive).has_value());

    const AssetResult<AssetHandle<Gui::UIDocument>> recipe =
        assets.LoadSync<Gui::UIDocument>(AssetId{0xE4D34F00C43EC714ULL});
    REQUIRE_MESSAGE(recipe.has_value(),
                    "load failed: ", recipe ? "" : recipe.error().Detail.c_str());
    REQUIRE(recipe->IsLoaded());

    const Unique<Gui::Document> document = Gui::Document::Instantiate(*recipe->Get(), assets);
    REQUIRE(document != nullptr);

    const Ref<Image> sceneImage =
        Image::Create(Context, {
                                   .Name = "Gui Background Scene",
                                   .Extent = {Extent.x, Extent.y, 1},
                                   .Format = Format::RGBA16Sfloat,
                                   .Usage = ImageUsage::ColorAttachment | ImageUsage::Sampled |
                                            ImageUsage::TransferSrc,
                               });
    const Ref<ImageView> sceneView =
        ImageView::Create(Context, {.Name = "Gui Background Scene View", .Image = sceneImage});
    ClearImage(Context, sceneView, ClearColor{.R = 0.10f, .G = 0.12f, .B = 0.16f, .A = 1.0f});

    document->Solve(vec2(static_cast<f32>(Extent.x), static_cast<f32>(Extent.y)));
    Gui::DrawList list;
    document->Build(list);

    const Unique<GuiScenePass> pass = GuiScenePass::Create({
        .Context = Context,
        .Assets = assets,
        .Extent = Extent,
        .OutputFormat = Format::RGBA16Sfloat,
    });
    pass->SetDrawList(list);
    Context.ImmediateCommands([&](CommandBuffer& cmd) { pass->Render(cmd, sceneView); });

    const vector<u8> raw = pass->GetOutput()->GetImage()->Download();
    REQUIRE(raw.size() == static_cast<usize>(Extent.x) * Extent.y * 8);
    const vector<u8> actual = DecodeHalfRgb(raw, Extent);

    if (const char* dump = std::getenv("VENG_GUI_BACKGROUND_GOLDEN_DUMP"))
    {
        WritePpm(path(dump), actual, Extent);
        MESSAGE("gui background golden: wrote capture to ", dump);
        std::filesystem::remove(outArchive);
        return;
    }

    const path golden = path(GUI_GOLDEN_DIR) / "gui_background.png";
    int gw = 0;
    int gh = 0;
    int gc = 0;
    u8* goldenPixels = stbi_load(golden.string().c_str(), &gw, &gh, &gc, 3);
    REQUIRE_MESSAGE(goldenPixels != nullptr, "gui background golden: failed to load ",
                    golden.string());
    REQUIRE(static_cast<u32>(gw) == Extent.x);
    REQUIRE(static_cast<u32>(gh) == Extent.y);

    const long pixelCount = static_cast<long>(Extent.x) * Extent.y;
    long mismatched = 0;
    int worst = 0;
    for (long i = 0; i < pixelCount; ++i)
    {
        int pixelDelta = 0;
        for (int c = 0; c < 3; ++c)
        {
            const int a = actual[i * 3 + c];
            const int g = goldenPixels[i * 3 + c];
            const int d = a > g ? a - g : g - a;
            pixelDelta = d > pixelDelta ? d : pixelDelta;
        }
        worst = pixelDelta > worst ? pixelDelta : worst;
        if (pixelDelta > MaxChannelDelta)
        {
            ++mismatched;
        }
    }
    stbi_image_free(goldenPixels);

    const double fraction = static_cast<double>(mismatched) / static_cast<double>(pixelCount);
    MESSAGE("gui background golden: ", mismatched, "/", pixelCount, " pixels exceed delta ",
            MaxChannelDelta, " (worst ", worst, ")");
    CHECK(fraction <= MaxMismatchFraction);

    std::filesystem::remove(outArchive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "gui shadow golden: box-shadow drops, spreads, hardens, and insets")
{
    // Cook a UI document whose panels carry `box-shadow` across the axes the effect has — two blur
    // radii at the same offset, a zero-blur hard shadow with a positive spread, an inset shadow,
    // and a spread-only glow with no offset — instantiate + solve + build it, render through
    // GuiScenePass, and pin the composite. The inset card is styled from a *stylesheet* rule while
    // the rest are inline, so the capture covers both authoring paths a shadow rides.
    const path fixtureDir = path(GPU_COOKER_FIXTURE_DIR);
    const path packJson = fixtureDir / "ui_shadow_pack.json";
    const path outArchive = Veng::TestSupport::TempDir() / "veng_gpu_ui_shadow.vengpack";

    Cook::Cooker cooker;
    Cook::RegisterBuiltinImporters(cooker);
    REQUIRE(cooker.CookPack(packJson, outArchive).has_value());

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(outArchive).has_value());

    const AssetResult<AssetHandle<Gui::UIDocument>> recipe =
        assets.LoadSync<Gui::UIDocument>(AssetId{0x35F48CAF87C31796ULL});
    REQUIRE_MESSAGE(recipe.has_value(),
                    "load failed: ", recipe ? "" : recipe.error().Detail.c_str());
    REQUIRE(recipe->IsLoaded());

    const Unique<Gui::Document> document = Gui::Document::Instantiate(*recipe->Get(), assets);
    REQUIRE(document != nullptr);

    const Ref<Image> sceneImage =
        Image::Create(Context, {
                                   .Name = "Gui Shadow Scene",
                                   .Extent = {Extent.x, Extent.y, 1},
                                   .Format = Format::RGBA16Sfloat,
                                   .Usage = ImageUsage::ColorAttachment | ImageUsage::Sampled |
                                            ImageUsage::TransferSrc,
                               });
    const Ref<ImageView> sceneView =
        ImageView::Create(Context, {.Name = "Gui Shadow Scene View", .Image = sceneImage});
    ClearImage(Context, sceneView, ClearColor{.R = 0.10f, .G = 0.12f, .B = 0.16f, .A = 1.0f});

    document->Solve(vec2(static_cast<f32>(Extent.x), static_cast<f32>(Extent.y)));
    Gui::DrawList list;
    document->Build(list);

    const Unique<GuiScenePass> pass = GuiScenePass::Create({
        .Context = Context,
        .Assets = assets,
        .Extent = Extent,
        .OutputFormat = Format::RGBA16Sfloat,
    });
    pass->SetDrawList(list);
    Context.ImmediateCommands([&](CommandBuffer& cmd) { pass->Render(cmd, sceneView); });

    const vector<u8> raw = pass->GetOutput()->GetImage()->Download();
    REQUIRE(raw.size() == static_cast<usize>(Extent.x) * Extent.y * 8);
    const vector<u8> actual = DecodeHalfRgb(raw, Extent);

    if (const char* dump = std::getenv("VENG_GUI_SHADOW_GOLDEN_DUMP"))
    {
        WritePpm(path(dump), actual, Extent);
        MESSAGE("gui shadow golden: wrote capture to ", dump);
        std::filesystem::remove(outArchive);
        return;
    }

    const path golden = path(GUI_GOLDEN_DIR) / "gui_shadow.png";
    int gw = 0;
    int gh = 0;
    int gc = 0;
    u8* goldenPixels = stbi_load(golden.string().c_str(), &gw, &gh, &gc, 3);
    REQUIRE_MESSAGE(goldenPixels != nullptr, "gui shadow golden: failed to load ", golden.string());
    REQUIRE(static_cast<u32>(gw) == Extent.x);
    REQUIRE(static_cast<u32>(gh) == Extent.y);

    const long pixelCount = static_cast<long>(Extent.x) * Extent.y;
    long mismatched = 0;
    int worst = 0;
    for (long i = 0; i < pixelCount; ++i)
    {
        int pixelDelta = 0;
        for (int c = 0; c < 3; ++c)
        {
            const int a = actual[i * 3 + c];
            const int g = goldenPixels[i * 3 + c];
            const int d = a > g ? a - g : g - a;
            pixelDelta = d > pixelDelta ? d : pixelDelta;
        }
        worst = pixelDelta > worst ? pixelDelta : worst;
        if (pixelDelta > MaxChannelDelta)
        {
            ++mismatched;
        }
    }
    stbi_image_free(goldenPixels);

    const double fraction = static_cast<double>(mismatched) / static_cast<double>(pixelCount);
    MESSAGE("gui shadow golden: ", mismatched, "/", pixelCount, " pixels exceed delta ",
            MaxChannelDelta, " (worst ", worst, ")");
    CHECK(fraction <= MaxMismatchFraction);

    std::filesystem::remove(outArchive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "gui material golden: a GuiFill material fills a panel and shades an Image")
{
    // Cook a UI document driven by two authored GuiFill materials — a procedural sweep filling
    // panel backgrounds (plain, bordered, faded, and heavily rounded) and a second material shading
    // an <Image>'s own texture through the two conventional handle fields — instantiate + solve +
    // build it, render through GuiScenePass, and pin the composite. One background is styled from a
    // *stylesheet* rule while the rest are inline, so the capture covers both residency paths a UI
    // material rides. The pass clock is left at its 0 default, so the animated sweep is captured at
    // a fixed time and the golden is reproducible.
    const path fixtureDir = path(GPU_COOKER_FIXTURE_DIR);
    const path packJson = fixtureDir / "ui_material_pack.json";
    const path outArchive = Veng::TestSupport::TempDir() / "veng_gpu_ui_material.vengpack";
    const std::array<path, 1> references{path(VENG_CORE_PACK_JSON)};

    Cook::Cooker cooker;
    Cook::RegisterBuiltinImporters(cooker);
    const VoidResult cooked = cooker.CookPack(packJson, outArchive, references, nullptr, nullptr,
                                              nullptr, nullptr, {}, path(VENG_CORE_SHADER_DIR));
    REQUIRE_MESSAGE(cooked.has_value(), cooked.error());

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(outArchive).has_value());

    const AssetResult<AssetHandle<Gui::UIDocument>> recipe =
        assets.LoadSync<Gui::UIDocument>(AssetId{0xED64D6867A7A6259ULL});
    const string loadError = recipe.has_value() ? string{} : recipe.error().Detail;
    REQUIRE_MESSAGE(recipe.has_value(), loadError);
    REQUIRE(recipe->IsLoaded());

    const Unique<Gui::Document> document = Gui::Document::Instantiate(*recipe->Get(), assets);
    REQUIRE(document != nullptr);

    const Ref<Image> sceneImage =
        Image::Create(Context, {
                                   .Name = "Gui Material Scene",
                                   .Extent = {Extent.x, Extent.y, 1},
                                   .Format = Format::RGBA16Sfloat,
                                   .Usage = ImageUsage::ColorAttachment | ImageUsage::Sampled |
                                            ImageUsage::TransferSrc,
                               });
    const Ref<ImageView> sceneView =
        ImageView::Create(Context, {.Name = "Gui Material Scene View", .Image = sceneImage});
    ClearImage(Context, sceneView, ClearColor{.R = 0.10f, .G = 0.12f, .B = 0.16f, .A = 1.0f});

    document->Solve(vec2(static_cast<f32>(Extent.x), static_cast<f32>(Extent.y)));
    Gui::DrawList list;
    document->Build(list);

    // The batching cost the fill source imposes, and why the pass's rebind guard cannot key on the
    // run kind alone: the five material fills collapse to four runs — the first two share a
    // material *and* adjacency so they merge, while the bordered one's border quad, the second
    // material, and the trailing panel each break the run.
    const auto materialRuns =
        std::ranges::count_if(list.GetRuns(), [](const Gui::DrawRun& run)
                              { return run.Pipeline == Gui::GuiPipeline::Material; });
    CHECK(materialRuns == 4);

    const Unique<GuiScenePass> pass = GuiScenePass::Create({
        .Context = Context,
        .Assets = assets,
        .Extent = Extent,
        .OutputFormat = Format::RGBA16Sfloat,
    });
    pass->SetDrawList(list);
    Context.ImmediateCommands([&](CommandBuffer& cmd) { pass->Render(cmd, sceneView); });

    const vector<u8> raw = pass->GetOutput()->GetImage()->Download();
    REQUIRE(raw.size() == static_cast<usize>(Extent.x) * Extent.y * 8);
    const vector<u8> actual = DecodeHalfRgb(raw, Extent);

    if (const char* dump = std::getenv("VENG_GUI_MATERIAL_GOLDEN_DUMP"))
    {
        WritePpm(path(dump), actual, Extent);
        MESSAGE("gui material golden: wrote capture to ", dump);
        std::filesystem::remove(outArchive);
        return;
    }

    const path golden = path(GUI_GOLDEN_DIR) / "gui_material.png";
    int gw = 0;
    int gh = 0;
    int gc = 0;
    u8* goldenPixels = stbi_load(golden.string().c_str(), &gw, &gh, &gc, 3);
    REQUIRE_MESSAGE(goldenPixels != nullptr, "gui material golden: failed to load ",
                    golden.string());
    REQUIRE(static_cast<u32>(gw) == Extent.x);
    REQUIRE(static_cast<u32>(gh) == Extent.y);

    const long pixelCount = static_cast<long>(Extent.x) * Extent.y;
    long mismatched = 0;
    int worst = 0;
    for (long i = 0; i < pixelCount; ++i)
    {
        int pixelDelta = 0;
        for (int c = 0; c < 3; ++c)
        {
            const int a = actual[i * 3 + c];
            const int g = goldenPixels[i * 3 + c];
            const int d = a > g ? a - g : g - a;
            pixelDelta = d > pixelDelta ? d : pixelDelta;
        }
        worst = pixelDelta > worst ? pixelDelta : worst;
        if (pixelDelta > MaxChannelDelta)
        {
            ++mismatched;
        }
    }
    stbi_image_free(goldenPixels);

    const double fraction = static_cast<double>(mismatched) / static_cast<double>(pixelCount);
    MESSAGE("gui material golden: ", mismatched, "/", pixelCount, " pixels exceed delta ",
            MaxChannelDelta, " (worst ", worst, ")");
    CHECK(fraction <= MaxMismatchFraction);

    std::filesystem::remove(outArchive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "gui popup golden: a popup escapes its ancestor's clip and paints over the tree")
{
    // The document popup layer's load-bearing capture. The tree is built imperatively (panels
    // only, no font), so the case needs no cook fixture: a clipping panel whose overflowing child
    // is visibly cut at its box, a wide banner painted after it in the main tree, and a popup
    // anchored to the row inside the clipping panel. The popup lays out against the document
    // extent, so it spills past the clip that bounds its anchor's siblings, and it is built after
    // the whole main tree with the scissor stack reset, so it covers the banner it overlaps.
    Gui::Document document;

    Gui::Style root;
    root.Direction = Gui::FlexDirection::Column;
    root.Padding = Gui::Insets::All(20.0f);
    document.SetStyle(document.Root(), root);

    Gui::Style clipped;
    clipped.Width = Gui::Length::Points(100.0f);
    clipped.Height = Gui::Length::Points(60.0f);
    clipped.FlexShrink = 0.0f;
    clipped.OverflowY = Gui::Overflow::Hidden;
    clipped.Background = vec4(0.09f, 0.13f, 0.20f, 1.0f);
    Gui::Element& view = document.Add(document.Root(), Gui::ElementKind::Panel);
    document.SetStyle(view, clipped);

    Gui::Style rowStyle;
    rowStyle.Height = Gui::Length::Points(18.0f);
    rowStyle.FlexShrink = 0.0f;
    rowStyle.Background = vec4(0.31f, 0.639f, 1.0f, 1.0f);
    Gui::Element& row = document.Add(view, Gui::ElementKind::Panel);
    document.SetStyle(row, rowStyle);

    // Tall enough to overflow the clipping panel: the cut edge is what the popup's spill is read
    // against.
    Gui::Style tail;
    tail.Height = Gui::Length::Points(140.0f);
    tail.FlexShrink = 0.0f;
    tail.Background = vec4(0.88f, 0.54f, 0.23f, 1.0f);
    Gui::Element& overflowing = document.Add(view, Gui::ElementKind::Panel);
    document.SetStyle(overflowing, tail);

    Gui::Style banner;
    banner.Height = Gui::Length::Points(80.0f);
    banner.FlexShrink = 0.0f;
    banner.Background = vec4(0.18f, 0.55f, 0.35f, 1.0f);
    Gui::Element& covered = document.Add(document.Root(), Gui::ElementKind::Panel);
    document.SetStyle(covered, banner);

    const Gui::PopupId popup = document.OpenPopup(
        row, Gui::PopupOptions{.Side = Gui::PopupSide::Below, .Offset = vec2(64.0f, 4.0f)});
    Gui::Style menu;
    menu.Direction = Gui::FlexDirection::Column;
    menu.Width = Gui::Length::Points(150.0f);
    menu.Padding = Gui::Insets::All(6.0f);
    menu.Background = vec4(0.95f, 0.95f, 0.97f, 1.0f);
    menu.Radii = Gui::CornerRadii::All(8.0f);
    menu.BorderStyle = Gui::Border{.Width = 2.0f, .Color = vec4(0.31f, 0.639f, 1.0f, 1.0f)};
    menu.Shadow = Gui::BoxShadow{
        .Offset = vec2(0.0f, 4.0f), .Blur = 10.0f, .Color = vec4(0.0f, 0.0f, 0.0f, 0.7f)};
    Gui::Element* const menuRoot = document.GetPopupRoot(popup);
    REQUIRE(menuRoot != nullptr);
    document.SetStyle(*menuRoot, menu);

    Gui::Style item;
    item.Height = Gui::Length::Points(20.0f);
    item.FlexShrink = 0.0f;
    item.Radii = Gui::CornerRadii::All(3.0f);
    item.Margin = Gui::Insets{.Left = 0.0f, .Top = 0.0f, .Right = 0.0f, .Bottom = 4.0f};
    for (int i = 0; i < 3; ++i)
    {
        Gui::Element& option = document.Add(*menuRoot, Gui::ElementKind::Panel);
        item.Background = vec4(0.22f + 0.2f * static_cast<f32>(i), 0.26f, 0.34f, 1.0f);
        document.SetStyle(option, item);
    }

    const Ref<Image> sceneImage =
        Image::Create(Context, {
                                   .Name = "Gui Popup Scene",
                                   .Extent = {Extent.x, Extent.y, 1},
                                   .Format = Format::RGBA16Sfloat,
                                   .Usage = ImageUsage::ColorAttachment | ImageUsage::Sampled |
                                            ImageUsage::TransferSrc,
                               });
    const Ref<ImageView> sceneView =
        ImageView::Create(Context, {.Name = "Gui Popup Scene View", .Image = sceneImage});
    ClearImage(Context, sceneView, ClearColor{.R = 0.10f, .G = 0.12f, .B = 0.16f, .A = 1.0f});

    document.Solve(vec2(static_cast<f32>(Extent.x), static_cast<f32>(Extent.y)));
    Gui::DrawList list;
    document.Build(list);

    AssetManager assets(Context, Tasks, Types);
    const Unique<GuiScenePass> pass = GuiScenePass::Create({
        .Context = Context,
        .Assets = assets,
        .Extent = Extent,
        .OutputFormat = Format::RGBA16Sfloat,
    });
    pass->SetDrawList(list);
    Context.ImmediateCommands([&](CommandBuffer& cmd) { pass->Render(cmd, sceneView); });

    const vector<u8> raw = pass->GetOutput()->GetImage()->Download();
    REQUIRE(raw.size() == static_cast<usize>(Extent.x) * Extent.y * 8);
    const vector<u8> actual = DecodeHalfRgb(raw, Extent);

    if (const char* dump = std::getenv("VENG_GUI_POPUP_GOLDEN_DUMP"))
    {
        WritePpm(path(dump), actual, Extent);
        MESSAGE("gui popup golden: wrote capture to ", dump);
        return;
    }

    const path golden = path(GUI_GOLDEN_DIR) / "gui_popup.png";
    int gw = 0;
    int gh = 0;
    int gc = 0;
    u8* goldenPixels = stbi_load(golden.string().c_str(), &gw, &gh, &gc, 3);
    REQUIRE_MESSAGE(goldenPixels != nullptr, "gui popup golden: failed to load ", golden.string());
    REQUIRE(static_cast<u32>(gw) == Extent.x);
    REQUIRE(static_cast<u32>(gh) == Extent.y);

    const long pixelCount = static_cast<long>(Extent.x) * Extent.y;
    long mismatched = 0;
    int worst = 0;
    for (long i = 0; i < pixelCount; ++i)
    {
        int pixelDelta = 0;
        for (int c = 0; c < 3; ++c)
        {
            const int a = actual[i * 3 + c];
            const int g = goldenPixels[i * 3 + c];
            const int d = a > g ? a - g : g - a;
            pixelDelta = d > pixelDelta ? d : pixelDelta;
        }
        worst = pixelDelta > worst ? pixelDelta : worst;
        if (pixelDelta > MaxChannelDelta)
        {
            ++mismatched;
        }
    }
    stbi_image_free(goldenPixels);

    const double fraction = static_cast<double>(mismatched) / static_cast<double>(pixelCount);
    MESSAGE("gui popup golden: ", mismatched, "/", pixelCount, " pixels exceed delta ",
            MaxChannelDelta, " (worst ", worst, ")");
    CHECK(fraction <= MaxMismatchFraction);
}
