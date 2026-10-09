#include "SceneColorCopyScenePass.h"

#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/GraphicsPipeline.h>

namespace Veng::Renderer
{
    void SceneColorCopyScenePass::Declare(RenderGraph& graph, const PassIO& /*io*/)
    {
        graph.AddPass(m_Name)
            .Color({
                .Resource = m_TargetId,
                .Load = LoadOp::Clear,
                .Store = StoreOp::Store,
                .Clear = m_Clear,
            })
            .Sample(m_SourceId)
            // The grab's only readers are the translucent draws, so a frame none of them samples
            // it leaves the target unread and the clear unobserved.
            .SkipWhenUnread([sampled = m_Sampled] { return !*sampled; })
            .Execute(
                [this](PassContext& inner)
                {
                    CommandBuffer& cmd = inner.Cmd();
                    cmd.BindPipeline(m_Pipeline);
                    const uvec2 renderExtent = Wrap(inner).View().RenderExtent;
                    cmd.SetViewport({0, 0}, renderExtent);
                    cmd.SetScissor({0, 0}, renderExtent);
                    m_Context.GetBindlessRegistry().Bind(cmd);
                    // Source and destination share the allocation extent, so this frame's
                    // sub-rect mapping serves both the sample and the clamp.
                    const vec2 alloc = vec2(m_Extent);
                    const vec2 valid = vec2(renderExtent);
                    cmd.PushConstants(SceneColorCopyPush{
                        .SourceTexture = m_SourceHandle.Index,
                        .Sampler = m_Sampler.Index,
                        .Pad0 = 0,
                        .Pad1 = 0,
                        .ScaleUV = valid / alloc,
                        .MaxUV = (valid - 0.5f) / alloc,
                    });
                    cmd.DrawFullscreenTriangle();
                });
    }
}
