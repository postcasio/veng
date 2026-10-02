// The SSR max-Z pyramid's coarse tail: one workgroup reduces the smallest levels in one dispatch, and
// each level it writes is the max-Z reduction's own definition — the MAX over its proportional
// footprint in the level above — so the nearest surface in a region survives to every coarser level
// that covers it, up to the 1x1. Level 0 is reduced the per-level way and the tail runs from level 1,
// as the SSR chain runs it; an odd extent puts overlapping footprints on both axes.

#include <algorithm>
#include <bit>
#include <cstring>
#include <span>
#include <vector>

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/ComputePipeline.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/DescriptorSetLayout.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/Native.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/Types.h>

#include <gpu/fixture.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    // The core pack's SSR max-Z reduction and its coarse tail.
    constexpr AssetId SsrHiZReduceCompId{0x93DA6E42B3B5479AULL};
    constexpr AssetId SsrHiZReduceTailCompId{0x7737782FD6CC04D1ULL};

    // The tail binds the level before it, then one storage view per slot (eight slots).
    constexpr u32 TailSlots = 8;

    struct ReducePush
    {
        uvec2 DestExtent;
        uvec2 SourceExtent;
    };

    struct TailPush
    {
        uvec2 Extent;
        u32 FirstLevel;
        u32 LevelCount;
    };

    uvec2 ExtentAt(const uvec2 base, const u32 level)
    {
        return {std::max(base.x >> level, 1u), std::max(base.y >> level, 1u)};
    }

    // Reads back one R32 mip level (Image::Download is mip-0 only).
    std::vector<float> DownloadMip(Context& context, const Ref<Image>& image,
                                   const Ref<ImageView>& mipView, const u32 level,
                                   const uvec2 extent)
    {
        auto buffer = Buffer::Create(
            context, {
                         .Name = "SSR MinZ Mip Readback",
                         .Size = static_cast<u64>(extent.x) * extent.y * sizeof(float),
                         .Usage = BufferUsage::TransferDst,
                     });
        context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                cmd.PrepareForAccess(mipView, AccessKind::TransferSrc);
                const vk::BufferImageCopy region{
                    .bufferOffset = 0,
                    .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                         .mipLevel = level,
                                         .baseArrayLayer = 0,
                                         .layerCount = 1},
                    .imageOffset = {.x = 0, .y = 0, .z = 0},
                    .imageExtent = {.width = extent.x, .height = extent.y, .depth = 1},
                };
                GetVkCommandBuffer(cmd).copyImageToBuffer(GetVkImage(*image),
                                                          vk::ImageLayout::eTransferSrcOptimal,
                                                          GetVkBuffer(*buffer), 1, &region);
            });
        const std::vector<u8> bytes = buffer->Download();
        std::vector<float> out(static_cast<size_t>(extent.x) * extent.y);
        std::memcpy(out.data(), bytes.data(), out.size() * sizeof(float));
        return out;
    }
}

TEST_CASE_FIXTURE(
    Test::GpuFixture,
    "ssr min-z tail: each tail level is the max over its footprint in the level above")
{
    // 45x27 -> 22x13 -> 11x6 -> 5x3 -> 2x1 -> 1x1: every level from 1 fits the tail, and the odd
    // parents give overlapping footprints.
    constexpr uvec2 Base{45, 27};
    const u32 mips = static_cast<u32>(std::bit_width(std::max(Base.x, Base.y)));

    AssetManager assets(Context, Tasks, Types);
    const AssetResult<AssetHandle<Shader>> reduceCs = assets.LoadSync<Shader>(SsrHiZReduceCompId);
    const AssetResult<AssetHandle<Shader>> tailCs = assets.LoadSync<Shader>(SsrHiZReduceTailCompId);
    REQUIRE(reduceCs.has_value());
    REQUIRE(tailCs.has_value());

    // A varied reverse-Z field (background 0 is far) with one near texel deep inside it — the
    // nearest surface, which has to reach the 1x1.
    std::vector<float> field(static_cast<size_t>(Base.x) * Base.y);
    for (u32 y = 0; y < Base.y; ++y)
    {
        for (u32 x = 0; x < Base.x; ++x)
        {
            field[static_cast<size_t>(y) * Base.x + x] =
                0.05f * static_cast<float>((x * 5 + y * 3) % 13);
        }
    }
    constexpr float Nearest = 0.97f;
    field[static_cast<size_t>(17) * Base.x + 31] = Nearest;

    auto source = Image::Create(Context, {
                                             .Name = "SSR MinZ Source Depth Field",
                                             .Extent = {Base.x, Base.y, 1},
                                             .Format = Format::R32Sfloat,
                                             .Usage = ImageUsage::Sampled | ImageUsage::TransferDst,
                                         });
    source->UploadSync(
        std::span(reinterpret_cast<const u8*>(field.data()), field.size() * sizeof(float)));
    auto sourceView = ImageView::Create(Context, {.Name = "SSR MinZ Source View", .Image = source});

    auto pyramid = Image::Create(
        Context, {
                     .Name = "SSR MinZ Pyramid",
                     .Extent = {Base.x, Base.y, 1},
                     .MipLevels = mips,
                     .Format = Format::R32Sfloat,
                     .Usage = ImageUsage::Storage | ImageUsage::Sampled | ImageUsage::TransferSrc,
                 });
    std::vector<Ref<ImageView>> mipViews;
    for (u32 level = 0; level < mips; ++level)
    {
        mipViews.push_back(ImageView::Create(Context, {.Name = "SSR MinZ Mip View",
                                                       .Image = pyramid,
                                                       .BaseMipLevel = level,
                                                       .MipLevels = 1}));
    }

    // Level 0: the per-level reduction (1:1 here).
    auto reduceSetLayout =
        DescriptorSetLayout::Create(Context, {.Name = "SSR MinZ Reduce Set Layout",
                                              .Bindings = {{.Binding = 0,
                                                            .Type = DescriptorType::SampledImage,
                                                            .Count = 1,
                                                            .Stages = ShaderStage::Compute},
                                                           {.Binding = 1,
                                                            .Type = DescriptorType::StorageImage,
                                                            .Count = 1,
                                                            .Stages = ShaderStage::Compute}}});
    auto reduceLayout = PipelineLayout::Create(
        Context, {.Name = "SSR MinZ Reduce Layout",
                  .DescriptorSetLayouts = {reduceSetLayout},
                  .PushConstantRanges = {PushConstantRange::Of<ReducePush>(ShaderStage::Compute)}});
    auto reducePipeline = ComputePipeline::Create(
        Context,
        {.Name = "SSR MinZ Reduce Pipeline",
         .PipelineLayout = reduceLayout,
         .ShaderStage = {.Stage = ShaderStage::Compute, .Module = reduceCs->Get()->Module}});
    auto reduceSet =
        DescriptorSet::Create(Context, {.Name = "SSR MinZ Reduce Set", .Layout = reduceSetLayout});
    reduceSet->Write(0, sourceView);
    reduceSet->Write(1, mipViews[0]);

    // Levels 1 on: the tail.
    vector<DescriptorBinding> tailBindings{{.Binding = 0,
                                            .Type = DescriptorType::SampledImage,
                                            .Count = 1,
                                            .Stages = ShaderStage::Compute}};
    for (u32 slot = 0; slot < TailSlots; ++slot)
    {
        tailBindings.push_back({.Binding = 1 + slot,
                                .Type = DescriptorType::StorageImage,
                                .Count = 1,
                                .Stages = ShaderStage::Compute});
    }
    auto tailSetLayout = DescriptorSetLayout::Create(
        Context, {.Name = "SSR MinZ Tail Set Layout", .Bindings = tailBindings});
    auto tailLayout = PipelineLayout::Create(
        Context, {.Name = "SSR MinZ Tail Layout",
                  .DescriptorSetLayouts = {tailSetLayout},
                  .PushConstantRanges = {PushConstantRange::Of<TailPush>(ShaderStage::Compute)}});
    auto tailPipeline = ComputePipeline::Create(
        Context, {.Name = "SSR MinZ Tail Pipeline",
                  .PipelineLayout = tailLayout,
                  .ShaderStage = {.Stage = ShaderStage::Compute, .Module = tailCs->Get()->Module}});
    auto tailSet =
        DescriptorSet::Create(Context, {.Name = "SSR MinZ Tail Set", .Layout = tailSetLayout});
    tailSet->Write(0, mipViews[0]);
    for (u32 slot = 0; slot < TailSlots; ++slot)
    {
        tailSet->Write(1 + slot, mipViews[std::min(1 + slot, mips - 1)]);
    }

    Context.ImmediateCommands(
        [&](CommandBuffer& cmd)
        {
            RenderGraph graph(Context);
            const ResourceId sourceId = graph.Import("SSR MinZ Source");
            const MipChainId chain = graph.ImportImageMips("SSR MinZ", mips);

            graph.AddComputePass("SSR MinZ Reduce Mip 0")
                .Sample(sourceId)
                .StorageWrite(chain.Level(0))
                .Execute(
                    [&](PassContext& ctx)
                    {
                        CommandBuffer& c = ctx.Cmd();
                        c.BindPipeline(reducePipeline);
                        c.BindDescriptorSets(DescriptorSetBindInfo{
                            .Sets = {reduceSet},
                            .FirstSet = 3,
                            .PipelineBindPoint = PipelineBindPoint::Compute,
                        });
                        c.PushConstants(ReducePush{.DestExtent = Base, .SourceExtent = Base});
                        c.Dispatch((Base.x + 7) / 8, (Base.y + 7) / 8, 1);
                    });
            RenderGraph::PassBuilder tail = graph.AddComputePass("SSR MinZ Reduce Tail");
            tail.Sample(chain.Level(0));
            for (u32 level = 1; level < mips; ++level)
            {
                tail.StorageWrite(chain.Level(level));
            }
            tail.Execute(
                [&](PassContext& ctx)
                {
                    CommandBuffer& c = ctx.Cmd();
                    c.BindPipeline(tailPipeline);
                    c.BindDescriptorSets(DescriptorSetBindInfo{
                        .Sets = {tailSet},
                        .FirstSet = 3,
                        .PipelineBindPoint = PipelineBindPoint::Compute,
                    });
                    c.PushConstants(
                        TailPush{.Extent = Base, .FirstLevel = 1, .LevelCount = mips - 1});
                    c.Dispatch(1, 1, 1);
                });

            std::vector<RenderGraph::ImportBinding> bindings{{sourceId, sourceView}};
            for (u32 level = 0; level < mips; ++level)
            {
                bindings.push_back({chain.Level(level), mipViews[level]});
            }
            graph.Compile()->Execute(cmd, bindings);
        });

    // Each tail texel against the max over its footprint in the level the GPU wrote above it.
    u32 mismatches = 0;
    std::vector<float> above = DownloadMip(Context, pyramid, mipViews[0], 0, Base);
    for (u32 level = 1; level < mips; ++level)
    {
        const uvec2 src = ExtentAt(Base, level - 1);
        const uvec2 dst = ExtentAt(Base, level);
        const std::vector<float> got = DownloadMip(Context, pyramid, mipViews[level], level, dst);
        for (u32 y = 0; y < dst.y; ++y)
        {
            for (u32 x = 0; x < dst.x; ++x)
            {
                const uvec2 start = (uvec2(x, y) * src) / dst;
                const uvec2 end = ((uvec2(x, y) + 1u) * src + dst - 1u) / dst;
                float expected = 0.0f;
                for (u32 sy = start.y; sy < end.y; ++sy)
                {
                    for (u32 sx = start.x; sx < end.x; ++sx)
                    {
                        expected = std::max(expected, above[static_cast<size_t>(sy) * src.x + sx]);
                    }
                }
                mismatches += got[static_cast<size_t>(y) * dst.x + x] == expected ? 0u : 1u;
            }
        }
        above = got;
    }
    CHECK(mismatches == 0u);
    // The nearest surface reached the 1x1.
    REQUIRE(above.size() == 1u);
    CHECK(above[0] == Nearest);
}
