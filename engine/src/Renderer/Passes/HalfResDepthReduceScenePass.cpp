#include "HalfResDepthReduceScenePass.h"

#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/GraphicsPipeline.h>

#include "../HalfResTranslucency.h"

namespace Veng::Renderer
{
    void HalfResDepthReduceScenePass::Declare(RenderGraph& graph, const PassIO& /*io*/)
    {
        graph.AddPass("Half-Res Depth Reduce")
            .Depth({
                // Cleared to the reverse-Z far plane so the texels outside the valid sub-rect —
                // which the viewport never rasterizes — read as background rather than stale
                // depth from the previous frame's sub-rect.
                .Resource = m_TargetId,
                .Load = LoadOp::Clear,
                .Store = StoreOp::Store,
                .Clear = ClearDepth{.Depth = 0.0f},
            })
            .Sample(m_DepthId)
            .Execute(
                [this](PassContext& inner)
                {
                    // An idle wired layer (deactivation hysteresis) keeps the cleared target and
                    // pays no fullscreen reduce — nothing will composite it.
                    if (m_Plan->Draws.empty())
                    {
                        return;
                    }
                    CommandBuffer& cmd = inner.Cmd();
                    cmd.BindPipeline(m_Pipeline);
                    const uvec2 validExtent = Wrap(inner).View().RenderExtent;
                    const uvec2 halfExtent = HalfResExtent(validExtent);
                    cmd.SetViewport({0, 0}, halfExtent);
                    cmd.SetScissor({0, 0}, halfExtent);
                    m_Context.GetBindlessRegistry().Bind(cmd);
                    cmd.PushConstants(HalfResDepthReducePush{
                        .DepthTexture = m_DepthHandle.Index,
                        .ValidX = validExtent.x,
                        .ValidY = validExtent.y,
                    });
                    cmd.DrawFullscreenTriangle();
                });
    }
}
