// Headless smoke test: bring up a windowless context, clear an
// off-screen image via the render graph, download it, and verify the pixels.
//
// Drives Context directly — no Window, no Application, no swapchain. This is the
// CI-facing proof that renderer code runs without a display.
//
// Skips cleanly (exit 77, ctest reports it as skipped — see CMakeLists.txt's
// SKIP_RETURN_CODE) on a machine with no usable Vulkan ICD, via
// Test::HasVulkanDriver().

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

int main()
{
    if (!Test::HasVulkanDriver())
    {
        return 77;
    }

    constexpr u32 size = 4;
    // Exactly representable in RGBA8Unorm: red, fully opaque.
    constexpr std::array<u8, 4> expected = {255, 0, 0, 255};

    Test::GpuContext gpu("Headless Smoke", {size, size});
    Context& context = gpu.Get();

    if (!context.IsHeadless())
    {
        std::fprintf(stderr, "FAIL: context did not initialize headless\n");
        return 1;
    }

    int status = 0;
    {
        auto image = Image::Create(
            context, {
                         .Name = "Headless Target",
                         .Extent = {size, size, 1},
                         .Format = Format::RGBA8Unorm,
                         .Usage = ImageUsage::ColorAttachment | ImageUsage::TransferSrc,
                     });

        auto view = ImageView::Create(context, {
                                                   .Name = "Headless Target View",
                                                   .Image = image,
                                               });

        // Clear the image through a render-graph pass (no window, no swapchain).
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
                        .Clear = ClearColor{.R = 1.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f},
                    })
                    .Execute([](PassContext&) {});
                const RenderGraph::ImportBinding binding{.Id = target, .View = view};
                graph.Compile()->Execute(cmd, {&binding, 1});
            });

        const vector<u8> pixels = image->Download();

        if (pixels.size() != static_cast<size_t>(size) * size * 4)
        {
            std::fprintf(stderr, "FAIL: unexpected download size %zu\n", pixels.size());
            status = 1;
        }
        else if (!Test::PixelsMatch(pixels, expected))
        {
            status = 1;
        }
    }

    if (status == 0)
    {
        std::printf("OK: headless render + download verified (%ux%u)\n", size, size);
    }

    return status;
}
