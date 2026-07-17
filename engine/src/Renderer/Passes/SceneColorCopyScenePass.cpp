#include "SceneColorCopyScenePass.h"

#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/GraphicsPipeline.h>

namespace Veng::Renderer
{
    void SceneColorCopyScenePass::Declare(RenderGraph& graph, const PassIO& /*io*/)
    {
        graph.AddPass("Scene Color Copy")
            .Color({
                .Resource = m_CopyId,
                .Load = LoadOp::Clear,
                .Store = StoreOp::Store,
                .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f},
            })
            .Color({
                // The depth copy clears to the far plane so an unwritten texel reads
                // as background, never as a phantom occluder.
                .Resource = m_DepthCopyId,
                .Load = LoadOp::Clear,
                .Store = StoreOp::Store,
                .Clear = ClearColor{.R = 1.0f, .G = 1.0f, .B = 1.0f, .A = 1.0f},
            })
            .Sample(m_SourceId)
            .Sample(m_DepthId)
            .Execute(
                [this](PassContext& inner)
                {
                    CommandBuffer& cmd = inner.Cmd();
                    cmd.BindPipeline(m_Pipeline);
                    const uvec2 renderExtent = Wrap(inner).View().RenderExtent;
                    cmd.SetViewport({0, 0}, renderExtent);
                    cmd.SetScissor({0, 0}, renderExtent);
                    m_Context.GetBindlessRegistry().Bind(cmd);
                    // Sources and destinations share the allocation extent, so this
                    // frame's sub-rect mapping serves both the sample and the clamp.
                    const vec2 alloc = vec2(m_Extent);
                    const vec2 valid = vec2(renderExtent);
                    cmd.PushConstants(SceneColorCopyPush{
                        .SourceTexture = m_SourceHandle.Index,
                        .DepthTexture = m_DepthHandle.Index,
                        .Sampler = m_Sampler.Index,
                        .Pad0 = 0,
                        .ScaleUV = valid / alloc,
                        .MaxUV = (valid - 0.5f) / alloc,
                    });
                    cmd.DrawFullscreenTriangle();
                });
    }
}
