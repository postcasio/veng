#include "TaaScenePass.h"

#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/GraphicsPipeline.h>

namespace Veng::Renderer
{
    void TaaScenePass::Declare(RenderGraph& graph, const PassIO& io)
    {
        const TextureHandle depthHandle = io.DepthHandle;
        const TextureHandle hdrHandle = io.HdrHandle;
        const SamplerHandle samplerHandle = io.SamplerHandle;

        // Resolve: read the lit color (current), the reprojected history, and depth;
        // write the resolved color into the HDR target downstream passes sample.
        graph.AddPass("TAA Resolve")
            .Color({
                .Resource = m_HdrId,
                .Load = LoadOp::Clear,
                .Store = StoreOp::Store,
                .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f},
            })
            .Sample(m_LitId)
            .Sample(m_HistoryId)
            .Sample(m_DepthId)
            .Sample(m_VelocityId)
            .Execute(
                [this, depthHandle, samplerHandle](PassContext& inner)
                {
                    CommandBuffer& cmd = inner.Cmd();
                    const BindlessRegistry& registry = m_Context.GetBindlessRegistry();
                    cmd.BindPipeline(m_ResolvePipeline);
                    const uvec2 renderExtent = Wrap(inner).View().RenderExtent;
                    cmd.SetViewport({0, 0}, renderExtent);
                    cmd.SetScissor({0, 0}, renderExtent);
                    registry.Bind(cmd);
                    cmd.PushConstants(TaaResolvePush{
                        .CurrentTexture = m_LitHandle.Index,
                        .HistoryTexture = m_HistoryHandle.Index,
                        .DepthTexture = depthHandle.Index,
                        .VelocityTexture = m_VelocityHandle.Index,
                        .Sampler = samplerHandle.Index,
                        .ViewConstantsIndex = registry.GetCurrentViewConstantsIndex(),
                        .HistoryValid = *m_HistoryResetPtr ? 0u : 1u,
                        .Extent = renderExtent,
                    });
                    cmd.DrawFullscreenTriangle();
                });

        // History copy: refresh the persisted history from the resolved HDR for the
        // next frame's reprojection. The graph orders it after the resolve's write of
        // the HDR target and after the resolve's read of the history (write-after-read).
        graph.AddPass("TAA History Copy")
            .Color({
                .Resource = m_HistoryId,
                .Load = LoadOp::Clear,
                .Store = StoreOp::Store,
                .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f},
            })
            .Sample(m_HdrId)
            .Execute(
                [this, hdrHandle, samplerHandle](PassContext& inner)
                {
                    CommandBuffer& cmd = inner.Cmd();
                    const BindlessRegistry& registry = m_Context.GetBindlessRegistry();
                    cmd.BindPipeline(m_CopyPipeline);
                    const uvec2 renderExtent = Wrap(inner).View().RenderExtent;
                    cmd.SetViewport({0, 0}, renderExtent);
                    cmd.SetScissor({0, 0}, renderExtent);
                    registry.Bind(cmd);
                    cmd.PushConstants(TaaCopyPush{
                        .SourceTexture = hdrHandle.Index,
                        .Sampler = samplerHandle.Index,
                    });
                    cmd.DrawFullscreenTriangle();
                });
    }
}
