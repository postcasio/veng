#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/ScenePass.h>
#include <Veng/Renderer/Types.h>

namespace Veng::Renderer
{
    class Context;
    class GraphicsPipeline;

    // The half-res composite push block, matching halfres_composite.frag PushConstants: the
    // layer, reduced-depth, and full-depth slots, and the two valid sub-rect extents the
    // upsample maps between.
    struct HalfResCompositePush
    {
        u32 LayerTexture;
        u32 HalfDepthTexture;
        u32 DepthTexture;
        u32 Pad0;
        vec2 FullValid;
        vec2 HalfValid;
    };

    /// @brief Composites the half-res translucent layer into the lit scene color.
    ///
    /// A fullscreen draw over the full valid sub-rect: the fragment upsamples the layer's
    /// (premultiplied color, coverage) depth-aware — each 2x2 tap's bilinear weight collapses
    /// in proportion to how far its reduced depth sits from the pixel's own opaque depth, so
    /// the layer hugs geometry edges — and the pipeline's (One, OneMinusSrcAlpha) blend lays
    /// it under whatever the full-resolution translucent pass draws after it.
    class HalfResCompositeScenePass final : public ScenePass
    {
    public:
        /// @brief Constructs the pass.
        /// @param context     Renderer context for bindless access.
        /// @param pipeline    The composite pipeline (premultiplied blend, one HDR attachment).
        /// @param layerId     The half-res layer color id (declared sampled).
        /// @param layerHandle Bindless slot for the layer color.
        /// @param halfDepthId The half-res reduced depth id (declared sampled).
        /// @param halfDepthHandle Bindless slot for the reduced depth.
        /// @param depthId     Full-res opaque depth id (declared sampled).
        /// @param depthHandle Bindless slot for the full-res opaque depth.
        /// @param targetId    The lit scene-color target the composite blends into.
        HalfResCompositeScenePass(Context& context, Ref<GraphicsPipeline> pipeline,
                                  ResourceId layerId, TextureHandle layerHandle,
                                  ResourceId halfDepthId, TextureHandle halfDepthHandle,
                                  ResourceId depthId, TextureHandle depthHandle,
                                  ResourceId targetId)
            : m_Context(context), m_Pipeline(std::move(pipeline)), m_LayerId(layerId),
              m_LayerHandle(layerHandle), m_HalfDepthId(halfDepthId),
              m_HalfDepthHandle(halfDepthHandle), m_DepthId(depthId), m_DepthHandle(depthHandle),
              m_TargetId(targetId)
        {
        }

        /// @brief Updates the render extent (unused; the viewport reads the per-frame view).
        void Resize(uvec2 /*extent*/) override {}
        /// @brief Contributes the composite pass into the graph.
        void Declare(RenderGraph& graph, const PassIO& io) override;

    private:
        /// @brief Renderer context for bindless access.
        Context& m_Context;
        /// @brief The composite pipeline.
        Ref<GraphicsPipeline> m_Pipeline;
        /// @brief The half-res layer color id.
        ResourceId m_LayerId;
        /// @brief Bindless slot for the layer color.
        TextureHandle m_LayerHandle;
        /// @brief The half-res reduced depth id.
        ResourceId m_HalfDepthId;
        /// @brief Bindless slot for the reduced depth.
        TextureHandle m_HalfDepthHandle;
        /// @brief Full-res opaque depth id.
        ResourceId m_DepthId;
        /// @brief Bindless slot for the full-res opaque depth.
        TextureHandle m_DepthHandle;
        /// @brief The lit scene-color target.
        ResourceId m_TargetId;
    };
}
