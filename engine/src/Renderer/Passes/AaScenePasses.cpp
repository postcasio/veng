#include "AaScenePasses.h"

#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/GraphicsPipeline.h>

namespace Veng::Renderer
{
    void FxaaScenePass::Declare(RenderGraph& graph, const PassIO& /*io*/)
    {
        graph.AddPass(m_Label)
            .Color({
                .Resource = m_OutputId,
                .Load = LoadOp::Clear,
                .Store = StoreOp::Store,
                .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f},
            })
            .Sample(m_InputId)
            .Execute(
                [this](PassContext& inner)
                {
                    CommandBuffer& cmd = inner.Cmd();
                    cmd.BindPipeline(m_Pipeline);
                    // FXAA runs on the already-upscaled LDR, so it renders the full output edge to
                    // edge (never the dynamic-resolution sub-rect) — the reciprocal extent is the
                    // full allocation.
                    cmd.SetViewport({0, 0}, m_Extent);
                    cmd.SetScissor({0, 0}, m_Extent);
                    m_Context.GetBindlessRegistry().Bind(cmd);
                    cmd.PushConstants(FxaaPush{
                        .Texture = m_InputHandle.Index,
                        .Sampler = m_SamplerHandle.Index,
                        .RcpFrame = vec2(1.0f / static_cast<f32>(m_Extent.x),
                                         1.0f / static_cast<f32>(m_Extent.y)),
                    });
                    cmd.DrawFullscreenTriangle();
                });
    }

    void Cmaa2ApplyScenePass::Declare(RenderGraph& graph, const PassIO& /*io*/)
    {
        graph.AddPass("CMAA2 Apply")
            .Color({
                .Resource = m_OutputId,
                .Load = LoadOp::Clear,
                .Store = StoreOp::Store,
                .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f},
            })
            .Sample(m_ColorId)
            .Sample(m_EdgeId)
            .Execute(
                [this](PassContext& inner)
                {
                    CommandBuffer& cmd = inner.Cmd();
                    cmd.BindPipeline(m_Pipeline);
                    cmd.SetViewport({0, 0}, m_Extent);
                    cmd.SetScissor({0, 0}, m_Extent);
                    m_Context.GetBindlessRegistry().Bind(cmd);
                    cmd.PushConstants(Cmaa2ApplyPush{
                        .ColorTexture = m_ColorHandle.Index,
                        .EdgeTexture = m_EdgeHandle.Index,
                        .Sampler = m_SamplerHandle.Index,
                        .Pad0 = 0,
                        .RcpFrame = vec2(1.0f / static_cast<f32>(m_Extent.x),
                                         1.0f / static_cast<f32>(m_Extent.y)),
                    });
                    cmd.DrawFullscreenTriangle();
                });
    }
}
