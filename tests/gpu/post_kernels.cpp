// Two post-chain compute kernels driven directly on exact content:
//
//   - the exposure meter's histogram counts every texel of a known image in its own bin and
//     nowhere else, each workgroup merging only the bins it touched;
//   - the anamorphic streak's row filter, scanned across a workgroup, gives each texel the forward
//     and backward first-order recurrences y[i] = r·y[i−1] + (1 − r)·x[i] combine to
//     (StreakFilterRow), to the half-float destination's precision.

#include <array>
#include <cmath>
#include <cstring>
#include <span>
#include <vector>

#include <doctest/doctest.h>
#include <glm/gtc/packing.hpp>

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
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Renderer/Types.h>
#include <Veng/Scene/BuiltinTypes.h>

#include <gpu/fixture.h>

#include "Renderer/StreakKernel.h"

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    constexpr AssetId AutoExposureCompId{0x9BF9CB926D0595B1ULL};
    constexpr AssetId StreakFilterCompId{0x40913DFFB93BA89FULL};

    // Matches auto_exposure.comp.slang's push block.
    struct AutoExposurePush
    {
        vec2 SourceScaleUV;
        vec2 SourceMaxUV;
        uvec2 ValidExtent;
        vec2 OrmScaleUV;
        f32 MinLogLum;
        f32 MaxLogLum;
        u32 HistogramBase;
        u32 ReadsFlags;
    };

    // Matches anamorphic_streak_filter.comp.slang's push block.
    struct StreakFilterPush
    {
        uvec2 Extent;
        uvec2 Padding;
        vec4 Ratio;
    };

    Ref<Image> UploadHalfImage(Renderer::Context& context, const uvec2 extent,
                               const std::vector<vec3>& texels, const ImageUsage extra)
    {
        std::vector<u16> halves(texels.size() * 4);
        for (usize i = 0; i < texels.size(); ++i)
        {
            for (u32 c = 0; c < 3; ++c)
            {
                halves[i * 4 + c] = glm::packHalf1x16(texels[i][static_cast<i32>(c)]);
            }
            halves[i * 4 + 3] = glm::packHalf1x16(1.0f);
        }
        const Ref<Image> image = Image::Create(
            context, {
                         .Name = "Post Kernel Source",
                         .Extent = {extent.x, extent.y, 1},
                         .Format = Format::RGBA16Sfloat,
                         .Usage = ImageUsage::Sampled | ImageUsage::TransferDst | extra,
                     });
        image->UploadSync(
            std::span(reinterpret_cast<const u8*>(halves.data()), halves.size() * sizeof(u16)));
        return image;
    }

    Ref<DescriptorSetLayout> ComputeSetLayout(Renderer::Context& context, const char* name,
                                              const vector<DescriptorType>& types)
    {
        DescriptorSetLayoutInfo info{.Name = name};
        for (u32 binding = 0; binding < types.size(); ++binding)
        {
            info.Bindings.push_back({.Binding = binding,
                                     .Type = types[binding],
                                     .Count = 1,
                                     .Stages = ShaderStage::Compute});
        }
        return DescriptorSetLayout::Create(context, info);
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "post kernels: the exposure histogram counts each texel of a known image once")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);

    // Grey levels at powers of two, each well inside a bin, and black for bin 0; every 16×16
    // workgroup sees a different mix, and most bins of every group stay empty.
    constexpr uvec2 Extent{32, 32};
    constexpr f32 MinLogLum = -8.0f;
    constexpr f32 MaxLogLum = 4.0f;
    const std::array<f32, 5> levels{0.0f, 0x1p-7f, 0x1p-4f, 0x1p-1f, 0x1p2f};
    const auto binOf = [&](const f32 lum)
    {
        if (lum <= 0.0f)
        {
            return 0u;
        }
        const f32 t = (std::log2(lum) - MinLogLum) / (MaxLogLum - MinLogLum);
        return static_cast<u32>(t * 254.0f + 1.0f);
    };
    std::vector<vec3> texels(static_cast<usize>(Extent.x) * Extent.y);
    std::array<u32, 256> expected{};
    for (u32 y = 0; y < Extent.y; ++y)
    {
        for (u32 x = 0; x < Extent.x; ++x)
        {
            const f32 lum = levels[(x / 5 + y / 16 + x * y % 3) % levels.size()];
            texels[static_cast<usize>(y) * Extent.x + x] = vec3(lum);
            ++expected[binOf(lum)];
        }
    }

    const Ref<Image> source = UploadHalfImage(Context, Extent, texels, ImageUsage{});
    const Ref<ImageView> sourceView =
        ImageView::Create(Context, {.Name = "Histogram Source View", .Image = source});
    const Ref<Buffer> histogram = Buffer::Create(Context, {.Name = "Histogram",
                                                           .Size = 256 * sizeof(u32),
                                                           .Usage = BufferUsage::Storage,
                                                           .HostMapped = true});
    std::memset(histogram->GetMappedData(), 0, 256 * sizeof(u32));
    const Ref<Sampler> sampler = Sampler::Create(Context, {.Name = "Histogram Sampler",
                                                           .MagFilter = Filter::Linear,
                                                           .MinFilter = Filter::Linear,
                                                           .AddressModeU = AddressMode::ClampToEdge,
                                                           .AddressModeV = AddressMode::ClampToEdge,
                                                           .AnisotropyEnabled = false});

    const Ref<DescriptorSetLayout> setLayout =
        ComputeSetLayout(Context, "Histogram Set Layout",
                         {DescriptorType::SampledImage, DescriptorType::Sampler,
                          DescriptorType::StorageBuffer, DescriptorType::SampledImage});
    const Ref<PipelineLayout> layout = PipelineLayout::Create(
        Context,
        {.Name = "Histogram Layout",
         .DescriptorSetLayouts = {setLayout},
         .PushConstantRanges = {PushConstantRange::Of<AutoExposurePush>(ShaderStage::Compute)}});
    const AssetResult<AssetHandle<Shader>> shader = assets.LoadSync<Shader>(AutoExposureCompId);
    REQUIRE(shader.has_value());
    const Ref<ComputePipeline> pipeline = ComputePipeline::Create(
        Context, {.Name = "Histogram Pipeline",
                  .PipelineLayout = layout,
                  .ShaderStage = {.Stage = ShaderStage::Compute, .Module = shader->Get()->Module}});
    const Ref<DescriptorSet> set =
        DescriptorSet::Create(Context, {.Name = "Histogram Set", .Layout = setLayout});
    set->Write(0, sourceView);
    set->Write(1, sampler);
    set->Write(2, histogram);
    set->Write(3, sourceView);

    Context.ImmediateCommands(
        [&](CommandBuffer& cmd)
        {
            RenderGraph graph(Context);
            const ResourceId sourceId = graph.Import("Histogram Source");
            const ResourceId histogramId = graph.ImportBuffer("Histogram");
            graph.AddComputePass("Histogram")
                .Sample(sourceId)
                .StorageBufferWrite(histogramId)
                .Execute(
                    [&](PassContext& ctx)
                    {
                        CommandBuffer& c = ctx.Cmd();
                        c.BindPipeline(pipeline);
                        c.BindDescriptorSets(DescriptorSetBindInfo{
                            .Sets = {set},
                            .FirstSet = 3,
                            .PipelineBindPoint = PipelineBindPoint::Compute,
                        });
                        c.PushConstants(AutoExposurePush{
                            .SourceScaleUV = vec2(1.0f),
                            .SourceMaxUV = (vec2(Extent) - 0.5f) / vec2(Extent),
                            .ValidExtent = Extent,
                            .OrmScaleUV = vec2(1.0f),
                            .MinLogLum = MinLogLum,
                            .MaxLogLum = MaxLogLum,
                            .HistogramBase = 0,
                            .ReadsFlags = 0,
                        });
                        c.Dispatch(Extent.x / 16, Extent.y / 16, 1);
                    });
            graph.Compile()->Execute(cmd, {{.Id = sourceId, .View = sourceView},
                                           {.Id = histogramId, .Buffer = histogram}});
        });

    std::array<u32, 256> counted{};
    std::memcpy(counted.data(), histogram->GetMappedData(), sizeof(counted));
    CHECK(counted == expected);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "post kernels: the scanned streak row satisfies its two recurrences")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);

    // Wider than the workgroup, so runs span several texels and the last threads hold a short run
    // or none; one bright source near each end, one in the middle, and a low floor.
    constexpr uvec2 Extent{700, 3};
    std::vector<vec3> texels(static_cast<usize>(Extent.x) * Extent.y, vec3(0.05f));
    for (u32 y = 0; y < Extent.y; ++y)
    {
        for (const u32 x : {3u, 351u + y * 40u, 690u})
        {
            texels[static_cast<usize>(y) * Extent.x + x] = vec3(8.0f, 4.0f, 2.0f);
        }
    }
    const vec3 ratio = StreakChannelRatio(0.1f, 0.4f, Extent.x);

    const Ref<Image> source = UploadHalfImage(Context, Extent, texels, ImageUsage{});
    const Ref<ImageView> sourceView =
        ImageView::Create(Context, {.Name = "Streak Source View", .Image = source});
    const Ref<Image> dest =
        Image::Create(Context, {.Name = "Streak Dest",
                                .Extent = {Extent.x, Extent.y, 1},
                                .Format = Format::RGBA16Sfloat,
                                .Usage = ImageUsage::Storage | ImageUsage::TransferSrc});
    const Ref<ImageView> destView =
        ImageView::Create(Context, {.Name = "Streak Dest View", .Image = dest});
    const Ref<Sampler> sampler = Sampler::Create(Context, {.Name = "Streak Sampler"});

    const Ref<DescriptorSetLayout> setLayout = ComputeSetLayout(
        Context, "Streak Set Layout",
        {DescriptorType::SampledImage, DescriptorType::Sampler, DescriptorType::StorageImage});
    const Ref<PipelineLayout> layout = PipelineLayout::Create(
        Context,
        {.Name = "Streak Layout",
         .DescriptorSetLayouts = {setLayout},
         .PushConstantRanges = {PushConstantRange::Of<StreakFilterPush>(ShaderStage::Compute)}});
    const AssetResult<AssetHandle<Shader>> shader = assets.LoadSync<Shader>(StreakFilterCompId);
    REQUIRE(shader.has_value());
    const Ref<ComputePipeline> pipeline = ComputePipeline::Create(
        Context, {.Name = "Streak Pipeline",
                  .PipelineLayout = layout,
                  .ShaderStage = {.Stage = ShaderStage::Compute, .Module = shader->Get()->Module}});
    const Ref<DescriptorSet> set =
        DescriptorSet::Create(Context, {.Name = "Streak Set", .Layout = setLayout});
    set->Write(0, sourceView);
    set->Write(1, sampler);
    set->Write(2, destView);

    Context.ImmediateCommands(
        [&](CommandBuffer& cmd)
        {
            RenderGraph graph(Context);
            const ResourceId sourceId = graph.Import("Streak Source");
            const ResourceId destId = graph.Import("Streak Dest");
            graph.AddComputePass("Streak Filter")
                .Sample(sourceId)
                .StorageWrite(destId)
                .Execute(
                    [&](PassContext& ctx)
                    {
                        CommandBuffer& c = ctx.Cmd();
                        c.BindPipeline(pipeline);
                        c.BindDescriptorSets(DescriptorSetBindInfo{
                            .Sets = {set},
                            .FirstSet = 3,
                            .PipelineBindPoint = PipelineBindPoint::Compute,
                        });
                        c.PushConstants(StreakFilterPush{
                            .Extent = Extent, .Padding = uvec2(0), .Ratio = vec4(ratio, 0.0f)});
                        c.Dispatch(Extent.y, 1, 1);
                    });
            graph.Compile()->Execute(
                cmd, {{.Id = sourceId, .View = sourceView}, {.Id = destId, .View = destView}});
        });

    const vector<u8> bytes = dest->Download();
    REQUIRE(bytes.size() == static_cast<usize>(Extent.x) * Extent.y * 8);
    std::vector<u16> halves(bytes.size() / sizeof(u16));
    std::memcpy(halves.data(), bytes.data(), bytes.size());

    // The worst error against the sequential sweeps, relative to the row's brightest result.
    f32 worst = 0.0f;
    f32 brightest = 0.0f;
    std::vector<f32> row(Extent.x);
    std::vector<f32> scratch;
    for (u32 y = 0; y < Extent.y; ++y)
    {
        for (u32 c = 0; c < 3; ++c)
        {
            for (u32 x = 0; x < Extent.x; ++x)
            {
                row[x] = glm::unpackHalf1x16(glm::packHalf1x16(
                    texels[static_cast<usize>(y) * Extent.x + x][static_cast<i32>(c)]));
            }
            StreakFilterRow(row, ratio[static_cast<i32>(c)], scratch);
            for (u32 x = 0; x < Extent.x; ++x)
            {
                const f32 got =
                    glm::unpackHalf1x16(halves[(static_cast<usize>(y) * Extent.x + x) * 4 + c]);
                worst = std::max(worst, std::abs(got - row[x]));
                brightest = std::max(brightest, row[x]);
            }
        }
    }
    CHECK(brightest > 0.05f);
    CHECK(worst <= 4.0e-3f * brightest);
}
