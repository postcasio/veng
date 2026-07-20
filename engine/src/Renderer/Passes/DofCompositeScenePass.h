#pragma once

#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/ScenePass.h>
#include <Veng/Veng.h>

namespace Veng::Renderer
{
    class Context;
    class GraphicsPipeline;

    /// @brief Fullscreen pass blending the depth-of-field layers over the lit scene color into
    ///        the HDR target.
    ///
    /// The battery's terminal stage: it samples the chain's full-resolution lit scene-color
    /// intermediate and the two half-resolution blurred layers, covers the source by each layer's
    /// coverage, and writes the HDR target — the same id the bloom sweep, the metering, and the
    /// tonemap already read, so the tail is spliced with no extra copy. It is its own pass rather
    /// than a fold into tonemap because it must run *before* bloom — a defocused highlight still
    /// blooms — and tonemap stays the authorable PostProcess-material exemplar.
    ///
    /// The pass owns nothing: the pipeline and every target belong to DofChain, and the source
    /// id/handle arrive through PassIO.
    class DofCompositeScenePass final : public ScenePass
    {
    public:
        /// @brief Constructs the composite over the chain's pipeline and targets.
        /// @param context        The render context.
        /// @param pipeline       The chain's composite pipeline.
        /// @param nearId         The near-layer fill import (declared sampled for barrier order).
        /// @param nearHandle     Bindless slot for the near-layer fill target.
        /// @param farId          The far-layer fill import.
        /// @param farHandle      Bindless slot for the far-layer fill target.
        /// @param destId         The HDR target the composite writes.
        /// @param samplerHandle  Bindless slot for the chain's linear clamp sampler.
        /// @param halfExtent     The half-resolution allocation extent the layers live in.
        /// @param extent         The full render allocation extent.
        DofCompositeScenePass(Context& context, Ref<GraphicsPipeline> pipeline, ResourceId nearId,
                              TextureHandle nearHandle, ResourceId farId, TextureHandle farHandle,
                              ResourceId destId, SamplerHandle samplerHandle, uvec2 halfExtent,
                              uvec2 extent);

        DofCompositeScenePass(const DofCompositeScenePass&) = delete;
        DofCompositeScenePass& operator=(const DofCompositeScenePass&) = delete;

        /// @brief Records the new full render allocation extent (and the half-resolution one).
        void Resize(uvec2 extent) override;

        /// @brief Contributes the fullscreen composite into the graph.
        void Declare(RenderGraph& graph, const PassIO& io) override;

    private:
        Context& m_Context;
        Ref<GraphicsPipeline> m_Pipeline;
        ResourceId m_NearId;
        TextureHandle m_NearHandle;
        ResourceId m_FarId;
        TextureHandle m_FarHandle;
        ResourceId m_DestId;
        SamplerHandle m_SamplerHandle;
        uvec2 m_HalfExtent;
        uvec2 m_Extent;
    };
}
