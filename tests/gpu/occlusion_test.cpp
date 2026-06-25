// Hi-Z occlusion-test primitive, proven in isolation. The core pack's
// hi_z_occlusion_test.comp runs IsOccluded (hi_z_occlusion.slang) per candidate
// against a known pyramid + view-projection and writes a per-candidate visibility
// flag (1 = visible/draw, 0 = occluded) to a result buffer this test reads back.
// No draw issues — this pins the math before the GPU cull wires it into indirect
// draw.
//
// The scene: a half-covered max-Z depth field — the left screen half is a near
// occluder (small stored depth), the right half is empty background (far depth 1).
// The field is reduced into a real pyramid through the core reduction, then world
// AABBs are projected by a perspective camera and tested:
//   - behind the occluder, footprint inside the left half      -> occluded (0)
//   - behind the occluder but straddling into the background   -> visible  (1, conservative)
//   - in front of the occluder                                 -> visible  (1)
//   - at the occluder depth (a tie)                            -> visible  (1, conservative)
//   - crossing the near plane                                  -> visible  (1, conservative)
//   - footprint off-screen                                     -> visible  (1)
//
// Skips (exit 77) with no Vulkan ICD.

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <vector>

#include <doctest/doctest.h>

#include <glm/gtc/matrix_transform.hpp>

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
#include <Veng/Renderer/Types.h>
#include <Veng/Scene/Camera.h>

#include <gpu/fixture.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    // The core pack's hi-Z reduction and isolation occlusion-test compute shaders.
    constexpr AssetId HiZReduceCompId{0xCB20C4EF8A20ADBCULL};
    constexpr AssetId HiZOcclusionTestCompId{0x40A58AE8E1B3135EULL};

    constexpr u32 FieldWidth = 256;
    constexpr u32 FieldHeight = 256;

    // The occluder plane sits this far from the camera; its stored NDC depth is
    // derived from the camera projection (Vulkan ZO depth is nonlinear, so a fixed
    // depth literal would not correspond to a meaningful distance). The far
    // background in the right half stores depth 1.
    constexpr float OccluderDistance = 1.0f;
    constexpr float BackgroundDepth = 1.0f;

    struct ReducePush
    {
        uvec2 DestExtent;
        uvec2 SourceExtent;
    };

    struct Candidate
    {
        vec4 BoundsMin;
        vec4 BoundsMax;
    };

    struct OcclusionPush
    {
        mat4 PrevViewProj;
        uvec2 HiZBaseExtent;
        u32 CandidateCount;
        f32 DepthBias;
    };

    u32 MipCountFor(u32 w, u32 h)
    {
        const u32 maxDim = std::max(w, h);
        return maxDim == 0 ? 1 : std::bit_width(maxDim);
    }

    // Projects a world point through viewProj into screen UV [0,1] and Vulkan clip
    // depth [0,1]. Mirrors the shader so the test can assert each candidate lands in
    // the screen region it intends before trusting the GPU verdict.
    struct Projected
    {
        vec2 Uv;
        f32 Depth;
        f32 W;
    };
    Projected Project(const mat4& viewProj, vec3 world)
    {
        const vec4 clip = viewProj * vec4(world, 1.0f);
        const vec3 ndc = vec3(clip) / clip.w;
        return Projected{.Uv = vec2(ndc) * 0.5f + 0.5f, .Depth = ndc.z, .W = clip.w};
    }

    // Reduces a row-major R32 depth field (extent w×h) into a real max-Z pyramid through
    // the core reduction, runs the isolation occlusion test over the candidates against
    // it with the given previous-frame view-projection, and returns one visibility flag
    // per candidate (1 = drawn, 0 = occluded). The GPU plumbing every occlusion case shares.
    std::vector<u32> RunOcclusion(Test::GpuFixture& fix, u32 w, u32 h,
                                  const std::vector<float>& field,
                                  const std::vector<Candidate>& candidates,
                                  const mat4& prevViewProj, f32 depthBias)
    {
        AssetManager assets(fix.Context, fix.Tasks, fix.Types);
        const AssetResult<AssetHandle<Shader>> reduceCs = assets.LoadSync<Shader>(HiZReduceCompId);
        REQUIRE_MESSAGE(reduceCs.has_value(), "load hi_z_reduce.comp from the core pack");
        const AssetResult<AssetHandle<Shader>> occlusionCs =
            assets.LoadSync<Shader>(HiZOcclusionTestCompId);
        REQUIRE_MESSAGE(occlusionCs.has_value(),
                        "load hi_z_occlusion_test.comp from the core pack");

        const u32 mips = MipCountFor(w, h);

        auto source =
            Image::Create(fix.Context, {
                                           .Name = "Occlusion Source Depth",
                                           .Extent = {w, h, 1},
                                           .Format = Format::R32Sfloat,
                                           .Usage = ImageUsage::Sampled | ImageUsage::TransferDst,
                                       });
        source->UploadSync(
            std::span(reinterpret_cast<const u8*>(field.data()), field.size() * sizeof(float)));
        auto sourceView =
            ImageView::Create(fix.Context, {.Name = "Occlusion Source View", .Image = source});

        auto hiZ =
            Image::Create(fix.Context, {
                                           .Name = "Occlusion HiZ",
                                           .Extent = {w, h, 1},
                                           .MipLevels = mips,
                                           .Format = Format::R32Sfloat,
                                           .Usage = ImageUsage::Storage | ImageUsage::Sampled,
                                       });
        std::vector<Ref<ImageView>> mipViews;
        for (u32 k = 0; k < mips; ++k)
        {
            mipViews.push_back(ImageView::Create(
                fix.Context,
                {.Name = "Occlusion HiZ Mip", .Image = hiZ, .BaseMipLevel = k, .MipLevels = 1}));
        }
        auto hiZSampleView = ImageView::Create(
            fix.Context, {.Name = "Occlusion HiZ Sample", .Image = hiZ, .MipLevels = mips});

        auto reduceSetLayout = DescriptorSetLayout::Create(
            fix.Context, {
                             .Name = "Occlusion Reduce Set Layout",
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
        auto reduceLayout = PipelineLayout::Create(
            fix.Context,
            {
                .Name = "Occlusion Reduce Layout",
                .DescriptorSetLayouts = {reduceSetLayout},
                .PushConstantRanges = {PushConstantRange::Of<ReducePush>(ShaderStage::Compute)},
            });
        auto reducePipeline = ComputePipeline::Create(
            fix.Context,
            {
                .Name = "Occlusion Reduce Pipeline",
                .PipelineLayout = reduceLayout,
                .ShaderStage = {.Stage = ShaderStage::Compute, .Module = reduceCs->Get()->Module},
            });
        std::vector<Ref<DescriptorSet>> reduceSets;
        for (u32 k = 0; k < mips; ++k)
        {
            auto set = DescriptorSet::Create(
                fix.Context, {.Name = "Occlusion Reduce Set", .Layout = reduceSetLayout});
            set->Write(0, k == 0 ? sourceView : mipViews[k - 1]);
            set->Write(1, mipViews[k]);
            reduceSets.push_back(std::move(set));
        }

        auto candidateBuffer = Buffer::Create(
            fix.Context, {
                             .Name = "Occlusion Candidates",
                             .Size = candidates.size() * sizeof(Candidate),
                             .Usage = BufferUsage::Storage | BufferUsage::TransferDst,
                         });
        candidateBuffer->UploadSync(std::span(reinterpret_cast<const u8*>(candidates.data()),
                                              candidates.size() * sizeof(Candidate)));

        auto visibilityBuffer = Buffer::Create(
            fix.Context, {
                             .Name = "Occlusion Visibility",
                             .Size = candidates.size() * sizeof(u32),
                             .Usage = BufferUsage::Storage | BufferUsage::TransferSrc,
                         });

        auto occlusionSetLayout = DescriptorSetLayout::Create(
            fix.Context, {
                             .Name = "Occlusion Test Set Layout",
                             .Bindings =
                                 {
                                     {.Binding = 0,
                                      .Type = DescriptorType::SampledImage,
                                      .Count = 1,
                                      .Stages = ShaderStage::Compute},
                                     {.Binding = 1,
                                      .Type = DescriptorType::StorageBuffer,
                                      .Count = 1,
                                      .Stages = ShaderStage::Compute},
                                     {.Binding = 2,
                                      .Type = DescriptorType::StorageBuffer,
                                      .Count = 1,
                                      .Stages = ShaderStage::Compute},
                                 },
                         });
        auto occlusionLayout = PipelineLayout::Create(
            fix.Context,
            {
                .Name = "Occlusion Test Layout",
                .DescriptorSetLayouts = {occlusionSetLayout},
                .PushConstantRanges = {PushConstantRange::Of<OcclusionPush>(ShaderStage::Compute)},
            });
        auto occlusionPipeline = ComputePipeline::Create(
            fix.Context, {
                             .Name = "Occlusion Test Pipeline",
                             .PipelineLayout = occlusionLayout,
                             .ShaderStage = {.Stage = ShaderStage::Compute,
                                             .Module = occlusionCs->Get()->Module},
                         });
        auto occlusionSet = DescriptorSet::Create(
            fix.Context, {.Name = "Occlusion Test Set", .Layout = occlusionSetLayout});
        occlusionSet->Write(0, hiZSampleView);
        occlusionSet->Write(1, candidateBuffer);
        occlusionSet->Write(2, visibilityBuffer);

        const OcclusionPush occlusionPush{
            .PrevViewProj = prevViewProj,
            .HiZBaseExtent = {w, h},
            .CandidateCount = static_cast<u32>(candidates.size()),
            .DepthBias = depthBias,
        };

        fix.Context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                RenderGraph graph(fix.Context);
                const ResourceId sourceId = graph.Import("Occlusion Source");
                const MipChainId chain = graph.ImportImageMips("Occlusion HiZ", mips);

                for (u32 k = 0; k < mips; ++k)
                {
                    const u32 dstW = std::max(w >> k, 1u);
                    const u32 dstH = std::max(h >> k, 1u);
                    const u32 srcW = k == 0 ? w : std::max(w >> (k - 1), 1u);
                    const u32 srcH = k == 0 ? h : std::max(h >> (k - 1), 1u);

                    RenderGraph::PassBuilder builder = graph.AddComputePass("Occlusion Reduce");
                    if (k == 0)
                    {
                        builder.Sample(sourceId);
                    }
                    else
                    {
                        builder.Sample(chain.Level(k - 1));
                    }
                    builder.StorageWrite(chain.Level(k));

                    const Ref<ComputePipeline> pl = reducePipeline;
                    const Ref<DescriptorSet> set = reduceSets[k];
                    const ReducePush push{.DestExtent = {dstW, dstH}, .SourceExtent = {srcW, srcH}};
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

                RenderGraph::PassBuilder testPass = graph.AddComputePass("Occlusion Test");
                for (u32 k = 0; k < mips; ++k)
                {
                    testPass.Sample(chain.Level(k));
                }
                const Ref<ComputePipeline> pl = occlusionPipeline;
                const Ref<DescriptorSet> set = occlusionSet;
                const OcclusionPush push = occlusionPush;
                const u32 count = static_cast<u32>(candidates.size());
                testPass.Execute(
                    [pl, set, push, count](PassContext& ctx)
                    {
                        CommandBuffer& c = ctx.Cmd();
                        c.BindPipeline(pl);
                        c.BindDescriptorSets(DescriptorSetBindInfo{
                            .Sets = {set},
                            .FirstSet = 1,
                            .PipelineBindPoint = PipelineBindPoint::Compute,
                        });
                        c.PushConstants(push);
                        c.Dispatch((count + 63) / 64, 1, 1);
                    });

                std::vector<RenderGraph::ImportBinding> bindings;
                bindings.push_back({sourceId, sourceView});
                for (u32 k = 0; k < mips; ++k)
                {
                    bindings.push_back({chain.Level(k), mipViews[k]});
                }
                graph.Compile()->Execute(cmd, bindings);
            });

        const std::vector<u8> bytes = visibilityBuffer->Download();
        std::vector<u32> visibility(candidates.size());
        std::memcpy(visibility.data(), bytes.data(), visibility.size() * sizeof(u32));
        return visibility;
    }
}

TEST_CASE_FIXTURE(Test::GpuFixture, "Hi-Z occlusion test reports occluded vs visible candidates")
{
    // The previous-frame camera: at the origin looking down -Z, Y-flipped Vulkan clip.
    CameraView camera;
    camera.SetPerspective(glm::radians(60.0f), static_cast<f32>(FieldWidth) / FieldHeight, 0.1f,
                          100.0f);
    camera.SetView(vec3(0.0f), vec3(0.0f, 0.0f, -1.0f), vec3(0.0f, 1.0f, 0.0f));
    const mat4 viewProj = camera.ViewProjection();

    // The occluder's stored NDC depth, from a point on the plane at OccluderDistance.
    const float occluderDepth = Project(viewProj, vec3(0.0f, 0.0f, -OccluderDistance)).Depth;

    // The half-covered depth field: left half a near occluder, right half background.
    std::vector<float> field(static_cast<size_t>(FieldWidth) * FieldHeight);
    for (u32 y = 0; y < FieldHeight; ++y)
    {
        for (u32 x = 0; x < FieldWidth; ++x)
        {
            field[static_cast<size_t>(y) * FieldWidth + x] =
                x < FieldWidth / 2 ? occluderDepth : BackgroundDepth;
        }
    }

    // ---- Candidates: one AABB per intended outcome. ----
    // A small axis-aligned cube spanning [center - h, center + h].
    const auto cube = [](vec3 center, f32 h)
    {
        return Candidate{.BoundsMin = vec4(center - vec3(h), 0.0f),
                         .BoundsMax = vec4(center + vec3(h), 0.0f)};
    };

    // Left-half world x is negative under this Y-flip-free x mapping: x<0 -> uv.x<0.5.
    // Pick centers and verify their projected footprint before trusting the GPU.
    const Candidate occluded = cube(vec3(-0.4f, 0.0f, -20.0f), 0.1f); // deep, left half
    const Candidate straddle = cube(vec3(0.0f, 0.0f, -20.0f), 1.0f);  // deep, spans x=0.5
    const Candidate inFront = cube(vec3(-0.2f, 0.0f, -0.5f), 0.05f);  // nearer than the occluder
    const Candidate atOccluder =
        cube(vec3(-0.2f, 0.0f, -OccluderDistance), 0.02f); // at the occluder depth (a tie)
    const Candidate nearCross = cube(vec3(0.0f, 0.0f, 0.05f), 0.5f);     // straddles the near plane
    const Candidate offScreen = cube(vec3(-200.0f, 0.0f, -20.0f), 0.5f); // far off the left edge

    const std::vector<Candidate> candidates = {occluded,   straddle,  inFront,
                                               atOccluder, nearCross, offScreen};
    enum Index
    {
        Occluded = 0,
        Straddle,
        InFront,
        AtOccluder,
        NearCross,
        OffScreen,
    };

    // Construction sanity: the occluded candidate's nearest corner must project deeper
    // than the occluder, fully inside the left half; the in-front nearer.
    {
        // Nearest corner = largest z (closest to 0) of the AABB = BoundsMax.z. The
        // occluded candidate's whole footprint must sit inside the left half and project
        // deeper than the occluder.
        const Projected nMin = Project(
            viewProj, vec3(occluded.BoundsMin.x, occluded.BoundsMin.y, occluded.BoundsMax.z));
        const Projected nMax = Project(
            viewProj, vec3(occluded.BoundsMax.x, occluded.BoundsMax.y, occluded.BoundsMax.z));
        REQUIRE(nMin.Depth > occluderDepth);
        REQUIRE(nMax.Uv.x < 0.5f);
    }
    {
        const Projected n =
            Project(viewProj, vec3(inFront.BoundsMin.x, inFront.BoundsMin.y, inFront.BoundsMax.z));
        REQUIRE(n.Depth < occluderDepth);
    }

    // Small bias: enough to make the tie draw, below the occluder/background gap.
    const std::vector<u32> visibility =
        RunOcclusion(*this, FieldWidth, FieldHeight, field, candidates, viewProj, 0.001f);

    // The one candidate that is provably hidden is reported occluded; every
    // uncertainty resolves to drawn — the conservative correctness bar.
    CHECK(visibility[Occluded] == 0u);
    CHECK(visibility[Straddle] == 1u);
    CHECK(visibility[InFront] == 1u);
    CHECK(visibility[AtOccluder] == 1u);
    CHECK(visibility[NearCross] == 1u);
    CHECK(visibility[OffScreen] == 1u);
}

TEST_CASE_FIXTURE(Test::GpuFixture,
                  "Hi-Z occlusion test draws a screen-filling candidate on a non-pot pyramid")
{
    // A footprint wider than the largest power of two <= maxDim picks a mip one past the
    // pyramid's last level (CeilLog2 overflowing bit_width's range). On a non-power-of-two
    // extent that Load reads out of range — on MoltenVK returning 0 (near), which falsely
    // occludes the one object large enough to hit it: a screen-filling ground plane. The
    // mip pick must clamp to the last real mip, whose single coarse texel is the
    // conservative global max-Z. Width 192 makes a full-screen footprint select that
    // overflow mip (mips run 0..7; 192 > 128 picks 8).
    constexpr u32 NonPotWidth = 192;
    constexpr u32 NonPotHeight = 192;

    CameraView camera;
    camera.SetPerspective(glm::radians(60.0f), static_cast<f32>(NonPotWidth) / NonPotHeight, 0.1f,
                          100.0f);
    camera.SetView(vec3(0.0f), vec3(0.0f, 0.0f, -1.0f), vec3(0.0f, 1.0f, 0.0f));
    const mat4 viewProj = camera.ViewProjection();

    // A uniform far-background field: nothing occludes, so a correct test must draw the
    // candidate (its nearest depth is in front of the far background).
    const std::vector<float> field(static_cast<size_t>(NonPotWidth) * NonPotHeight,
                                   BackgroundDepth);

    // A near face at z=-2 spanning ±0.9 fills ~78% of the screen (≈150px, selecting the
    // overflow mip) while its whole footprint stays inside [0,1] uv — so the test exercises
    // the mip pick rather than early-outing on an off-screen footprint. Nearest corner
    // (z=-2) is well in front of the far background, so the correct verdict is visible.
    const std::vector<Candidate> candidates = {Candidate{
        .BoundsMin = vec4(-0.9f, -0.9f, -10.0f, 0.0f),
        .BoundsMax = vec4(0.9f, 0.9f, -2.0f, 0.0f),
    }};

    // Construction sanity: the footprint stays on-screen (so it is not the off-screen
    // early-out) yet exceeds the largest power of two below the extent.
    {
        const Projected lo = Project(viewProj, vec3(-0.9f, -0.9f, -2.0f));
        const Projected hi = Project(viewProj, vec3(0.9f, 0.9f, -2.0f));
        REQUIRE(lo.Uv.x > 0.0f);
        REQUIRE(hi.Uv.x < 1.0f);
        REQUIRE((hi.Uv.x - lo.Uv.x) * NonPotWidth > 128.0f);
    }

    const std::vector<u32> visibility =
        RunOcclusion(*this, NonPotWidth, NonPotHeight, field, candidates, viewProj, 0.001f);

    CHECK(visibility[0] == 1u);
}
