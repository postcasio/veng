// Headless smoke test (plan 10): bring up a windowless context, clear an
// off-screen image via the render graph, download it, and verify the pixels.
//
// Drives Context directly — no Window, no Application, no swapchain. This is the
// CI-facing proof that renderer code runs without a display.
//
// Note: a Vulkan ICD must be present (MoltenVK on macOS dev machines; lavapipe/
// SwiftShader on Linux CI). With the engine's fatal-assert model there is no
// graceful "no ICD" skip yet — run this only where a Vulkan implementation
// exists.

#include <cstdio>

#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/Types.h>

using namespace Veng;
using namespace Veng::Renderer;

int main()
{
    constexpr u32 size = 4;
    // Exactly representable in RGBA8Unorm: red, fully opaque.
    constexpr u8 expected[4] = {255, 0, 0, 255};

    Context context;
    context.Initialize({
        .ApplicationName = "Headless Smoke",
        .InternalRenderExtent = {size, size},
    }, nullptr);

    if (!context.IsHeadless())
    {
        std::fprintf(stderr, "FAIL: context did not initialize headless\n");
        return 1;
    }

    int status = 0;
    {
        auto image = Image::Create({
            .Name = "Headless Target",
            .Extent = {size, size, 1},
            .Format = Format::RGBA8Unorm,
            .Usage = ImageUsage::ColorAttachment | ImageUsage::TransferSrc,
        });

        auto view = ImageView::Create({
            .Name = "Headless Target View",
            .Image = image,
        });

        // Clear the image through a render-graph pass (no window, no swapchain).
        context.ImmediateCommands([&](CommandBuffer& cmd)
        {
            RenderGraph graph;
            graph.AddPass("clear")
                .Color({
                    .View = view,
                    .Load = LoadOp::Clear,
                    .Store = StoreOp::Store,
                    .Clear = ClearColor{1.0f, 0.0f, 0.0f, 1.0f},
                })
                .Execute([](CommandBuffer&) {});
            graph.Execute(cmd);
        });

        const vector<u8> pixels = image->Download();

        if (pixels.size() != static_cast<size_t>(size) * size * 4)
        {
            std::fprintf(stderr, "FAIL: unexpected download size %zu\n", pixels.size());
            status = 1;
        }
        else
        {
            for (size_t p = 0; p < pixels.size() && status == 0; p += 4)
            {
                for (u32 c = 0; c < 4; c++)
                {
                    if (pixels[p + c] != expected[c])
                    {
                        std::fprintf(stderr,
                                     "FAIL: pixel %zu channel %u = %u, expected %u\n",
                                     p / 4, c, pixels[p + c], expected[c]);
                        status = 1;
                        break;
                    }
                }
            }
        }
    }

    context.WaitIdle();
    context.DisposeResources();
    context.Dispose();

    if (status == 0)
    {
        std::printf("OK: headless render + download verified (%ux%u)\n", size, size);
    }

    return status;
}
