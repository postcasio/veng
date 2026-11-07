#include "AnamorphicStreak.h"

#include <algorithm>

#include <fmt/format.h>

#include <Veng/Assert.h>
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
#include <Veng/Renderer/Sampler.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Renderer/Types.h>

#include "AutoExposureMeter.h"
#include "BloomMips.h"

namespace Veng::Renderer
{
    namespace
    {
        constexpr AssetId StreakPrefilterCompId{0xF1791BCB2403CDEFULL};
        constexpr AssetId StreakFilterCompId{0x40913DFFB93BA89FULL};

        // Linear float HDR, as the bloom pyramid's.
        constexpr Format StreakFormat = Format::RGBA16Sfloat;

        // The prefilter push: the destination's valid extent, the scene colour's sub-rect map, and
        // the bright pass's threshold and per-tap ceiling in scene units. Matches
        // anamorphic_streak_prefilter.comp.
        struct StreakPrefilterPush
        {
            uvec2 DestExtent;
            vec2 SourceScaleUV;
            vec2 SourceMaxUV;
            f32 Threshold;
            f32 FireflyCeiling;
        };

        // The row filter's push: the valid extent and each channel's ratio. Matches
        // anamorphic_streak_filter.comp.
        struct StreakFilterPush
        {
            uvec2 Extent;
            uvec2 Padding;
            vec4 Ratio;
        };
    }

    Unique<AnamorphicStreak> AnamorphicStreak::Create(Context& context, AssetManager& assets,
                                                      const Ref<DescriptorSetLayout>& setLayout)
    {
        return Unique<AnamorphicStreak>(new AnamorphicStreak(context, assets, setLayout));
    }

    AnamorphicStreak::AnamorphicStreak(Context& context, AssetManager& assets,
                                       const Ref<DescriptorSetLayout>& setLayout)
        : m_Context(context), m_SetLayout(setLayout)
    {
        const auto LoadShader = [&](const AssetId id, const char* what) -> AssetHandle<Veng::Shader>
        {
            const AssetResult<AssetHandle<Veng::Shader>> result = assets.LoadSync<Veng::Shader>(id);
            VE_ASSERT(result.has_value(), "AnamorphicStreak: {} shader load failed: {}", what,
                      result.error().Detail);
            return *result;
        };
        const AssetHandle<Veng::Shader> prefilterCs =
            LoadShader(StreakPrefilterCompId, "streak prefilter");
        const AssetHandle<Veng::Shader> filterCs =
            LoadShader(StreakFilterCompId, "streak row filter");

        m_PrefilterLayout = PipelineLayout::Create(
            m_Context, {
                           .Name = "SceneRenderer Streak Prefilter Layout",
                           .DescriptorSetLayouts = {m_SetLayout},
                           .PushConstantRanges = {PushConstantRange::Of<StreakPrefilterPush>(
                               ShaderStage::Compute)},
                       });
        m_FilterLayout = PipelineLayout::Create(
            m_Context, {
                           .Name = "SceneRenderer Streak Filter Layout",
                           .DescriptorSetLayouts = {m_SetLayout},
                           .PushConstantRanges = {PushConstantRange::Of<StreakFilterPush>(
                               ShaderStage::Compute)},
                       });
        m_PrefilterPipeline = ComputePipeline::Create(
            m_Context,
            {
                .Name = "SceneRenderer Streak Prefilter Pipeline",
                .PipelineLayout = m_PrefilterLayout,
                .ShaderStage = {.Stage = ShaderStage::Compute, .Module = prefilterCs.Get()->Module},
            });
        m_FilterPipeline = ComputePipeline::Create(
            m_Context,
            {
                .Name = "SceneRenderer Streak Filter Pipeline",
                .PipelineLayout = m_FilterLayout,
                .ShaderStage = {.Stage = ShaderStage::Compute, .Module = filterCs.Get()->Module},
            });
    }

    AnamorphicStreak::~AnamorphicStreak()
    {
        m_Context.GetBindlessRegistry().Release(m_FinalHandle);
    }

    void AnamorphicStreak::Resize(const uvec2 sceneExtent, const Ref<ImageView>& hdrView)
    {
        if (m_Images[0] != nullptr && sceneExtent == m_SceneExtent)
        {
            SetSourceView(hdrView);
            return;
        }

        VE_ASSERT(m_Context.IsFormatLinearFilterSupported(StreakFormat),
                  "AnamorphicStreak: the streak needs SampledImageFilterLinear on the HDR format");

        m_SceneExtent = sceneExtent;
        const uvec2 extent = StreakExtent(sceneExtent);

        BindlessRegistry& bindless = m_Context.GetBindlessRegistry();
        bindless.Release(m_FinalHandle);

        for (u32 index = 0; index < 2; index++)
        {
            m_Images[index] =
                Image::Create(m_Context, {
                                             .Name = fmt::format("SceneRenderer Streak {}", index),
                                             .Extent = {extent.x, extent.y, 1},
                                             .Format = StreakFormat,
                                             .Usage = ImageUsage::Storage | ImageUsage::Sampled,
                                         });
            m_Views[index] = ImageView::Create(
                m_Context, {
                               .Name = fmt::format("SceneRenderer Streak {} View", index),
                               .Image = m_Images[index],
                           });
        }

        m_Sampler = Sampler::Create(m_Context, {
                                                   .Name = "SceneRenderer Streak Sampler",
                                                   .MagFilter = Filter::Linear,
                                                   .MinFilter = Filter::Linear,
                                                   .MipmapMode = MipmapMode::Nearest,
                                                   .AddressModeU = AddressMode::ClampToEdge,
                                                   .AddressModeV = AddressMode::ClampToEdge,
                                                   .AddressModeW = AddressMode::ClampToEdge,
                                                   .AnisotropyEnabled = false,
                                               });

        m_FinalHandle = bindless.Register(m_Views[GetFinalImage()]);

        m_FilterSet = DescriptorSet::Create(
            m_Context, {.Name = "SceneRenderer Streak Filter Set", .Layout = m_SetLayout});
        m_FilterSet->Write(0, m_Views[0]);
        m_FilterSet->Write(1, m_Sampler);
        m_FilterSet->Write(2, m_Views[1]);
        SetSourceView(hdrView);
    }

    void AnamorphicStreak::Release()
    {
        m_Context.GetBindlessRegistry().Release(m_FinalHandle);
        m_FinalHandle = TextureHandle{};
        m_PrefilterSet.reset();
        m_FilterSet.reset();
        m_Views = {};
        m_Images = {};
        m_Sampler.reset();
        m_SceneExtent = uvec2(0);
    }

    void AnamorphicStreak::SetSourceView(const Ref<ImageView>& source)
    {
        // A prior frame's command buffer may still reference the set, which is not
        // update-after-bind, so a fresh one replaces it and the old one retires through the frame
        // bin (BloomPyramid::SetSourceView's reason).
        if (m_Views[0] == nullptr)
        {
            return;
        }
        Ref<DescriptorSet> set = DescriptorSet::Create(
            m_Context, {.Name = "SceneRenderer Streak Prefilter Set", .Layout = m_SetLayout});
        set->Write(0, source);
        set->Write(1, m_Sampler);
        set->Write(2, m_Views[0]);
        m_PrefilterSet = std::move(set);
    }

    void AnamorphicStreak::Declare(RenderGraph& graph, const ResourceId hdrId,
                                   const std::array<ResourceId, 2>& imageIds,
                                   const AutoExposureMeter& autoExposure)
    {
        const uvec2 sceneAllocExtent = m_SceneExtent;
        const bool* const active = &m_Active;

        {
            RenderGraph::PassBuilder builder = graph.AddComputePass("Streak Prefilter");
            builder.Sample(hdrId);
            builder.StorageWrite(imageIds[0]);
            // The tonemap samples nothing from the streak on a frame it is not drawn.
            builder.SkipWhenUnread([active] { return !*active; });

            const Ref<ComputePipeline> pipeline = m_PrefilterPipeline;
            const Ref<DescriptorSet> set = m_PrefilterSet;
            const AutoExposureMeter* meter = &autoExposure;
            builder.Execute(
                [pipeline, set, sceneAllocExtent, meter](PassContext& inner)
                {
                    const auto* view = static_cast<const SceneView*>(inner.UserData());
                    VE_ASSERT(view != nullptr, "Streak prefilter pass: null SceneView");
                    const uvec2 dest = StreakExtent(view->PostResolveExtent);
                    const MipSubRect src =
                        ComputeMipSubRect(view->PostResolveExtent, sceneAllocExtent, 0);
                    // Display-referred like bloom's, so a source streaks where it blooms.
                    const f32 toHdr = 1.0f / std::max(meter->GetResolvedExposure(), 1e-5f);
                    CommandBuffer& cmd = inner.Cmd();
                    cmd.BindPipeline(pipeline);
                    cmd.BindDescriptorSets(DescriptorSetBindInfo{
                        .Sets = {set},
                        .FirstSet = 3, // sets 0-2 are the typed bindless registries
                        .PipelineBindPoint = PipelineBindPoint::Compute,
                    });
                    cmd.PushConstants(StreakPrefilterPush{
                        .DestExtent = dest,
                        .SourceScaleUV = src.ScaleUV,
                        .SourceMaxUV = src.MaxUV,
                        .Threshold = view->BloomThreshold * toHdr,
                        .FireflyCeiling = view->BloomFireflyCeiling * toHdr,
                    });
                    cmd.Dispatch((dest.x + 7) / 8, (dest.y + 7) / 8, 1);
                });
        }

        RenderGraph::PassBuilder builder = graph.AddComputePass("Streak Filter");
        builder.Sample(imageIds[0]);
        builder.StorageWrite(imageIds[1]);
        builder.SkipWhenUnread([active] { return !*active; });

        const Ref<ComputePipeline> pipeline = m_FilterPipeline;
        const Ref<DescriptorSet> set = m_FilterSet;
        builder.Execute(
            [pipeline, set](PassContext& inner)
            {
                const auto* view = static_cast<const SceneView*>(inner.UserData());
                VE_ASSERT(view != nullptr, "Streak filter pass: null SceneView");
                const uvec2 extent = StreakExtent(view->PostResolveExtent);
                const vec3 ratio =
                    StreakChannelRatio(view->StreakLength, view->StreakChromaticSpread, extent.x);
                CommandBuffer& cmd = inner.Cmd();
                cmd.BindPipeline(pipeline);
                cmd.BindDescriptorSets(DescriptorSetBindInfo{
                    .Sets = {set},
                    .FirstSet = 3,
                    .PipelineBindPoint = PipelineBindPoint::Compute,
                });
                cmd.PushConstants(StreakFilterPush{
                    .Extent = extent,
                    .Padding = uvec2(0),
                    .Ratio = vec4(ratio, 0.0f),
                });
                // One workgroup per row, which scans the row across its invocations.
                cmd.Dispatch(extent.y, 1, 1);
            });
    }
}
