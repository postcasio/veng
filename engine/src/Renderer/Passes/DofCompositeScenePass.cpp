#include "DofCompositeScenePass.h"

#include <algorithm>

#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/SceneRenderer.h>

#include "../DofChain.h"

namespace Veng::Renderer
{
    namespace
    {
        // This frame's valid sub-rect mapping over an allocation, in the form the composite's
        // push block carries: the validExtent/allocExtent scale and the half-texel-inset clamp.
        void FillSubRectUv(const uvec2 valid, const uvec2 alloc, vec2& scale, vec2& maxUv)
        {
            const vec2 validF{static_cast<f32>(valid.x), static_cast<f32>(valid.y)};
            const vec2 allocF{static_cast<f32>(std::max(alloc.x, 1u)),
                              static_cast<f32>(std::max(alloc.y, 1u))};
            scale = validF / allocF;
            maxUv = (validF - vec2(0.5f)) / allocF;
        }
    }

    DofCompositeScenePass::DofCompositeScenePass(
        Context& context, Ref<GraphicsPipeline> pipeline, const ResourceId nearId,
        const TextureHandle nearHandle, const ResourceId farId, const TextureHandle farHandle,
        const ResourceId destId, const SamplerHandle samplerHandle, const uvec2 halfExtent,
        const uvec2 extent)
        : m_Context(context), m_Pipeline(std::move(pipeline)), m_NearId(nearId),
          m_NearHandle(nearHandle), m_FarId(farId), m_FarHandle(farHandle), m_DestId(destId),
          m_SamplerHandle(samplerHandle), m_HalfExtent(halfExtent), m_Extent(extent)
    {
    }

    void DofCompositeScenePass::Resize(const uvec2 extent)
    {
        m_Extent = extent;
        m_HalfExtent = DofHalfExtent(extent);
    }

    void DofCompositeScenePass::Declare(RenderGraph& graph, const PassIO& io)
    {
        const ResourceId sourceId = io.Hdr;
        const TextureHandle sourceHandle = io.HdrHandle;

        graph.AddPass("DoF Composite")
            .Color({
                .Resource = m_DestId,
                .Load = LoadOp::Clear,
                .Store = StoreOp::Store,
                .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f},
            })
            .Sample(sourceId)
            .Sample(m_NearId)
            .Sample(m_FarId)
            .Execute(
                [this, sourceHandle](PassContext& inner)
                {
                    const ScenePassContext context = Wrap(inner);
                    const SceneView& view = context.View();
                    const uvec2 renderExtent = view.RenderExtent;
                    const uvec2 halfValid = DofHalfExtent(renderExtent);

                    DofCompositePush push{
                        .HdrTexture = sourceHandle.Index,
                        .NearTexture = m_NearHandle.Index,
                        .FarTexture = m_FarHandle.Index,
                        .Sampler = m_SamplerHandle.Index,
                    };
                    FillSubRectUv(renderExtent, m_Extent, push.ScaleUV, push.MaxUV);
                    FillSubRectUv(halfValid, m_HalfExtent, push.HalfScaleUV, push.HalfMaxUV);

                    CommandBuffer& cmd = context.Cmd();
                    cmd.BindPipeline(m_Pipeline);
                    cmd.SetViewport({0, 0}, renderExtent);
                    cmd.SetScissor({0, 0}, renderExtent);
                    m_Context.GetBindlessRegistry().Bind(cmd);
                    cmd.PushConstants(push);
                    cmd.DrawFullscreenTriangle();
                });
    }
}
