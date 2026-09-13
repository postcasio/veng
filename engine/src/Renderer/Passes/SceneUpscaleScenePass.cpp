#include "SceneUpscaleScenePass.h"

#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/SceneView.h>

namespace Veng::Renderer
{
    void SceneUpscaleScenePass::Declare(RenderGraph& graph, const PassIO& /*io*/)
    {
        graph.AddPass("Scene Upscale")
            .Color({
                // Every destination texel is written, so the load is discarded rather than cleared.
                .Resource = m_OutputId,
                .Load = LoadOp::DontCare,
                .Store = StoreOp::Store,
            })
            .Sample(m_SourceId)
            .Execute(
                [this](PassContext& inner)
                {
                    const SceneView& view = Wrap(inner).View();
                    CommandBuffer& cmd = inner.Cmd();
                    cmd.BindPipeline(m_Pipeline);
                    // The destination is the allocation: this pass is what makes the tail's
                    // PostResolveExtent the allocation rather than the rendered sub-rect.
                    cmd.SetViewport({0, 0}, m_Extent);
                    cmd.SetScissor({0, 0}, m_Extent);
                    m_Context.GetBindlessRegistry().Bind(cmd);
                    const vec2 alloc = vec2(m_Extent);
                    const vec2 valid = vec2(view.RenderExtent);
                    cmd.PushConstants(SceneUpscalePush{
                        .SourceTexture = m_SourceHandle.Index,
                        .Sampler = m_Sampler.Index,
                        .Pad0 = 0,
                        .Pad1 = 0,
                        .ScaleUV = valid / alloc,
                        // Half-texel inset so the bilinear tap never reads past the rendered region.
                        .MaxUV = (valid - 0.5f) / alloc,
                    });
                    cmd.DrawFullscreenTriangle();
                });
    }
}
