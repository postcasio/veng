// Image clear + format test (planset-3, plan 06): clears off-screen images via
// the render graph at a non-trivial extent and in a second format, broadening
// headless_smoke's single 4x4 RGBA8Unorm case.
//
//   1. A 64x32 RGBA8Unorm image cleared to an exactly-representable colour
//      (green, fully opaque) and downloaded/verified at that larger extent.
//   2. A 64x32 R8Unorm image cleared to 1.0 (byte value 255) and
//      downloaded/verified as a single-channel buffer.
//
// Skips cleanly (exit 77, ctest reports it as skipped) on a machine with no
// usable Vulkan ICD, via Test::HasVulkanDriver() (planset-3, plan 01/06).

#include <array>
#include <cstdio>

#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/Types.h>

#include <support/GpuContext.h>
#include <support/GpuProbe.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    // Clears `view` (size x extent x 1) through a one-pass render graph.
    void ClearImage(Context& context, const Ref<ImageView>& view, const ClearColor& clear)
    {
        context.ImmediateCommands([&](CommandBuffer& cmd)
        {
            RenderGraph graph;
            graph.AddPass("clear")
                .Color({
                    .View = view,
                    .Load = LoadOp::Clear,
                    .Store = StoreOp::Store,
                    .Clear = clear,
                })
                .Execute([](CommandBuffer&) {});
            graph.Execute(cmd);
        });
    }
}

int main()
{
    if (!Test::HasVulkanDriver())
        return 77;

    constexpr u32 width = 64;
    constexpr u32 height = 32;

    Test::GpuContext gpu("Image Clear Format Test", {width, height});
    Context& context = gpu.Get();

    int status = 0;

    // --- 64x32 RGBA8Unorm, cleared to green ---
    {
        // Exactly representable in RGBA8Unorm: green, fully opaque.
        constexpr std::array<u8, 4> expected = {0, 255, 0, 255};

        auto image = Image::Create({
            .Name = "Clear Target RGBA8",
            .Extent = {width, height, 1},
            .Format = Format::RGBA8Unorm,
            .Usage = ImageUsage::ColorAttachment | ImageUsage::TransferSrc,
        });

        auto view = ImageView::Create({.Name = "Clear Target RGBA8 View", .Image = image});

        ClearImage(context, view, ClearColor{0.0f, 1.0f, 0.0f, 1.0f});

        const vector<u8> pixels = image->Download();

        if (pixels.size() != static_cast<size_t>(width) * height * 4)
        {
            std::fprintf(stderr, "FAIL: RGBA8 download size %zu, expected %zu\n",
                          pixels.size(), static_cast<size_t>(width) * height * 4);
            status = 1;
        }
        else if (!Test::PixelsMatch(pixels, expected))
        {
            status = 1;
        }
    }

    // --- 64x32 R8Unorm, cleared to 1.0 (byte value 255) ---
    if (status == 0)
    {
        constexpr u8 expected = 255;

        auto image = Image::Create({
            .Name = "Clear Target R8",
            .Extent = {width, height, 1},
            .Format = Format::R8Unorm,
            .Usage = ImageUsage::ColorAttachment | ImageUsage::TransferSrc,
        });

        auto view = ImageView::Create({.Name = "Clear Target R8 View", .Image = image});

        // Only the red channel is meaningful for a single-channel format; the
        // other channels of ClearColor are ignored by the backend.
        ClearImage(context, view, ClearColor{1.0f, 0.0f, 0.0f, 1.0f});

        const vector<u8> pixels = image->Download();

        if (pixels.size() != static_cast<size_t>(width) * height)
        {
            std::fprintf(stderr, "FAIL: R8 download size %zu, expected %zu\n",
                          pixels.size(), static_cast<size_t>(width) * height);
            status = 1;
        }
        else
        {
            for (size_t p = 0; p < pixels.size() && status == 0; p++)
            {
                if (pixels[p] != expected)
                {
                    std::fprintf(stderr, "FAIL: R8 pixel %zu = %u, expected %u\n", p, pixels[p], expected);
                    status = 1;
                }
            }
        }
    }

    if (status == 0)
    {
        std::printf("OK: image clear + download verified (%ux%u, RGBA8Unorm + R8Unorm)\n", width, height);
    }

    return status;
}
