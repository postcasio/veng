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
    class DescriptorSet;

    /// @brief Fullscreen procedural-atmosphere sky pass: fills the cleared-depth background with
    ///        the precomputed atmospheric scattering, compositing over the lit scene color.
    ///
    /// Occupies the same slot as SkyboxScenePass — after deferred lighting (writing the same
    /// scene-color target, LoadOp::Load) and before the bloom/SSR/tonemap tail, so the sky
    /// tonemaps and reflects with the scene. It samples the g-buffer depth through bindless and
    /// discards foreground pixels, and reads the scattering total + transmittance LUTs from the
    /// renderer's atmosphere set (bound at set 1). The sun direction and Atmosphere parameters
    /// ride the per-frame SceneView; the pass is a no-op (everything discarded) unless the
    /// renderer pushes Enabled for the frame.
    class SkyScenePass final : public ScenePass
    {
    public:
        /// @brief Constructs the pass.
        /// @param context        The render context.
        /// @param pipeline       The fullscreen sky pipeline (reserves set 0 bindless + set 1 atmosphere).
        /// @param atmosphereSet  The renderer's atmosphere descriptor set (scattering/transmittance/sampler).
        /// @param targetId       The scene-color target lighting wrote (this pass composites into it).
        /// @param depthId        The g-buffer depth resource, declared sampled for barrier derivation.
        /// @param depthHandle    The bindless handle for the depth target.
        /// @param samplerHandle  The shared sampler bindless handle.
        /// @param extent         The render extent.
        SkyScenePass(Context& context, Ref<GraphicsPipeline> pipeline,
                     Ref<DescriptorSet> atmosphereSet, ResourceId targetId, ResourceId depthId,
                     TextureHandle depthHandle, SamplerHandle samplerHandle, uvec2 extent);

        /// @brief Updates the cached render extent.
        void Resize(uvec2 extent) override;

        /// @brief Contributes the fullscreen sky pass into the graph.
        void Declare(RenderGraph& graph, const PassIO& io) override;

    private:
        Context& m_Context;
        Ref<GraphicsPipeline> m_Pipeline;
        Ref<DescriptorSet> m_AtmosphereSet;
        ResourceId m_TargetId;
        ResourceId m_DepthId;
        TextureHandle m_DepthHandle;
        SamplerHandle m_SamplerHandle;
        uvec2 m_Extent;
    };
}
