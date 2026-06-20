// Hi-Z max-Z reduction correctness. Builds a known depth field, runs the core
// pack's hi_z_reduce.comp through the per-mip (subresource) render-graph surface
// (RenderGraph::ImportImageMips / PassContext::ResolvedMip), reads back every mip,
// and compares each against a CPU reference reduction.
//
// Two properties are pinned:
//   - Odd-extent handling: an odd parent dimension folds its dropped row/column
//     into the max (the 3x3 clamp), so no far sample is lost from the pyramid.
//   - Conservatism: a single far texel buried in a near footprint survives to the
//     coarsest mip — max-Z over [0,1] depth never reports a footprint nearer than
//     its farthest sample.
//
// The chain is one image with a full mip chain; the reduction binds one per-mip
// storage view per dispatch, so this also exercises the per-mip graph surface and
// (under the validation gate) its derived per-subresource read-after-write barriers.

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
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
#include <Veng/Renderer/Native.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/Types.h>

#include <gpu/fixture.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    // The core pack's hi-Z reduction compute shader.
    constexpr AssetId HiZReduceCompId{0xCB20C4EF8A20ADBCULL};

    struct HiZReducePush
    {
        uvec2 DestExtent;
        uvec2 SourceExtent;
    };

    // floor(log2(max(w,h))) + 1.
    u32 MipCountFor(u32 w, u32 h)
    {
        const u32 maxDim = std::max(w, h);
        return maxDim == 0 ? 1 : std::bit_width(maxDim);
    }

    // The CPU reference reduction the GPU must match. mip[0] is a 1:1 copy of the
    // source field; mip[k] maxes the 2x2 footprint of mip[k-1], folding the dropped
    // row/column when a parent dimension is odd (the 3x3 clamp).
    std::vector<std::vector<float>> ReferenceReduce(const std::vector<float>& field, u32 w, u32 h)
    {
        const u32 mips = MipCountFor(w, h);
        std::vector<std::vector<float>> levels(mips);
        std::vector<uvec2> extents(mips);

        levels[0] = field;
        extents[0] = {w, h};

        for (u32 k = 1; k < mips; ++k)
        {
            const uvec2 src = extents[k - 1];
            const uvec2 dst = {std::max(src.x >> 1, 1u), std::max(src.y >> 1, 1u)};
            extents[k] = dst;
            std::vector<float>& out = levels[k];
            out.assign(static_cast<size_t>(dst.x) * dst.y, 0.0f);

            const std::vector<float>& in = levels[k - 1];
            const bool oddX = (src.x & 1u) != 0u;
            const bool oddY = (src.y & 1u) != 0u;

            // Mirror the shader's LoadClamped: coordinates clamp into the source
            // extent, so an out-of-range footprint texel folds the nearest edge texel.
            const auto At = [&](i32 x, i32 y)
            {
                const u32 cx = static_cast<u32>(std::clamp(x, 0, static_cast<i32>(src.x) - 1));
                const u32 cy = static_cast<u32>(std::clamp(y, 0, static_cast<i32>(src.y) - 1));
                return in[static_cast<size_t>(cy) * src.x + cx];
            };

            for (u32 y = 0; y < dst.y; ++y)
            {
                for (u32 x = 0; x < dst.x; ++x)
                {
                    const i32 bx = static_cast<i32>(x) * 2;
                    const i32 by = static_cast<i32>(y) * 2;
                    float m = At(bx, by);
                    m = std::max(m, At(bx + 1, by));
                    m = std::max(m, At(bx, by + 1));
                    m = std::max(m, At(bx + 1, by + 1));
                    if (oddX)
                    {
                        m = std::max(m, At(bx + 2, by));
                        m = std::max(m, At(bx + 2, by + 1));
                    }
                    if (oddY)
                    {
                        m = std::max(m, At(bx, by + 2));
                        m = std::max(m, At(bx + 1, by + 2));
                    }
                    if (oddX && oddY)
                    {
                        m = std::max(m, At(bx + 2, by + 2));
                    }
                    out[static_cast<size_t>(y) * dst.x + x] = m;
                }
            }
        }
        return levels;
    }

    // Reads back one R32 mip level into a row-major float vector. PrepareForAccess
    // transitions the per-mip view to TransferSrc through the engine's tracked-state
    // path (so the barrier's old layout matches whatever the reduction left this mip
    // in — ShaderReadOnly for a source mip, General for the coarsest); the raw
    // vkCmdCopyImageToBuffer then copies at that mip (Image::Download is mip-0 only).
    std::vector<float> DownloadMip(Context& context, const Ref<Image>& image,
                                   const Ref<ImageView>& mipView, u32 level, u32 w, u32 h)
    {
        auto buffer = Buffer::Create(context, {
                                                  .Name = "HiZ Mip Readback",
                                                  .Size = static_cast<u64>(w) * h * sizeof(float),
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
                    .imageExtent = {.width = w, .height = h, .depth = 1},
                };
                GetVkCommandBuffer(cmd).copyImageToBuffer(GetVkImage(*image),
                                                          vk::ImageLayout::eTransferSrcOptimal,
                                                          GetVkBuffer(*buffer), 1, &region);
            });

        const std::vector<u8> bytes = buffer->Download();
        std::vector<float> out(static_cast<size_t>(w) * h);
        std::memcpy(out.data(), bytes.data(), out.size() * sizeof(float));
        return out;
    }
}

TEST_CASE_FIXTURE(Test::GpuFixture, "Hi-Z reduction matches the CPU reference (odd extent)")
{
    // An odd extent on both axes exercises the 3x3 odd-mip fold at the first
    // reduction. 13x7 -> 6x3 -> 3x1 -> 1x1 (mip 0 is the 13x7 copy).
    constexpr u32 W = 13;
    constexpr u32 H = 7;

    AssetManager assets(Context, Tasks, Types);
    const AssetResult<AssetHandle<Shader>> reduceCs = assets.LoadSync<Shader>(HiZReduceCompId);
    REQUIRE_MESSAGE(reduceCs.has_value(), "load hi_z_reduce.comp from the core pack");

    // A varied depth field, plus a single far texel (1.0) buried in an otherwise
    // near (0.0) region — the conservatism probe: that far value must survive to
    // every coarser mip covering it.
    std::vector<float> field(static_cast<size_t>(W) * H);
    for (u32 y = 0; y < H; ++y)
    {
        for (u32 x = 0; x < W; ++x)
        {
            field[static_cast<size_t>(y) * W + x] =
                0.1f + 0.05f * static_cast<float>((x * 3 + y * 7) % 11);
        }
    }
    field[0] = 0.0f; // a near texel at the origin's 2x2
    field[1] = 1.0f; // the far texel that must propagate up the chain

    const u32 mips = MipCountFor(W, H);

    auto source = Image::Create(Context, {
                                             .Name = "HiZ Source Depth Field",
                                             .Extent = {W, H, 1},
                                             .Format = Format::R32Sfloat,
                                             .Usage = ImageUsage::Sampled | ImageUsage::TransferDst,
                                         });
    source->UploadSync(
        std::span(reinterpret_cast<const u8*>(field.data()), field.size() * sizeof(float)));

    auto sourceView = ImageView::Create(Context, {.Name = "HiZ Source View", .Image = source});

    auto hiZ = Image::Create(
        Context, {
                     .Name = "HiZ Pyramid",
                     .Extent = {W, H, 1},
                     .MipLevels = mips,
                     .Format = Format::R32Sfloat,
                     .Usage = ImageUsage::Storage | ImageUsage::Sampled | ImageUsage::TransferSrc,
                 });
    std::vector<Ref<ImageView>> mipViews;
    for (u32 k = 0; k < mips; ++k)
    {
        mipViews.push_back(ImageView::Create(
            Context, {.Name = "HiZ Mip View", .Image = hiZ, .BaseMipLevel = k, .MipLevels = 1}));
    }

    // Reduction set layout: binding 0 sampled source, binding 1 storage dest.
    auto setLayout =
        DescriptorSetLayout::Create(Context, {
                                                 .Name = "HiZ Reduce Set Layout",
                                                 .Bindings =
                                                     {
                                                         {.Binding = 0,
                                                          .Type = DescriptorType::SampledImage,
                                                          .Count = 1,
                                                          .Stages = ShaderStage::Compute},
                                                         {.Binding = 1,
                                                          .Type = DescriptorType::StorageImage,
                                                          .Count = 1,
                                                          .Stages = ShaderStage::Compute},
                                                     },
                                             });
    auto layout = PipelineLayout::Create(
        Context,
        {
            .Name = "HiZ Reduce Layout",
            .DescriptorSetLayouts = {setLayout},
            .PushConstantRanges = {PushConstantRange::Of<HiZReducePush>(ShaderStage::Compute)},
        });
    auto pipeline = ComputePipeline::Create(
        Context,
        {
            .Name = "HiZ Reduce Pipeline",
            .PipelineLayout = layout,
            .ShaderStage = {.Stage = ShaderStage::Compute, .Module = reduceCs->Get()->Module},
        });

    std::vector<Ref<DescriptorSet>> sets;
    for (u32 k = 0; k < mips; ++k)
    {
        auto set = DescriptorSet::Create(Context, {.Name = "HiZ Reduce Set", .Layout = setLayout});
        set->Write(0, k == 0 ? sourceView : mipViews[k - 1]);
        set->Write(1, mipViews[k]);
        sets.push_back(std::move(set));
    }

    // The reduction chain through the per-mip graph surface: dispatch k reads mip
    // k's source (the depth field for k=0, hi-Z mip k-1 otherwise) and writes mip k.
    Context.ImmediateCommands(
        [&](CommandBuffer& cmd)
        {
            RenderGraph graph(Context);
            const ResourceId sourceId = graph.Import("HiZ Source");
            const MipChainId chain = graph.ImportImageMips("HiZ", mips);

            for (u32 k = 0; k < mips; ++k)
            {
                const u32 dstW = std::max(W >> k, 1u);
                const u32 dstH = std::max(H >> k, 1u);
                const u32 srcW = k == 0 ? W : std::max(W >> (k - 1), 1u);
                const u32 srcH = k == 0 ? H : std::max(H >> (k - 1), 1u);

                RenderGraph::PassBuilder builder = graph.AddComputePass("HiZ Reduce");
                if (k == 0)
                {
                    builder.Sample(sourceId);
                }
                else
                {
                    builder.Sample(chain.Level(k - 1));
                }
                builder.StorageWrite(chain.Level(k));

                const Ref<ComputePipeline> pl = pipeline;
                const Ref<DescriptorSet> set = sets[k];
                const HiZReducePush push{.DestExtent = {dstW, dstH}, .SourceExtent = {srcW, srcH}};
                builder.Execute(
                    [pl, set, push](PassContext& ctx)
                    {
                        CommandBuffer& c = ctx.Cmd();
                        c.BindPipeline(pl);
                        c.BindDescriptorSets(DescriptorSetBindInfo{
                            .Sets = {set},
                            .FirstSet = 1,
                            .PipelineBindPoint = PipelineBindPoint::Compute,
                        });
                        c.PushConstants(push);
                        c.Dispatch((push.DestExtent.x + 7) / 8, (push.DestExtent.y + 7) / 8, 1);
                    });
            }

            std::vector<RenderGraph::ImportBinding> bindings;
            bindings.push_back({sourceId, sourceView});
            for (u32 k = 0; k < mips; ++k)
            {
                bindings.push_back({chain.Level(k), mipViews[k]});
            }
            graph.Compile()->Execute(cmd, bindings);
        });

    const std::vector<std::vector<float>> reference = ReferenceReduce(field, W, H);

    uvec2 extent{W, H};
    for (u32 k = 0; k < mips; ++k)
    {
        const std::vector<float> got =
            DownloadMip(Context, hiZ, mipViews[k], k, extent.x, extent.y);
        REQUIRE(got.size() == reference[k].size());
        for (size_t i = 0; i < got.size(); ++i)
        {
            CHECK(got[i] == doctest::Approx(reference[k][i]).epsilon(1e-5));
        }
        extent = {std::max(extent.x >> 1, 1u), std::max(extent.y >> 1, 1u)};
    }

    // The conservatism probe: the far texel (1.0) at (1,0) sits in mip 0's origin
    // 2x2, so every coarser mip texel covering the origin must be >= 1.0 - eps.
    for (u32 k = 1; k < mips; ++k)
    {
        const u32 w = std::max(W >> k, 1u);
        const u32 h = std::max(H >> k, 1u);
        const std::vector<float> got = DownloadMip(Context, hiZ, mipViews[k], k, w, h);
        CHECK(got[0] == doctest::Approx(1.0f).epsilon(1e-5));
    }
}
