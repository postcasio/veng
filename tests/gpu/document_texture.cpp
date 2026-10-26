// DocumentTexture — the world-space presenter that renders a document into an owned HDR target for a
// material to sample. Drives an imperative document into the texture and asserts:
//   - RenderToTarget records the document and its >1.0 background survives into the HDR target;
//   - the dirty-gate skips an idle frame (nothing changed), and GetOutputHandle is stable across the
//     skip (the target is not reallocated);
//   - a resolution change re-allocates the target (new extent) and re-records unconditionally;
//   - a premultiplied Image composites to the same pixel as the straight Image of the same subject.

#include <array>
#include <span>

#include <doctest/doctest.h>

#include <glm/gtc/packing.hpp>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Gui/Document.h>
#include <Veng/Gui/DocumentTexture.h>
#include <Veng/Gui/RenderTarget.h>
#include <Veng/Gui/Style.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/Sampler.h>

#include <gpu/fixture.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    constexpr uvec2 Extent{64, 64};

    // The document's bright fill: HDR cyan whose green/blue components are 4.0 — above the 1.0 an
    // 8-bit target would clamp to, so its survival proves the half-float target round-trip.
    constexpr vec4 Glow{0.0f, 4.0f, 4.0f, 1.0f};

    // Builds an imperative document filling `size` with the glowing background.
    Unique<Gui::Document> BuildGlowDocument(uvec2 size)
    {
        auto document = CreateUnique<Gui::Document>();
        Gui::Style root;
        root.Width = Gui::Length::Points(static_cast<f32>(size.x));
        root.Height = Gui::Length::Points(static_cast<f32>(size.y));
        root.Background = Glow;
        document->SetStyle(document->Root(), root);
        return document;
    }

    // Decodes one RGBA16Sfloat texel to a linear vec4.
    vec4 DecodeTexel(const vector<u8>& rgba16f, u32 width, u32 x, u32 y)
    {
        const auto* halves = reinterpret_cast<const u16*>(rgba16f.data());
        const usize base = (static_cast<usize>(y) * width + x) * 4;
        return vec4(glm::unpackHalf1x16(halves[base + 0]), glm::unpackHalf1x16(halves[base + 1]),
                    glm::unpackHalf1x16(halves[base + 2]), glm::unpackHalf1x16(halves[base + 3]));
    }
}

TEST_CASE_FIXTURE(
    Veng::Test::GpuFixture,
    "document texture: renders to target, dirty-gate skips an idle frame, handle stable")
{
    // The GuiScenePass gui shaders come from the auto-mounted core pack; no extra mount is needed.
    AssetManager assets(Context, Tasks, Types);

    const Unique<Gui::Document> document = BuildGlowDocument(Extent);

    Gui::DocumentTexture texture;

    // The first RenderToTarget materializes the target + pass and records the document.
    Context.ImmediateCommands(
        [&](CommandBuffer& cmd)
        { CHECK(texture.RenderToTarget(Context, assets, cmd, *document, Extent, 1.0f, 0.016f)); });
    CHECK(texture.WasRenderedLastDrive());
    REQUIRE(texture.GetTarget() != nullptr);
    CHECK(texture.GetTarget()->GetExtent() == Extent);

    const Renderer::TextureHandle firstHandle = texture.GetOutputHandle();
    CHECK(firstHandle.IsValid());

    // The document's >1.0 radiance survived into the HDR target — the value a material would sample.
    const vector<u8> targetPixels = texture.GetTarget()->GetOutput()->GetImage()->Download();
    const vec4 center = DecodeTexel(targetPixels, Extent.x, Extent.x / 2, Extent.y / 2);
    CHECK(center.g > 1.0f);
    CHECK(center.b > 1.0f);

    // A second, unchanged frame is idle — no binding moved, no transition, no resolution change — so
    // the dirty-gate skips the re-record and keeps the persistent target content.
    Context.ImmediateCommands(
        [&](CommandBuffer& cmd)
        {
            CHECK_FALSE(
                texture.RenderToTarget(Context, assets, cmd, *document, Extent, 1.0f, 0.016f));
        });
    CHECK_FALSE(texture.WasRenderedLastDrive());
    // The target was not reallocated across the skip, so the output handle is stable.
    CHECK(texture.GetOutputHandle().Index == firstHandle.Index);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "document texture: a resolution change re-allocates the target and re-renders")
{
    AssetManager assets(Context, Tasks, Types);

    const Unique<Gui::Document> document = BuildGlowDocument(Extent);
    Gui::DocumentTexture texture;

    Context.ImmediateCommands(
        [&](CommandBuffer& cmd)
        { texture.RenderToTarget(Context, assets, cmd, *document, Extent, 1.0f, 0.016f); });
    REQUIRE(texture.GetTarget() != nullptr);
    CHECK(texture.GetTarget()->GetExtent() == Extent);

    // A larger resolution re-sizes the HDR target and re-records unconditionally, even though the
    // document itself did not change.
    constexpr uvec2 Larger{96, 80};
    Context.ImmediateCommands(
        [&](CommandBuffer& cmd)
        { CHECK(texture.RenderToTarget(Context, assets, cmd, *document, Larger, 1.0f, 0.016f)); });
    CHECK(texture.WasRenderedLastDrive());
    CHECK(texture.GetTarget()->GetExtent() == Larger);
    CHECK(texture.GetOutputHandle().IsValid());
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "document texture: a premultiplied image composites as the straight image of the "
                  "same subject does")
{
    AssetManager assets(Context, Tasks, Types);
    BindlessRegistry& bindless = Context.GetBindlessRegistry();

    // One texel of the same subject twice: straight (colour, half coverage), and that colour
    // already multiplied by its coverage.
    const vec4 straight{0.8f, 0.4f, 0.2f, 0.5f};
    const vec4 premultiplied{straight.r * straight.a, straight.g * straight.a,
                             straight.b * straight.a, straight.a};
    const auto MakeTexel = [&](const vec4 texel)
    {
        const Ref<Image> image =
            Image::Create(Context, {.Name = "Alpha Mode Texel",
                                    .Extent = {1, 1, 1},
                                    .Format = Format::RGBA16Sfloat,
                                    .Usage = ImageUsage::Sampled | ImageUsage::TransferDst});
        const std::array<u16, 4> halves = {glm::packHalf1x16(texel.r), glm::packHalf1x16(texel.g),
                                           glm::packHalf1x16(texel.b), glm::packHalf1x16(texel.a)};
        image->UploadSync(
            std::span<const u8>(reinterpret_cast<const u8*>(halves.data()), sizeof(halves)));
        return ImageView::Create(Context, {.Name = "Alpha Mode Texel View", .Image = image});
    };
    const Ref<ImageView> straightView = MakeTexel(straight);
    const Ref<ImageView> premultipliedView = MakeTexel(premultiplied);
    const TextureHandle straightHandle = bindless.Register(straightView);
    const TextureHandle premultipliedHandle = bindless.Register(premultipliedView);
    const SamplerHandle sampler = bindless
                                      .AcquireSampler({.Name = "Alpha Mode Sampler",
                                                       .MagFilter = Filter::Nearest,
                                                       .MinFilter = Filter::Nearest,
                                                       .AddressModeU = AddressMode::ClampToEdge,
                                                       .AddressModeV = AddressMode::ClampToEdge,
                                                       .AddressModeW = AddressMode::ClampToEdge})
                                      .Handle;

    // Two images side by side over an opaque ground.
    const vec4 ground{0.2f, 0.3f, 0.4f, 1.0f};
    Gui::Document document;
    Gui::Style root;
    root.Width = Gui::Length::Points(static_cast<f32>(Extent.x));
    root.Height = Gui::Length::Points(static_cast<f32>(Extent.y));
    root.Background = ground;
    document.SetStyle(document.Root(), root);
    const auto AddImage =
        [&](const f32 left, const TextureHandle texture, const Gui::ImageAlphaMode alpha)
    {
        Gui::Element& image = document.Add(document.Root(), Gui::ElementKind::Image);
        Gui::Style style;
        style.Position = Gui::PositionType::Absolute;
        style.Inset.Left = left;
        style.Inset.Top = 0.0f;
        style.Width = Gui::Length::Points(static_cast<f32>(Extent.x) * 0.5f);
        style.Height = Gui::Length::Points(static_cast<f32>(Extent.y));
        document.SetStyle(image, style);
        document.SetImageTexture(image, texture, sampler, uvec2(1, 1), alpha);
    };
    AddImage(0.0f, straightHandle, Gui::ImageAlphaMode::Straight);
    AddImage(static_cast<f32>(Extent.x) * 0.5f, premultipliedHandle,
             Gui::ImageAlphaMode::Premultiplied);

    Gui::DocumentTexture texture;
    Context.ImmediateCommands(
        [&](CommandBuffer& cmd)
        { texture.RenderToTarget(Context, assets, cmd, document, Extent, 1.0f, 0.016f); });
    REQUIRE(texture.GetTarget() != nullptr);
    const vector<u8> pixels = texture.GetTarget()->GetOutput()->GetImage()->Download();
    const vec4 left = DecodeTexel(pixels, Extent.x, Extent.x / 4, Extent.y / 2);
    const vec4 right = DecodeTexel(pixels, Extent.x, Extent.x * 3 / 4, Extent.y / 2);

    // Both are the subject over the ground, which the straight one is by construction.
    const vec3 expected = vec3(straight) * straight.a + vec3(ground) * (1.0f - straight.a);
    for (int channel = 0; channel < 3; ++channel)
    {
        CHECK(left[channel] == doctest::Approx(expected[channel]).epsilon(0.01));
        CHECK(right[channel] == doctest::Approx(left[channel]).epsilon(0.01));
    }

    bindless.Release(straightHandle);
    bindless.Release(premultipliedHandle);
}
