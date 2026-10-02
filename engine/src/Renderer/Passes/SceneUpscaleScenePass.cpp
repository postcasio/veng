#include "SceneUpscaleScenePass.h"

#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/SceneView.h>

namespace Veng::Renderer
{
    namespace
    {
        // This frame's mapping of a destination UV into a promotion's valid source region.
        struct Mapping
        {
            vec2 ScaleUV;
            vec2 MaxUV;
        };

        Mapping MappingFor(const Promotion& promotion, const SceneView& view)
        {
            const vec2 alloc = vec2(promotion.SourceExtent);
            const vec2 valid =
                vec2(promotion.Source == PromotionSource::BloomMask ? view.RenderExtent
                                                                    : view.SceneColorExtent);
            // Half-texel inset so the bilinear tap never reads past the rendered region.
            return {.ScaleUV = valid / alloc, .MaxUV = (valid - 0.5f) / alloc};
        }

        const char* PassName(const Promotion& primary, const bool paired)
        {
            if (paired)
            {
                return "Scene And Bloom Mask Upscale";
            }
            return primary.Source == PromotionSource::BloomMask ? "Bloom Mask Upscale"
                                                                : "Scene Upscale";
        }
    }

    void SceneUpscaleScenePass::Declare(RenderGraph& graph, const PassIO& /*io*/)
    {
        RenderGraph::PassBuilder builder = graph.AddPass(PassName(m_Primary, m_Mask.has_value()));
        // Every destination texel is written, so each load is discarded rather than cleared.
        builder.Color({
            .Resource = m_Primary.OutputId,
            .Load = LoadOp::DontCare,
            .Store = StoreOp::Store,
        });
        if (m_Mask)
        {
            builder.Color({
                .Resource = m_Mask->OutputId,
                .Load = LoadOp::DontCare,
                .Store = StoreOp::Store,
            });
        }
        builder.Sample(m_Primary.SourceId);
        if (m_Mask)
        {
            builder.Sample(m_Mask->SourceId);
        }
        builder.Execute(
            [this](PassContext& inner)
            {
                const SceneView& view = Wrap(inner).View();
                CommandBuffer& cmd = inner.Cmd();
                cmd.BindPipeline(m_Pipeline);
                // The destination is the post-resolve allocation: this pass is what makes the
                // tail's PostResolveExtent that rather than the scene's own resolution.
                cmd.SetViewport({0, 0}, m_Extent);
                cmd.SetScissor({0, 0}, m_Extent);
                m_Context.GetBindlessRegistry().Bind(cmd);
                const Mapping primary = MappingFor(m_Primary, view);
                const Mapping mask = m_Mask ? MappingFor(*m_Mask, view) : Mapping{};
                cmd.PushConstants(SceneUpscalePush{
                    .SourceTexture = m_Primary.SourceHandle.Index,
                    .Sampler = m_Sampler.Index,
                    .MaskTexture = m_Mask ? m_Mask->SourceHandle.Index : 0,
                    .Pad0 = 0,
                    .ScaleUV = primary.ScaleUV,
                    .MaxUV = primary.MaxUV,
                    .MaskScaleUV = mask.ScaleUV,
                    .MaskMaxUV = mask.MaxUV,
                });
                cmd.DrawFullscreenTriangle();
            });
    }
}
