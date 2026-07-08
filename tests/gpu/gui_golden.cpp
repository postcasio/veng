// Gui render golden: builds a hand-authored DrawList — a rounded panel, a 9-slice frame,
// a tinted texture, and a line of text at two pixel sizes — renders it through GuiScenePass
// over a solid scene output, downloads the composited result, and fuzzy-compares it to a
// committed golden PNG. The image floor every later Gui plan holds stable (02-08 change what
// builds the draw list, never the pixels a given draw list produces).
//
// Set VENG_GUI_GOLDEN_DUMP=<path.ppm> to write the capture instead of comparing — the way the
// golden is (re)generated: dump, sips the PPM to tests/golden/gui_overlay.png, commit.

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

TEST_CASE_FIXTURE(
    Veng::Test::GpuFixture,
    "gui image golden: an authored <Image> renders its cooked texture with corner-radius + border")
{
    // Cook a UI document whose <Image src=…> names a cooked texture and styles a corner-radius +
    // border, instantiate + solve + build it, render through GuiScenePass, and pin the composite —
    // the texture fills the element's box, rounded and framed by the border.
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
