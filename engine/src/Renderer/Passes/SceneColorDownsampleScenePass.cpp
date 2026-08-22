#include "SceneColorDownsampleScenePass.h"

#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/GraphicsPipeline.h>

#include <algorithm>

namespace Veng::Renderer
{
    uvec2 SceneColorDownsampleScenePass::LevelExtent(const uvec2 base, const u32 level)
    {
        return uvec2{std::max(base.x >> level, 1u), std::max(base.y >> level, 1u)};
    }

    void SceneColorDownsampleScenePass::Declare(RenderGraph& graph, const PassIO& /*io*/)
    {
        graph.AddPass(m_Name)
            .Color({
                // Cleared rather than loaded: only the sub-rect is drawn, and an undrawn texel at a
                // coarse level must read as nothing rather than as whatever the last frame left —
                // this target is persistent across frames.
                .Resource = m_TargetId,
                .Load = LoadOp::Clear,
                .Store = StoreOp::Store,
                .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f},
            })
            .Sample(m_SourceId)
            .Execute(
                [this](PassContext& inner)
                {
                    CommandBuffer& cmd = inner.Cmd();
                    cmd.BindPipeline(m_Pipeline);

                    // Every level renders the sub-rect its parent occupied, halved — so the valid
                    // fraction is the same at every level and a material's one sub-rect mapping
                    // serves a sample at any of them.
                    const uvec2 renderExtent = Wrap(inner).View().RenderExtent;
                    const uvec2 parentAlloc = LevelExtent(m_Extent, m_Level - 1);
                    const uvec2 parentValid = LevelExtent(renderExtent, m_Level - 1);
                    const uvec2 targetValid = LevelExtent(renderExtent, m_Level);
                    cmd.SetViewport({0, 0}, targetValid);
                    cmd.SetScissor({0, 0}, targetValid);
                    m_Context.GetBindlessRegistry().Bind(cmd);

                    const vec2 alloc = vec2(parentAlloc);
                    const vec2 valid = vec2(parentValid);
                    cmd.PushConstants(SceneColorDownsamplePush{
                        .SourceTexture = m_SourceHandle.Index,
                        .Sampler = m_Sampler.Index,
                        .Pad0 = 0,
                        .Pad1 = 0,
                        .ScaleUV = valid / alloc,
                        .MaxUV = (valid - 0.5f) / alloc,
                        .TexelSize = 1.0f / alloc,
                    });
                    cmd.DrawFullscreenTriangle();
                });
    }
}
