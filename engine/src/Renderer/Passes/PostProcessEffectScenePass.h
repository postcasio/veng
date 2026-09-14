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

    /// @brief A game-authored fullscreen post-process effect run over scene color + depth before bloom.
    ///
    /// Models PostProcessScenePass (a fullscreen PostProcess-domain material, its source handles
    /// written into named material fields each frame) fused with SkyMaterialScenePass's set-and-gate
    /// (the material is supplied per frame and the pipeline rebuilds on a material-identity change,
    /// since the material may arrive after the graph compiled). The renderer resolves the scene's
    /// PostProcessEffect components and drives one pass per active effect at the pre-bloom tail
    /// anchor, ping-ponging between two intermediate HDR targets (a fullscreen effect samples its
    /// source and cannot run in place).
    ///
    /// Each frame the pass writes, into the material's fields when it declares them: the current
    /// scene-color source ("Scene"/"SceneSampler"), the g-buffer depth ("Depth"/"DepthSampler"), and
    /// the two mappings the fragment samples through — "SceneScaleUV" (the finished scene color
    /// fills the post-resolve allocation, so this is the identity) and "DepthScaleUV" (the g-buffer
    /// depth lives in the rendered sub-rect of the separate, render-scaled render allocation). The
    /// pass renders over the post-resolve extent, so its output is a drop-in scene-color source for
    /// the downstream pre-bloom consumers.
    class PostProcessEffectScenePass final : public ScenePass
    {
    public:
        /// @brief Constructs the pass; the pipeline builds on the first Declare with a bound material.
        /// @param context      The render context for pipeline creation.
        /// @param outputFormat Color format of the intermediate HDR target this pass writes.
        /// @param extent       The post-resolve allocation this pass writes.
        /// @param renderExtent The render allocation the depth mapping is derived against.
        PostProcessEffectScenePass(Context& context, Format outputFormat, uvec2 extent,
                                   uvec2 renderExtent);

        /// @brief Sets the effect material to run (or a null handle to disable the pass this frame).
        ///
        /// A change of material identity rebuilds the pipeline on the next Execute. The renderer
        /// calls this each Execute from the resolved PostProcessEffect list.
        /// @param material The PostProcess-domain material instance, or a default handle for none.
        void SetMaterial(AssetHandle<MaterialInstance> material);

        /// @brief Sets the graph wiring (imported source and output ids) for this Rebuild.
        ///
        /// The source is the finished scene color this effect reads (the previous effect's output,
        /// or the pre-effect scene color for the first); the output is this effect's ping-pong
        /// target. Both are graph ids that change per Rebuild, so they are set before Declare.
        /// @param source        Imported scene-color source id (declared sampled).
        /// @param sourceHandle  Bindless slot of the source, written into the "Scene" field.
        /// @param output        Imported output id this pass writes.
        void SetWiring(ResourceId source, TextureHandle sourceHandle, ResourceId output);

        /// @brief Updates the cached post-resolve allocation extent.
        void Resize(uvec2 extent) override { m_Extent = extent; }

        /// @brief Contributes the fullscreen effect pass into the graph.
        void Declare(RenderGraph& graph, const PassIO& io) override;

    private:
        /// @brief Builds the output-format-dependent fullscreen pipeline from the bound material.
        void BuildPipeline();

        /// @brief Context for pipeline creation.
        Context& m_Context;
        /// @brief Output color format the pipeline is built against.
        Format m_OutputFormat;
        /// @brief The post-resolve allocation the scene mapping is derived against.
        uvec2 m_Extent;
        /// @brief The render allocation the depth mapping is derived against.
        uvec2 m_RenderExtent;

        /// @brief Imported scene-color source id this pass samples.
        ResourceId m_Source;
        /// @brief Bindless slot of the scene-color source.
        TextureHandle m_SourceHandle;
        /// @brief Imported output id this pass writes.
        ResourceId m_Output;

        /// @brief The effect material instance driving this pass (set per frame).
        AssetHandle<MaterialInstance> m_Material;
        /// @brief Built from the material's shaders; rebuilt on a material-identity change.
        Ref<GraphicsPipeline> m_Pipeline;
        /// @brief The material id the pipeline was built for; a change rebuilds it.
        u64 m_PipelineMaterialId = 0;
    };
}
