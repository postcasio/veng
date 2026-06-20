#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/ScenePass.h>
#include <Veng/Renderer/Types.h>

namespace Veng
{
    class AssetManager;
    class Shader;
}

namespace Veng::Renderer
{
    class Context;
    class GraphicsPipeline;
    class PipelineLayout;

    /// @brief Renders every budgeted shadowed punctual light's depth into the renderer-owned punctual shadow atlas.
    ///
    /// The atlas is a 2D depth image of CubeFaceCount columns × MaxShadowedPunctual rows of
    /// PunctualShadowResolution² tiles (slot = row, face = column). A spot light writes one
    /// perspective view into its slot's face-0 tile; a point light writes six cube faces into
    /// its slot's six tiles. Contributes one depth-only RenderGraph pass that sets each tile's
    /// viewport + scissor, pushes the raw (non-tile-remapped) light view-proj, and culls
    /// casters against the light's own frustum through the broadphase the renderer synced once
    /// for the frame.
    ///
    /// The atlas is renderer-owned (set 1 binding 4, off bindless — a comparison-sampled image
    /// bars set-0 bindless on MoltenVK and a closed producer→consumer resource needs no global
    /// registration). The pass receives the atlas view and tile resolution through PassIO and
    /// writes io.PunctualShadowMap. The graph derives the write → sample barrier from the
    /// lighting pass's declared .Sample(io.PunctualShadowMap).
    class PunctualShadowScenePass final : public ScenePass
    {
    public:
        /// @brief Constructs the pass, loading the depth-only vertex shader and building the pipeline.
        /// @param resolution  Per-tile edge length in texels; the atlas is sized from this.
        PunctualShadowScenePass(Context& context, AssetManager& assets, u32 resolution);
        ~PunctualShadowScenePass() override;

        /// @brief Reallocates the atlas when the punctual shadow resolution in the settings changed.
        void Configure(const SceneRendererSettings& settings) override;

        /// @brief Contributes the depth-only punctual pass into the graph, writing the atlas.
        void Declare(RenderGraph& graph, const PassIO& io) override;

    private:
        Context& m_Context;
        u32 m_Resolution;
        bool m_FrustumCull = true;
        /// @brief Frustum-query scratch — candidate indices into SceneView::Visible.
        ///
        /// Cleared and refilled per view/face; reused across frames to avoid per-frame allocation.
        vector<u32> m_CullScratch;

        Ref<GraphicsPipeline> m_Pipeline;
        Ref<PipelineLayout> m_Layout;
        AssetHandle<Veng::Shader> m_VertexShader;
    };
}
