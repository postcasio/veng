// Image clear + format cases: clears off-screen images via the render graph
// at a non-trivial extent and in a second format.
//
//   1. A 64x32 RGBA8Unorm image cleared to an exactly-representable colour
//      (green, fully opaque) and downloaded/verified at that larger extent.
//   2. A 64x32 R8Unorm image cleared to 1.0 (byte value 255) and
//      downloaded/verified as a single-channel buffer.
//   3. A pass over part of two attachments clears the first over its render area alone and
//      leaves the loaded second untouched.

#include <algorithm>
#include <array>

#include <doctest/doctest.h>

#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/Types.h>

#include <gpu/fixture.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    constexpr u32 Width = 64;
    constexpr u32 Height = 32;

    // Clears `view` (size x extent x 1) through a one-pass render graph.
    void ClearImage(Context& context, const Ref<ImageView>& view, const ClearColor& clear)
    {
        context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                RenderGraph graph(context);
                const ResourceId target = graph.Import("Target");
                graph.AddPass("clear")
                    .Color({
                        .Resource = target,
                        .Load = LoadOp::Clear,
                        .Store = StoreOp::Store,
                        .Clear = clear,
                    })
                    .Execute([](PassContext&) {});
                const RenderGraph::ImportBinding binding{.Id = target, .View = view};
                graph.Compile()->Execute(cmd, {&binding, 1});
            });
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture, "image clear: 64x32 RGBA8Unorm cleared to green")
{
    // Exactly representable in RGBA8Unorm: green, fully opaque.
    constexpr std::array<u8, 4> expected = {0, 255, 0, 255};

    const auto image =
        Image::Create(Context, {
                                   .Name = "Clear Target RGBA8",
                                   .Extent = {Width, Height, 1},
                                   .Format = Format::RGBA8Unorm,
                                   .Usage = ImageUsage::ColorAttachment | ImageUsage::TransferSrc,
                               });

    const auto view =
        ImageView::Create(Context, {.Name = "Clear Target RGBA8 View", .Image = image});

    ClearImage(Context, view, ClearColor{.R = 0.0f, .G = 1.0f, .B = 0.0f, .A = 1.0f});

    const vector<u8> pixels = image->Download();

    REQUIRE(pixels.size() == static_cast<size_t>(Width) * Height * 4);
    CHECK(Test::PixelsMatch(pixels, expected));
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture, "image clear: 64x32 R8Unorm cleared to 255")
{
    constexpr u8 expected = 255;

    const auto image =
        Image::Create(Context, {
                                   .Name = "Clear Target R8",
                                   .Extent = {Width, Height, 1},
                                   .Format = Format::R8Unorm,
                                   .Usage = ImageUsage::ColorAttachment | ImageUsage::TransferSrc,
                               });

    const auto view = ImageView::Create(Context, {.Name = "Clear Target R8 View", .Image = image});

    // Only the red channel is meaningful for a single-channel format; the
    // other channels of ClearColor are ignored by the backend.
    ClearImage(Context, view, ClearColor{.R = 1.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f});

    const vector<u8> pixels = image->Download();

    REQUIRE(pixels.size() == static_cast<size_t>(Width) * Height);

    for (size_t p = 0; p < pixels.size(); p++)
    {
        CAPTURE(p);
        CHECK(pixels[p] == expected);
    }
}

TEST_CASE_FIXTURE(
    Veng::Test::GpuFixture,
    "image clear: a pass clearing part of its attachment clears its render area alone")
{
    const auto makeTarget = [this](const char* name, const Format format)
    {
        return Image::Create(Context,
                             {
                                 .Name = name,
                                 .Extent = {Width, Height, 1},
                                 .Format = format,
                                 .Usage = ImageUsage::ColorAttachment | ImageUsage::TransferSrc,
                             });
    };
    const Ref<Image> cleared = makeTarget("Area Clear Target", Format::RGBA8Unorm);
    const Ref<Image> loaded = makeTarget("Area Load Target", Format::R8Unorm);
    const Ref<ImageView> clearedView =
        ImageView::Create(Context, {.Name = "Area Clear Target View", .Image = cleared});
    const Ref<ImageView> loadedView =
        ImageView::Create(Context, {.Name = "Area Load Target View", .Image = loaded});
    ClearImage(Context, clearedView, ClearColor{.R = 0.0f, .G = 1.0f, .B = 0.0f, .A = 1.0f});
    ClearImage(Context, loadedView, ClearColor{.R = 1.0f});

    // The pass covers a 16x8 area: the first attachment is cleared there, the second only loaded.
    constexpr ivec2 AreaOffset{16, 8};
    constexpr uvec2 AreaExtent{16, 8};
    Context.ImmediateCommands(
        [&](CommandBuffer& cmd)
        {
            cmd.PrepareForAccess(clearedView, AccessKind::ColorAttachment);
            cmd.PrepareForAccess(loadedView, AccessKind::ColorAttachment);
            cmd.BeginRendering({
                .Offset = AreaOffset,
                .Extent = AreaExtent,
                .ColorAttachments = {{
                                         .ImageView = clearedView,
                                         .LoadOp = LoadOp::Clear,
                                         .StoreOp = StoreOp::Store,
                                         .ClearValue = ClearColor{.R = 1.0f, .A = 1.0f},
                                     },
                                     {
                                         .ImageView = loadedView,
                                         .LoadOp = LoadOp::Load,
                                         .StoreOp = StoreOp::Store,
                                     }},
            });
            cmd.EndRendering();
        });

    const vector<u8> pixels = cleared->Download();
    const vector<u8> mask = loaded->Download();
    REQUIRE(pixels.size() == static_cast<usize>(Width) * Height * 4);
    REQUIRE(mask.size() == static_cast<usize>(Width) * Height);
    u32 wrong = 0;
    for (u32 y = 0; y < Height; ++y)
    {
        for (u32 x = 0; x < Width; ++x)
        {
            const bool inside =
                static_cast<i32>(x) >= AreaOffset.x && x < AreaOffset.x + AreaExtent.x &&
                static_cast<i32>(y) >= AreaOffset.y && y < AreaOffset.y + AreaExtent.y;
            const usize texel = static_cast<usize>(y) * Width + x;
            const std::array<u8, 4> expected =
                inside ? std::array<u8, 4>{255, 0, 0, 255} : std::array<u8, 4>{0, 255, 0, 255};
            wrong += std::equal(expected.begin(), expected.end(), pixels.begin() + texel * 4) &&
                             mask[texel] == 255
                         ? 0
                         : 1;
        }
    }
    CHECK(wrong == 0);
}
