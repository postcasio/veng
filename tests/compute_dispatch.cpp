// Compute dispatch test: brings up a windowless context and runs a
// three-pass render graph that exercises the full
// graphics-write -> compute-read/write -> graphics-read chain:
//
//   1. A graphics pass clears a "source" image to a known colour.
//   2. A compute pass (cmd.Dispatch) reads the source image as a storage
//      image and writes the colour-inverted result to a "derived" storage
//      image.
//   3. A graphics pass samples the derived image and writes it to an output
//      image, which is downloaded and checked against the expected inverted
//      colour.
//
// With validation layers enabled (VE_ENABLE_VALIDATION_LAYERS) this also
// proves the render graph's derived barriers are sync-validation clean across
// a compute pass.
//
// Skips cleanly (exit 77, ctest reports it as skipped — see CMakeLists.txt's
// SKIP_RETURN_CODE) on a machine with no usable Vulkan ICD, via
// Test::HasVulkanDriver().

#include <array>
#include <cstdio>

#include <Veng/Assert.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Task/TaskSystem.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/ComputePipeline.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/DescriptorSetLayout.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Asset/Shader.h>
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

    // Source image is cleared to (0.2, 0.4, 0.6, 1.0); the compute pass writes
    // 1 - rgb (alpha unchanged), so the output image must match `expected`.
    constexpr std::array<u8, 4> expected = {204, 153, 102, 255}; // ~ (0.8, 0.6, 0.4, 1.0)

    Test::GpuContext gpu("Compute Dispatch Test", {size, size});
    Context& context = gpu.Get();

    if (!context.IsHeadless())
    {
        std::fprintf(stderr, "FAIL: context did not initialize headless\n");
        return 1;
    }

    int status = 0;
    {
        // Shaders reach the GPU only through a cooked pack; mount the test pack
        // and load each module by id. Handles stay in scope until after the
        // graph executes so their modules outlive the pipelines.
        TaskSystem tasks;
        TypeRegistry types;
        AssetManager assets(context, tasks, types);
        const VoidResult mountResult = assets.Mount(path(TEST_SHADER_PACK));
        VE_ASSERT(mountResult, "mount test shader pack: {}", mountResult.error());

        auto sourceImage =
            Image::Create(context, {
                                       .Name = "Compute Source",
                                       .Extent = {size, size, 1},
                                       .Format = Format::RGBA8Unorm,
                                       .Usage = ImageUsage::ColorAttachment | ImageUsage::Storage,
                                   });

        auto sourceView = ImageView::Create(context, {
                                                         .Name = "Compute Source View",
                                                         .Image = sourceImage,
                                                     });

        auto derivedImage =
            Image::Create(context, {
                                       .Name = "Compute Derived",
                                       .Extent = {size, size, 1},
                                       .Format = Format::RGBA8Unorm,
                                       .Usage = ImageUsage::Storage | ImageUsage::Sampled,
                                   });

        auto derivedView = ImageView::Create(context, {
                                                          .Name = "Compute Derived View",
                                                          .Image = derivedImage,
                                                      });

        auto outputImage = Image::Create(
            context, {
                         .Name = "Compute Output",
                         .Extent = {size, size, 1},
                         .Format = Format::RGBA8Unorm,
                         .Usage = ImageUsage::ColorAttachment | ImageUsage::TransferSrc,
                     });

        auto outputView = ImageView::Create(context, {
                                                         .Name = "Compute Output View",
                                                         .Image = outputImage,
                                                     });

        // --- Compute pipeline: reads `source`, writes `derived`. ---

        const auto computeShaderAsset = assets.LoadSync<Shader>(AssetId{0x1F41});
        VE_ASSERT(computeShaderAsset, "load invert.comp: {}", computeShaderAsset.error().Detail);

        auto computeSetLayout =
            DescriptorSetLayout::Create(context, {
                                                     .Name = "Compute Set Layout",
                                                     .Bindings =
                                                         {
                                                             {.Binding = 0,
                                                              .Type = DescriptorType::StorageImage,
                                                              .Count = 1,
                                                              .Stages = ShaderStage::Compute},
                                                             {.Binding = 1,
                                                              .Type = DescriptorType::StorageImage,
                                                              .Count = 1,
                                                              .Stages = ShaderStage::Compute},
                                                         },
                                                 });

        auto computeLayout =
            PipelineLayout::Create(context, {
                                                .Name = "Compute Layout",
                                                .DescriptorSetLayouts = {computeSetLayout},
                                            });

        auto computePipeline = ComputePipeline::Create(
            context, {
                         .Name = "Invert Pipeline",
                         .PipelineLayout = computeLayout,
                         .ShaderStage = {.Stage = ShaderStage::Compute,
                                         .Module = computeShaderAsset->Get()->Module},
                     });

        auto computeSet = DescriptorSet::Create(context, {
                                                             .Name = "Compute Set",
                                                             .Layout = computeSetLayout,
                                                         });

        computeSet->Write(0, sourceView);
        computeSet->Write(1, derivedView);

        // --- Graphics pipeline: samples `derived`, writes to `output`. ---

        const auto vertexShaderAsset = assets.LoadSync<Shader>(AssetId{0x1F42});
        VE_ASSERT(vertexShaderAsset, "load fullscreen.vert: {}", vertexShaderAsset.error().Detail);

        const auto fragmentShaderAsset = assets.LoadSync<Shader>(AssetId{0x1F43});
        VE_ASSERT(fragmentShaderAsset, "load sample.frag: {}", fragmentShaderAsset.error().Detail);

        auto sampleSetLayout = DescriptorSetLayout::Create(
            context, {
                         .Name = "Sample Set Layout",
                         .Bindings =
                             {
                                 {.Binding = 0,
                                  .Type = DescriptorType::CombinedImageSampler,
                                  .Count = 1,
                                  .Stages = ShaderStage::Fragment},
                             },
                     });

        auto sampleLayout =
            PipelineLayout::Create(context, {
                                                .Name = "Sample Layout",
                                                .DescriptorSetLayouts = {sampleSetLayout},
                                            });

        auto samplePipeline = GraphicsPipeline::Create(
            context,
            {
                .Name = "Sample Pipeline",
                .ColorAttachments = {{.Format = Format::RGBA8Unorm}},
                .PipelineLayout = sampleLayout,
                .ShaderStages =
                    {
                        {.Stage = ShaderStage::Vertex, .Module = vertexShaderAsset->Get()->Module},
                        {.Stage = ShaderStage::Fragment,
                         .Module = fragmentShaderAsset->Get()->Module},
                    },
            });

        auto sampler = Sampler::Create(context, {
                                                    .Name = "Compute Test Sampler",
                                                    .AddressModeU = AddressMode::ClampToEdge,
                                                    .AddressModeV = AddressMode::ClampToEdge,
                                                    .AddressModeW = AddressMode::ClampToEdge,
                                                });

        auto sampleSet = DescriptorSet::Create(context, {
                                                            .Name = "Sample Set",
                                                            .Layout = sampleSetLayout,
                                                        });

        sampleSet->Write(0, derivedView, sampler);

        // --- The three-pass graph. ---

        context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                RenderGraph graph(context);

                const ResourceId sourceId = graph.Import("Source");
                const ResourceId derivedId = graph.Import("Derived");
                const ResourceId outputId = graph.Import("Output");

                graph.AddPass("Clear Source")
                    .Color({
                        .Resource = sourceId,
                        .Load = LoadOp::Clear,
                        .Store = StoreOp::Store,
                        .Clear = ClearColor{.R = 0.2f, .G = 0.4f, .B = 0.6f, .A = 1.0f},
                    })
                    .Execute([](PassContext&) {});

                graph.AddComputePass("Invert")
                    .StorageRead(sourceId)
                    .StorageWrite(derivedId)
                    .Execute(
                        [&](PassContext& ctx)
                        {
                            CommandBuffer& cmd = ctx.Cmd();
                            cmd.BindPipeline(computePipeline);
                            cmd.BindDescriptorSets({
                                .Sets = {computeSet},
                                .FirstSet = 1, // set 0 is reserved for the bindless registry
                                .PipelineBindPoint = PipelineBindPoint::Compute,
                            });
                            cmd.Dispatch(size, size, 1);
                        });

                graph.AddPass("Sample Derived")
                    .Color({
                        .Resource = outputId,
                        .Load = LoadOp::Clear,
                        .Store = StoreOp::Store,
                        .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f},
                    })
                    .Sample(derivedId)
                    .Execute(
                        [&](PassContext& ctx)
                        {
                            CommandBuffer& cmd = ctx.Cmd();
                            cmd.BindPipeline(samplePipeline);
                            cmd.SetViewport({0, 0}, {size, size});
                            cmd.SetScissor({0, 0}, {size, size});
                            cmd.BindDescriptorSets(
                                {.Sets = {sampleSet},
                                 .FirstSet = 1}); // set 0 is reserved for the bindless registry
                            cmd.DrawFullscreenTriangle();
                        });

                const RenderGraph::ImportBinding bindings[] = {
                    {.Id = sourceId, .View = sourceView},
                    {.Id = derivedId, .View = derivedView},
                    {.Id = outputId, .View = outputView},
                };
                graph.Compile()->Execute(cmd, bindings);
            });

        const vector<u8> pixels = outputImage->Download();

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
        std::printf("OK: compute dispatch verified (%ux%u)\n", size, size);
    }

    return status;
}
