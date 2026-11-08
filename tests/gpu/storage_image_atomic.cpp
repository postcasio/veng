// Atomics on a storage image land. A compute pass zeroes a small R32Uint storage image; a second
// dispatch, many times larger than the image, has every invocation add one to the texel its id
// wraps onto, so each texel takes a known number of adds from invocations spread across many
// workgroups. The readback must hold exactly that count in every texel: a lost update, or an atomic
// the device silently drops, leaves it short.

#include <algorithm>
#include <cstring>
#include <vector>

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/ComputePipeline.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/DescriptorSetLayout.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/Types.h>

#include <gpu/fixture.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    // The test pack's atomic-add compute shader.
    constexpr AssetId StorageImageAtomicCompId{0xBC879E085F4766A8ULL};

    constexpr u32 Width = 4;
    constexpr u32 Height = 4;
    // The add dispatch covers this many invocations along each axis (8x8 threads per workgroup),
    // so every texel takes (Span / Width) * (Span / Height) adds.
    constexpr u32 Span = 64;
    constexpr u32 GroupSize = 8;
    constexpr u32 AddsPerTexel = (Span / Width) * (Span / Height);

    struct AtomicPush
    {
        uvec2 Extent;
        u32 Mode = 0;
        u32 Pad = 0;
    };
}

TEST_CASE_FIXTURE(Test::GpuFixture, "R32Uint storage image: concurrent atomic adds all land")
{
    AssetManager assets(Context, Tasks, Types);
    const VoidResult mountResult = assets.Mount(path(TEST_SHADER_PACK));
    REQUIRE_MESSAGE(mountResult, "mount test shader pack: ", mountResult.error());

    const AssetResult<AssetHandle<Shader>> atomicCs =
        assets.LoadSync<Shader>(StorageImageAtomicCompId);
    REQUIRE_MESSAGE(atomicCs.has_value(), "load storage_image_atomic.comp from the test pack");

    const auto target =
        Image::Create(Context, {
                                   .Name = "Atomic Target",
                                   .Extent = {Width, Height, 1},
                                   .Format = Format::R32Uint,
                                   .Usage = ImageUsage::Storage | ImageUsage::TransferSrc,
                               });
    auto storageView = ImageView::Create(Context, {.Name = "Atomic Storage View", .Image = target});

    auto setLayout =
        DescriptorSetLayout::Create(Context, {
                                                 .Name = "Atomic Set Layout",
                                                 .Bindings =
                                                     {
                                                         {.Binding = 0,
                                                          .Type = DescriptorType::StorageImage,
                                                          .Count = 1,
                                                          .Stages = ShaderStage::Compute},
                                                     },
                                             });
    const auto layout = PipelineLayout::Create(
        Context,
        {
            .Name = "Atomic Layout",
            .DescriptorSetLayouts = {setLayout},
            .PushConstantRanges = {PushConstantRange::Of<AtomicPush>(ShaderStage::Compute)},
        });
    auto pipeline = ComputePipeline::Create(
        Context,
        {
            .Name = "Atomic Pipeline",
            .PipelineLayout = layout,
            .ShaderStage = {.Stage = ShaderStage::Compute, .Module = atomicCs->Get()->Module},
        });

    auto set = DescriptorSet::Create(Context, {.Name = "Atomic Set", .Layout = setLayout});
    set->Write(0, storageView);

    Context.ImmediateCommands(
        [&](CommandBuffer& cmd)
        {
            RenderGraph graph(Context);
            const ResourceId targetId = graph.Import("Atomic Target");
            const Ref<ComputePipeline> pl = pipeline;
            const Ref<DescriptorSet> descriptors = set;
            const auto addPass = [&](const char* name, const AtomicPush push, const u32 groups)
            {
                RenderGraph::PassBuilder builder = graph.AddComputePass(name);
                builder.StorageWrite(targetId);
                builder.Execute(
                    [pl, descriptors, push, groups](PassContext& ctx)
                    {
                        CommandBuffer& c = ctx.Cmd();
                        c.BindPipeline(pl);
                        c.BindDescriptorSets(DescriptorSetBindInfo{
                            .Sets = {descriptors},
                            .FirstSet = BindlessRegistry::FirstUserSet,
                            .PipelineBindPoint = PipelineBindPoint::Compute,
                        });
                        c.PushConstants(push);
                        c.Dispatch(groups, groups, 1);
                    });
            };
            addPass("Atomic Clear", AtomicPush{.Extent = {Width, Height}, .Mode = 0}, 1);
            addPass("Atomic Add", AtomicPush{.Extent = {Width, Height}, .Mode = 1},
                    Span / GroupSize);

            const RenderGraph::ImportBinding binding{.Id = targetId, .View = storageView};
            graph.Compile()->Execute(cmd, {&binding, 1});
        });

    const std::vector<u8> bytes = target->Download();
    REQUIRE(bytes.size() == static_cast<size_t>(Width) * Height * sizeof(u32));

    std::vector<u32> texels(static_cast<size_t>(Width) * Height);
    std::memcpy(texels.data(), bytes.data(), bytes.size());
    const auto [lowest, highest] = std::ranges::minmax(texels);
    CHECK(lowest == AddsPerTexel);
    CHECK(highest == AddsPerTexel);
}
