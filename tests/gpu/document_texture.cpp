// DocumentTexture — the world-space presenter that renders a document into an owned HDR target for a
// material to sample. Drives an imperative document into the texture and asserts:
//   - RenderToTarget records the document and its >1.0 background survives into the HDR target;
//   - the dirty-gate skips an idle frame (nothing changed), and GetOutputHandle is stable across the
//     skip (the target is not reallocated);
//   - a resolution change re-allocates the target (new extent) and re-records unconditionally.

#include <doctest/doctest.h>

#include <glm/gtc/packing.hpp>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Gui/Document.h>
#include <Veng/Gui/DocumentTexture.h>
#include <Veng/Gui/RenderTarget.h>
#include <Veng/Gui/Style.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>

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
