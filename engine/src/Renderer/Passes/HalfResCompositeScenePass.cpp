#include "HalfResCompositeScenePass.h"

#include <fmt/format.h>

#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/GraphicsPipeline.h>

#include "../HalfResTranslucency.h"

namespace Veng::Renderer
{
    void HalfResCompositeScenePass::Declare(RenderGraph& graph, const PassIO& /*io*/)
    {
        graph
            .AddPass(m_Segment == 0 ? string{"Half-Res Translucent Composite"}
                                    : fmt::format("Half-Res Translucent Composite {}", m_Segment))
            .Color({
                .Resource = m_TargetId,
                .Load = LoadOp::Load,
                .Store = StoreOp::Store,
            })
            .Sample(m_LayerId)
            .Sample(m_HalfDepthId)
            .Sample(m_DepthId)
            // An idle wired layer (deactivation hysteresis) drew nothing this frame, so there is
            // nothing to lay under the full-res translucents. The depth reduce and the layer pass
            // skip on the same predicate, which is what leaves their outputs unread.
            .SkipWhen([this] { return m_Plan->Draws.empty(); })
            .Execute(
                [this](PassContext& inner)
                {
                    CommandBuffer& cmd = inner.Cmd();
                    cmd.BindPipeline(m_Pipeline);
                    const uvec2 validExtent = Wrap(inner).View().RenderExtent;
                    // The viewport stays the whole sub-rect, so sv_position is the scene pixel the
                    // upsample reads; only the scissor narrows to what this segment drew.
                    cmd.SetViewport({0, 0}, validExtent);
                    const HalfResScissor scissor = HalfResCompositeScissor(*m_Rect, validExtent);
                    if (scissor.Extent.x == 0 || scissor.Extent.y == 0)
                    {
                        return;
                    }
                    cmd.SetScissor(scissor.Offset, scissor.Extent);
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
