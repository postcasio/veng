#include "SsrScenePass.h"

#include "../SsrChain.h"
#include "DebugBlitScenePasses.h"

#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/GraphicsPipeline.h>

namespace Veng::Renderer
{
    void SsrScenePass::Declare(RenderGraph& graph, const PassIO& io)
    {
        m_Chain.Declare(graph, SsrGraphInputs{
                                   .Albedo = io.GBufferAlbedo,
                                   .AlbedoHandle = io.AlbedoHandle,
                                   .Depth = io.GBufferDepth,
                                   .Normal = io.GBufferNormal,
                                   .Material = io.GBufferOrm,
                                   .DepthHandle = io.DepthHandle,
                                   .NormalHandle = io.NormalHandle,
                                   .MaterialHandle = io.OrmHandle,
                                   .Color = io.Hdr,
                                   .ColorHandle = io.HdrHandle,
                                   .Velocity = io.Velocity,
                                   .VelocityHandle = io.VelocityHandle,
                               });
    }

    void SsrDebugBlitScenePass::Declare(RenderGraph& graph, const PassIO& io)
    {
        const bool coverage = m_Source == Source::Coverage;
        const SamplerHandle samplerHandle = io.SamplerHandle;
        graph.AddPass("SSR Debug Blit")
            .Color({
                .Resource = io.Output,
                .Load = LoadOp::Clear,
                .Store = StoreOp::Store,
                .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f},
            })
            .Sample(coverage ? m_Chain.GetCoverageId() : m_Chain.GetReflectionId())
            .Execute(
                [this, coverage, samplerHandle](PassContext& inner)
                {
                    // Radiance A alternates with the frame's parity, so its slot is read per frame.
                    const TextureHandle texture =
                        coverage ? m_Chain.GetCoverageHandle() : m_Chain.GetReflectionHandle();
                    CommandBuffer& cmd = inner.Cmd();
                    cmd.BindPipeline(m_Pipeline);
                    cmd.SetViewport({0, 0}, m_Extent);
                    cmd.SetScissor({0, 0}, m_Extent);
                    m_Context.GetBindlessRegistry().Bind(cmd);
                    cmd.PushConstants(BlitPushConstants{
                        .Texture = texture.Index,
                        .Sampler = samplerHandle.Index,
                    });
                    cmd.DrawFullscreenTriangle();
                });
    }
}
