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

    /// @brief Fullscreen skybox pass: fills the cleared-depth background with the environment
    ///        radiance cube, compositing over the lit scene color.
    ///
    /// Runs after deferred lighting (writing the same scene-color target, LoadOp::Load) and
    /// before the bloom/SSR/tonemap tail, so the sky tonemaps and reflects with the scene. It
    /// samples the g-buffer depth through bindless and discards foreground pixels, and reads the
    /// radiance cube + linear sampler from the renderer's IBL set (bound at set 1). Topology is
    /// gated by SceneRendererSettings::Skybox; per frame the draw is a no-op (everything
    /// discarded) unless an environment is bound — the renderer pushes that as the Enabled flag.
    class SkyboxScenePass final : public ScenePass
    {
    public:
        /// @brief Constructs the pass.
        /// @param context       The render context.
        /// @param pipeline      The fullscreen skybox pipeline (reserves set 0 bindless + set 1 IBL).
        /// @param iblSet        The renderer's IBL descriptor set (radiance cube at binding 0, sampler at 4).
        /// @param targetId      The scene-color target lighting wrote (this pass composites into it).
        /// @param depthId       The g-buffer depth resource, declared sampled for barrier derivation.
        /// @param depthHandle   The bindless handle for the depth target.
        /// @param samplerHandle The shared sampler bindless handle.
        /// @param extent        The render extent.
        SkyboxScenePass(Context& context, Ref<GraphicsPipeline> pipeline, Ref<DescriptorSet> iblSet,
                        ResourceId targetId, ResourceId depthId, TextureHandle depthHandle,
                        SamplerHandle samplerHandle, uvec2 extent);

        /// @brief Updates the cached render extent.
        void Resize(uvec2 extent) override;

        /// @brief Contributes the fullscreen skybox pass into the graph.
        void Declare(RenderGraph& graph, const PassIO& io) override;

    private:
        Context& m_Context;
        Ref<GraphicsPipeline> m_Pipeline;
        Ref<DescriptorSet> m_IblSet;
        ResourceId m_TargetId;
        ResourceId m_DepthId;
        TextureHandle m_DepthHandle;
        SamplerHandle m_SamplerHandle;
        uvec2 m_Extent;
    };
}
