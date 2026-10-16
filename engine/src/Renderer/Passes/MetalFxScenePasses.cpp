#include "MetalFxScenePasses.h"

#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/SceneView.h>

#include "../MetalFxUpscale.h"

namespace Veng::Renderer
{
    void MetalFxMotionScenePass::Declare(RenderGraph& graph, const PassIO& io)
    {
        const TextureHandle depthHandle = io.DepthHandle;
        // Outside the rendered sub-rect the scaler reads nothing, so the target is not cleared.
        graph.AddPass("MetalFX Motion")
            .Color({.Resource = m_MotionId, .Load = LoadOp::DontCare, .Store = StoreOp::Store})
            .Sample(m_DepthId)
            .Sample(m_VelocityId)
            .Execute(
                [this, depthHandle](PassContext& inner)
                {
                    const SceneView& view = Wrap(inner).View();
                    const BindlessRegistry& registry = m_Context.GetBindlessRegistry();
                    CommandBuffer& cmd = inner.Cmd();
                    cmd.BindPipeline(m_Upscale.GetMotionPipeline());
                    cmd.SetViewport({0, 0}, view.RenderExtent);
                    cmd.SetScissor({0, 0}, view.RenderExtent);
                    registry.Bind(cmd);
                    cmd.PushConstants(MetalFxMotionPush{
                        .DepthTexture = depthHandle.Index,
                        .VelocityTexture = m_VelocityHandle.Index,
                        .ViewConstantsIndex = registry.GetCurrentViewConstantsIndex(),
                    });
                    cmd.DrawFullscreenTriangle();
                });
    }

    void MetalFxTemporalScenePass::Declare(RenderGraph& graph, const PassIO& /*io*/)
    {
        graph.AddComputePass("MetalFX Temporal")
            .Sample(m_LitId)
            .Sample(m_DepthId)
            .Sample(m_VelocityId)
            .StorageWrite(m_OutputId)
            .Execute(
                [this](PassContext& inner)
                {
                    Backend::MetalFxTemporalScaler* scaler = m_Upscale.GetTemporal();
                    VE_ASSERT(scaler != nullptr,
                              "MetalFX Temporal: the pass is wired with no scaler held");
                    const SceneView& view = Wrap(inner).View();
                    const vec2 content = vec2(view.RenderExtent);
                    scaler->Encode({
                        .Color = inner.Resolved(m_LitId).GetImage().get(),
                        .Depth = inner.Resolved(m_DepthId).GetImage().get(),
                        .Motion = inner.Resolved(m_VelocityId).GetImage().get(),
                        .Output = inner.Resolved(m_OutputId).GetImage().get(),
                        .ContentExtent = view.RenderExtent,
                        // The jitter moved the image by JitterNdc / 2 of the sub-rect, in the
                        // same axes the scaler's pixel offset is in.
                        .JitterPixels = view.JitterNdc * 0.5f * content,
                        // Velocity is current minus previous position in viewport UV; the scaler
                        // wants the step back to the previous position, in content pixels.
                        .MotionScale = -content,
                        .Reset = *m_HistoryReset,
                    });
                });
    }

    void MetalFxSpatialScenePass::Declare(RenderGraph& graph, const PassIO& /*io*/)
    {
        graph.AddComputePass("MetalFX Spatial")
            .Sample(m_SourceId)
            .StorageWrite(m_OutputId)
            .Execute(
                [this](PassContext& inner)
                {
                    Backend::MetalFxSpatialScaler* scaler = m_Upscale.GetSpatial();
                    VE_ASSERT(scaler != nullptr,
                              "MetalFX Spatial: the pass is wired with no scaler held");
                    const SceneView& view = Wrap(inner).View();
                    scaler->Encode({
                        .Color = inner.Resolved(m_SourceId).GetImage().get(),
                        .Output = inner.Resolved(m_OutputId).GetImage().get(),
                        .ContentExtent = view.SceneColorExtent,
                    });
                });
    }
}
