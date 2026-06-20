// Image clear + format cases: clears off-screen images via the render graph
// at a non-trivial extent and in a second format.
//
//   1. A 64x32 RGBA8Unorm image cleared to an exactly-representable colour
//      (green, fully opaque) and downloaded/verified at that larger extent.
//   2. A 64x32 R8Unorm image cleared to 1.0 (byte value 255) and
//      downloaded/verified as a single-channel buffer.

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
                const RenderGraph::ImportBinding binding{target, view};
                graph.Compile()->Execute(cmd, {&binding, 1});
            });
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture, "image clear: 64x32 RGBA8Unorm cleared to green")
{
    // Exactly representable in RGBA8Unorm: green, fully opaque.
    constexpr std::array<u8, 4> expected = {0, 255, 0, 255};

    auto image =
        Image::Create(Context, {
                                   .Name = "Clear Target RGBA8",
                                   .Extent = {Width, Height, 1},
                                   .Format = Format::RGBA8Unorm,
                                   .Usage = ImageUsage::ColorAttachment | ImageUsage::TransferSrc,
                               });

    auto view = ImageView::Create(Context, {.Name = "Clear Target RGBA8 View", .Image = image});

    ClearImage(Context, view, ClearColor{0.0f, 1.0f, 0.0f, 1.0f});

    const vector<u8> pixels = image->Download();

    REQUIRE(pixels.size() == static_cast<size_t>(Width) * Height * 4);
    CHECK(Test::PixelsMatch(pixels, expected));
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture, "image clear: 64x32 R8Unorm cleared to 255")
{
    constexpr u8 expected = 255;

    auto image =
        Image::Create(Context, {
                                   .Name = "Clear Target R8",
                                   .Extent = {Width, Height, 1},
                                   .Format = Format::R8Unorm,
                                   .Usage = ImageUsage::ColorAttachment | ImageUsage::TransferSrc,
                               });

    auto view = ImageView::Create(Context, {.Name = "Clear Target R8 View", .Image = image});

    // Only the red channel is meaningful for a single-channel format; the
    // other channels of ClearColor are ignored by the backend.
    ClearImage(Context, view, ClearColor{1.0f, 0.0f, 0.0f, 1.0f});

    const vector<u8> pixels = image->Download();

    REQUIRE(pixels.size() == static_cast<size_t>(Width) * Height);

    for (size_t p = 0; p < pixels.size(); p++)
    {
        CAPTURE(p);
        CHECK(pixels[p] == expected);
    }
}
