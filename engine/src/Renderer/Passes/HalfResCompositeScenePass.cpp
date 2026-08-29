#include "HalfResCompositeScenePass.h"

#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/GraphicsPipeline.h>

#include "../HalfResTranslucency.h"

namespace Veng::Renderer
{
    void HalfResCompositeScenePass::Declare(RenderGraph& graph, const PassIO& /*io*/)
    {
        graph.AddPass("Half-Res Translucent Composite")
            .Color({
                .Resource = m_TargetId,
                .Load = LoadOp::Load,
                .Store = StoreOp::Store,
            })
            .Sample(m_LayerId)
            .Sample(m_HalfDepthId)
            .Sample(m_DepthId)
            .Execute(
                [this](PassContext& inner)
                {
                    // An idle wired layer (deactivation hysteresis) drew nothing this frame, so
                    // there is nothing to lay under the full-res translucents.
                    if (m_Plan->Draws.empty())
                    {
                        return;
                    }
                    CommandBuffer& cmd = inner.Cmd();
                    cmd.BindPipeline(m_Pipeline);
                    const uvec2 validExtent = Wrap(inner).View().RenderExtent;
                    cmd.SetViewport({0, 0}, validExtent);
                    cmd.SetScissor({0, 0}, validExtent);
                    m_Context.GetBindlessRegistry().Bind(cmd);
                    cmd.PushConstants(HalfResCompositePush{
                        .LayerTexture = m_LayerHandle.Index,
                        .HalfDepthTexture = m_HalfDepthHandle.Index,
                        .DepthTexture = m_DepthHandle.Index,
                        .Pad0 = 0,
                        .FullValid = vec2(validExtent),
                        .HalfValid = vec2(HalfResExtent(validExtent)),
                    });
                    cmd.DrawFullscreenTriangle();
                });
    }
}
