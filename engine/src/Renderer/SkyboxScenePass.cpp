#include "SkyboxScenePass.h"

#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/SceneRenderer.h>

namespace Veng::Renderer
{
    namespace
    {
        // Matches skybox.frag PushConstants byte-for-byte (eight u32s — three pads reach a
        // 16-byte boundary).
        struct SkyboxPushConstants
        {
            u32 DepthTexture;
            u32 Sampler;
            u32 ViewConstantsIndex;
            u32 Enabled;
            f32 EnvIntensity;
            u32 Pad0;
            u32 Pad1;
            u32 Pad2;
        };
    }

    SkyboxScenePass::SkyboxScenePass(Context& context, Ref<GraphicsPipeline> pipeline,
                                     Ref<DescriptorSet> iblSet, ResourceId targetId,
                                     ResourceId depthId, TextureHandle depthHandle,
                                     SamplerHandle samplerHandle, uvec2 extent)
        : m_Context(context), m_Pipeline(std::move(pipeline)), m_IblSet(std::move(iblSet)),
          m_TargetId(targetId), m_DepthId(depthId), m_DepthHandle(depthHandle),
          m_SamplerHandle(samplerHandle), m_Extent(extent)
    {
    }

    void SkyboxScenePass::Resize(const uvec2 extent)
    {
        m_Extent = extent;
    }

    void SkyboxScenePass::Declare(RenderGraph& graph, const PassIO& /*io*/)
    {
        const TextureHandle depthHandle = m_DepthHandle;
        const SamplerHandle samplerHandle = m_SamplerHandle;
        const Ref<DescriptorSet> iblSet = m_IblSet;

        graph
            .AddPass("Skybox")
            // Composite over the lit scene color (preserve it; the shader discards foreground).
            .Color({
                .Resource = m_TargetId,
                .Load = LoadOp::Load,
                .Store = StoreOp::Store,
            })
            .Sample(m_DepthId)
            .Execute(
                [this, depthHandle, samplerHandle, iblSet](PassContext& inner)
                {
                    const ScenePassContext ctx = Wrap(inner);
                    CommandBuffer& cmd = ctx.Cmd();
                    const BindlessRegistry& registry = m_Context.GetBindlessRegistry();
                    const SceneView& view = ctx.View();

                    const uvec2 renderExtent = view.RenderExtent;
                    cmd.BindPipeline(m_Pipeline);
                    cmd.SetViewport({0, 0}, renderExtent);
                    cmd.SetScissor({0, 0}, renderExtent);
                    registry.Bind(cmd);

                    // The radiance cube + linear sampler ride the IBL set (set 1 here).
                    cmd.BindDescriptorSets(DescriptorSetBindInfo{
                        .Sets = {iblSet},
                        .FirstSet = 1,
                        .PipelineBindPoint = PipelineBindPoint::Graphics,
                    });

                    cmd.PushConstants(SkyboxPushConstants{
                        .DepthTexture = depthHandle.Index,
                        .Sampler = samplerHandle.Index,
                        .ViewConstantsIndex = registry.GetCurrentViewConstantsIndex(),
                        .Enabled = view.Environment.IsLoaded() ? 1u : 0u,
                        .EnvIntensity = view.EnvironmentIntensity,
                    });
                    cmd.DrawFullscreenTriangle();
                });
    }
}
