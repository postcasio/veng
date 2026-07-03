#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/ScenePass.h>
#include <Veng/Renderer/Types.h>

namespace Veng
{
    class MaterialInstance;
}

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

    /// @brief Authored-material sky source: runs a game-authored Sky-domain material fullscreen
    ///        in the sky slot, compositing its radiance over the lit scene color.
    ///
    /// Occupies the same slot as SkyScenePass / SkyboxScenePass — after deferred lighting (writing
    /// the scene-color target, LoadOp::Load) and before the bloom/SSR/tonemap tail — so an authored
    /// sky is a peer sky source to the cubemap skybox and the procedural atmosphere. Mirrors the
    /// PostProcessScenePass pattern: it builds a GraphicsPipeline from the Sky material's fragment
    /// shader against the renderer's scene-color format, binds set 0 (bindless), and pushes the
    /// material's SkyPushConstants (the frame-folded selector + the g-buffer depth handle/sampler +
    /// the view-constants index). The material's fragment reconstructs the view ray, reads the depth
    /// for the foreground discard, and writes sky radiance. The pass is a no-op (no bound material)
    /// until the renderer supplies one for the frame.
    class SkyMaterialScenePass final : public ScenePass
    {
    public:
        /// @brief Constructs the pass.
        /// @param context       The render context.
        /// @param targetId      The scene-color target lighting wrote (this pass composites into it).
        /// @param depthId       The g-buffer depth resource, declared sampled for barrier derivation.
        /// @param depthHandle   The bindless handle for the depth target.
        /// @param samplerHandle The shared sampler bindless handle.
        /// @param targetFormat  The scene-color format the material's pipeline is built against.
        /// @param extent        The render extent.
        SkyMaterialScenePass(Context& context, ResourceId targetId, ResourceId depthId,
                             TextureHandle depthHandle, SamplerHandle samplerHandle,
                             Format targetFormat, uvec2 extent);

        /// @brief Sets the Sky material to run this frame (or a null handle to disable the pass).
        ///
        /// A change of material identity rebuilds the pipeline on the next Declare. The renderer
        /// calls this each Execute from the per-frame SceneView.
        /// @param material The Sky-domain material instance, or a default (empty) handle for none.
        void SetMaterial(AssetHandle<MaterialInstance> material);

        /// @brief Updates the cached render extent.
        void Resize(uvec2 extent) override;

        /// @brief Contributes the fullscreen sky-material pass into the graph.
        void Declare(RenderGraph& graph, const PassIO& io) override;

    private:
        /// @brief Builds the fullscreen pipeline from the bound Sky material's shaders.
        void BuildPipeline();

        Context& m_Context;
        ResourceId m_TargetId;
        ResourceId m_DepthId;
        TextureHandle m_DepthHandle;
        SamplerHandle m_SamplerHandle;
        Format m_TargetFormat;
        uvec2 m_Extent;

        AssetHandle<MaterialInstance> m_Material;
        Ref<GraphicsPipeline> m_Pipeline;
        u64 m_PipelineMaterialId = 0;
    };
}
