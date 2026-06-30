// Type3D volume-texture lifecycle, the MoltenVK 3D-storage probe. Drives the
// full create -> storage-write -> sample -> readback -> mid-frame-retire path a
// 3D texture has never exercised, with the extent.z axis as the new variable:
//
//   1. Create an RGBA16F Type3D storage image and write a known per-voxel field
//      from a compute dispatch (imageStore at a 3D coordinate).
//   2. Barrier (graph-derived StorageWrite -> Sample) and sample it as a 3D
//      texture in a second compute pass at each voxel center (nearest filter, so
//      the fetch returns the exact stored texel), writing an identity copy into a
//      second Type3D storage image.
//   3. Download the output volume (folding extent.z) and assert it matches what
//      was written across all z slices.
//   4. Drop the source 3D image's Ref mid-frame and run another graph frame to
//      prove the deferred-retire path handles a Type3D image cleanly.
//
// If write-only 3D storage or 3D sampling is unsupported on the installed
// MoltenVK this case fails here, before any consumer depends on the layout.

#include <array>
#include <bit>
#include <cstring>
#include <span>
#include <vector>

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/ComputePipeline.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/DescriptorSetLayout.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Renderer/Types.h>

#include <gpu/fixture.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    // The test pack's 3D volume write / sample compute shaders.
    constexpr AssetId VolumeWriteCompId{0x1F49};
    constexpr AssetId VolumeSampleCompId{0x1F4A};

    struct VolumePush
    {
        uvec3 Extent;
    };

    // Decodes one IEEE-754 half to float. The written field is small integers
    // (0..extent) and 1.0, all exactly representable in f16, so the roundtrip is
    // exact and an exact float compare is the right assertion.
    float HalfToFloat(u16 h)
    {
        const u32 sign = (h & 0x8000u) << 16;
        const u32 exp = (h >> 10) & 0x1Fu;
        const u32 mant = h & 0x3FFu;
        u32 bits = 0;
        if (exp == 0)
        {
            // Zero or subnormal: scale the mantissa into a normalized float.
            if (mant != 0)
            {
                float value = static_cast<float>(mant) * (1.0f / 16777216.0f);
                std::memcpy(&bits, &value, sizeof(bits));
                bits |= sign;
            }
            else
            {
                bits = sign;
            }
        }
        else if (exp == 0x1Fu)
        {
            bits = sign | 0x7F800000u | (mant << 13);
        }
        else
        {
            bits = sign | ((exp + 112u) << 23) | (mant << 13);
        }
        float out;
        std::memcpy(&out, &bits, sizeof(out));
        return out;
    }
}

TEST_CASE_FIXTURE(Test::GpuFixture,
                  "Type3D volume texture: write, sample, readback, mid-frame retire")
{
    // A non-cubic extent so an extent.z bug in mip/byte/copy math cannot hide
    // behind equal axes.
    constexpr u32 W = 8;
    constexpr u32 H = 4;
    constexpr u32 D = 6;
    constexpr u32 ChannelCount = 4;

    AssetManager assets(Context, Tasks, Types);
    const VoidResult mountResult = assets.Mount(path(TEST_SHADER_PACK));
    REQUIRE_MESSAGE(mountResult, "mount test shader pack: ", mountResult.error());

    const AssetResult<AssetHandle<Shader>> writeCs = assets.LoadSync<Shader>(VolumeWriteCompId);
    REQUIRE_MESSAGE(writeCs.has_value(), "load volume_write.comp from the test pack");
    const AssetResult<AssetHandle<Shader>> sampleCs = assets.LoadSync<Shader>(VolumeSampleCompId);
    REQUIRE_MESSAGE(sampleCs.has_value(), "load volume_sample.comp from the test pack");

    // The source volume: written by compute, then sampled as a 3D texture.
    auto source = Image::Create(Context, {
                                             .Name = "Volume Source",
                                             .Extent = {W, H, D},
                                             .Format = Format::RGBA16Sfloat,
                                             .Type = ImageType::Type3D,
                                             .Usage = ImageUsage::Storage | ImageUsage::Sampled,
                                         });
    auto sourceStorageView = ImageView::Create(
        Context,
        {.Name = "Volume Source Storage View", .Image = source, .ViewType = ImageViewType::Type3D});
    auto sourceSampledView = ImageView::Create(
        Context,
        {.Name = "Volume Source Sampled View", .Image = source, .ViewType = ImageViewType::Type3D});

    // The destination volume: the identity copy the sample pass writes, read back.
    auto dest = Image::Create(Context, {
                                           .Name = "Volume Dest",
                                           .Extent = {W, H, D},
                                           .Format = Format::RGBA16Sfloat,
                                           .Type = ImageType::Type3D,
                                           .Usage = ImageUsage::Storage | ImageUsage::TransferSrc,
                                       });
    auto destStorageView = ImageView::Create(
        Context,
        {.Name = "Volume Dest Storage View", .Image = dest, .ViewType = ImageViewType::Type3D});

    // Nearest filter + clamp so the voxel-center fetch returns the exact texel.
    auto sampler = Sampler::Create(Context, {
                                                .Name = "Volume Sampler",
                                                .MagFilter = Filter::Nearest,
                                                .MinFilter = Filter::Nearest,
                                                .MipmapMode = MipmapMode::Nearest,
                                                .AddressModeU = AddressMode::ClampToEdge,
                                                .AddressModeV = AddressMode::ClampToEdge,
                                                .AddressModeW = AddressMode::ClampToEdge,
                                                .AnisotropyEnabled = false,
                                            });

    // Write-pass layout: binding 0 is the 3D storage destination.
    auto writeSetLayout =
        DescriptorSetLayout::Create(Context, {
                                                 .Name = "Volume Write Set Layout",
                                                 .Bindings =
                                                     {
                                                         {.Binding = 0,
                                                          .Type = DescriptorType::StorageImage,
                                                          .Count = 1,
                                                          .Stages = ShaderStage::Compute},
                                                     },
                                             });
    auto writeLayout = PipelineLayout::Create(
        Context,
        {
            .Name = "Volume Write Layout",
            .DescriptorSetLayouts = {writeSetLayout},
            .PushConstantRanges = {PushConstantRange::Of<VolumePush>(ShaderStage::Compute)},
        });
    auto writePipeline = ComputePipeline::Create(
        Context,
        {
            .Name = "Volume Write Pipeline",
            .PipelineLayout = writeLayout,
            .ShaderStage = {.Stage = ShaderStage::Compute, .Module = writeCs->Get()->Module},
        });

    auto writeSet =
        DescriptorSet::Create(Context, {.Name = "Volume Write Set", .Layout = writeSetLayout});
    writeSet->Write(0, sourceStorageView);

    // Sample-pass layout: binding 0 is the combined 3D sampler, binding 1 the
    // 3D storage destination.
    auto sampleSetLayout = DescriptorSetLayout::Create(
        Context, {
                     .Name = "Volume Sample Set Layout",
                     .Bindings =
                         {
                             {.Binding = 0,
                              .Type = DescriptorType::CombinedImageSampler,
                              .Count = 1,
                              .Stages = ShaderStage::Compute},
                             {.Binding = 1,
                              .Type = DescriptorType::StorageImage,
                              .Count = 1,
                              .Stages = ShaderStage::Compute},
                         },
                 });
    auto sampleLayout = PipelineLayout::Create(
        Context,
        {
            .Name = "Volume Sample Layout",
            .DescriptorSetLayouts = {sampleSetLayout},
            .PushConstantRanges = {PushConstantRange::Of<VolumePush>(ShaderStage::Compute)},
        });
    auto samplePipeline = ComputePipeline::Create(
        Context,
        {
            .Name = "Volume Sample Pipeline",
            .PipelineLayout = sampleLayout,
            .ShaderStage = {.Stage = ShaderStage::Compute, .Module = sampleCs->Get()->Module},
        });

    auto sampleSet =
        DescriptorSet::Create(Context, {.Name = "Volume Sample Set", .Layout = sampleSetLayout});
    sampleSet->Write(0, sourceSampledView, sampler);
    sampleSet->Write(1, destStorageView);

    const VolumePush push{.Extent = {W, H, D}};
    const auto groups = [](u32 n) { return (n + 3) / 4; };

    // Two compute passes: write the source volume, then sample it into dest.
    // The graph derives the StorageWrite(source) -> Sample(source) barrier the
    // 3D image needs between them.
    Context.ImmediateCommands(
        [&](CommandBuffer& cmd)
        {
            RenderGraph graph(Context);
            const ResourceId sourceId = graph.Import("Volume Source");
            const ResourceId destId = graph.Import("Volume Dest");

            {
                RenderGraph::PassBuilder builder = graph.AddComputePass("Volume Write");
                builder.StorageWrite(sourceId);
                const Ref<ComputePipeline> pl = writePipeline;
                const Ref<DescriptorSet> set = writeSet;
                builder.Execute(
                    [pl, set, push, groups](PassContext& ctx)
                    {
                        CommandBuffer& c = ctx.Cmd();
                        c.BindPipeline(pl);
                        c.BindDescriptorSets(DescriptorSetBindInfo{
                            .Sets = {set},
                            .FirstSet = 1,
                            .PipelineBindPoint = PipelineBindPoint::Compute,
                        });
                        c.PushConstants(push);
                        c.Dispatch(groups(W), groups(H), groups(D));
                    });
            }
            {
                RenderGraph::PassBuilder builder = graph.AddComputePass("Volume Sample");
                builder.Sample(sourceId);
                builder.StorageWrite(destId);
                const Ref<ComputePipeline> pl = samplePipeline;
                const Ref<DescriptorSet> set = sampleSet;
                builder.Execute(
                    [pl, set, push, groups](PassContext& ctx)
                    {
                        CommandBuffer& c = ctx.Cmd();
                        c.BindPipeline(pl);
                        c.BindDescriptorSets(DescriptorSetBindInfo{
                            .Sets = {set},
                            .FirstSet = 1,
                            .PipelineBindPoint = PipelineBindPoint::Compute,
                        });
                        c.PushConstants(push);
                        c.Dispatch(groups(W), groups(H), groups(D));
                    });
            }

            const std::array<RenderGraph::ImportBinding, 2> bindings{
                RenderGraph::ImportBinding{.Id = sourceId, .View = sourceStorageView},
                RenderGraph::ImportBinding{.Id = destId, .View = destStorageView},
            };
            graph.Compile()->Execute(cmd, bindings);
        });

    // Read back the whole volume (Download folds extent.z) and verify every voxel
    // across every z slice equals the field volume_write wrote.
    const std::vector<u8> bytes = dest->Download();
    REQUIRE(bytes.size() == static_cast<size_t>(W) * H * D * ChannelCount * sizeof(u16));

    const auto* halfs = reinterpret_cast<const u16*>(bytes.data());
    for (u32 z = 0; z < D; ++z)
    {
        for (u32 y = 0; y < H; ++y)
        {
            for (u32 x = 0; x < W; ++x)
            {
                const size_t voxel = (static_cast<size_t>(z) * H + y) * W + x;
                const size_t base = voxel * ChannelCount;
                CAPTURE(x);
                CAPTURE(y);
                CAPTURE(z);
                CHECK(HalfToFloat(halfs[base + 0]) == static_cast<float>(x));
                CHECK(HalfToFloat(halfs[base + 1]) == static_cast<float>(y));
                CHECK(HalfToFloat(halfs[base + 2]) == static_cast<float>(z));
                CHECK(HalfToFloat(halfs[base + 3]) == 1.0f);
            }
        }
    }

    // Mid-frame retire: drop every owner of the source volume (the descriptor
    // sets that referenced it, its views, then the image itself) while the device
    // is live, then run another graph frame. The Type3D image's handle retires
    // into the frame bin and is destroyed only once the GPU is done — a leak or
    // premature destroy of the 3D resource surfaces here. Rebinding writeSet's
    // storage image to dest before the reset releases its Ref on the source view.
    writeSet->Write(0, destStorageView);
    sampleSet.reset();
    sourceSampledView.reset();
    sourceStorageView.reset();
    source.reset();

    Context.ImmediateCommands(
        [&](CommandBuffer& cmd)
        {
            RenderGraph graph(Context);
            const ResourceId destId = graph.Import("Volume Dest");
            RenderGraph::PassBuilder builder = graph.AddComputePass("Volume Rewrite");
            builder.StorageWrite(destId);
            const Ref<ComputePipeline> pl = writePipeline;
            const Ref<DescriptorSet> set = writeSet;
            builder.Execute(
                [pl, set, push, groups](PassContext& ctx)
                {
                    CommandBuffer& c = ctx.Cmd();
                    c.BindPipeline(pl);
                    c.BindDescriptorSets(DescriptorSetBindInfo{
                        .Sets = {set},
                        .FirstSet = 1,
                        .PipelineBindPoint = PipelineBindPoint::Compute,
                    });
                    c.PushConstants(push);
                    c.Dispatch(groups(W), groups(H), groups(D));
                });

            const RenderGraph::ImportBinding binding{.Id = destId, .View = destStorageView};
            graph.Compile()->Execute(cmd, {&binding, 1});
        });

    CHECK(true); // reaching here with no abort/validation error is the retire pass.
}
