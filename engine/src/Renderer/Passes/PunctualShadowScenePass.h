#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/ScenePass.h>
#include <Veng/Renderer/Types.h>

#include "../DepthInstancing.h"

namespace Veng
{
    class AssetManager;
    class Shader;
}

namespace Veng::Renderer
{
    class Context;
    class DescriptorSetLayout;
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
    /// for the frame. A cube face SceneView::PunctualShadowFaceMask leaves out is not rendered.
    /// Static casters draw instanced (one draw per submesh per view, each instance placed by its
    /// caster record); skinned casters draw one at a time, posed through the skinning palette.
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
        /// @param context     Renderer context.
        /// @param assets      Asset manager for the core-pack shader loads.
        /// @param records     The renderer's caster records, read by the static caster draws.
        /// @param resolution  Per-tile edge length in texels; the atlas is sized from this.
        PunctualShadowScenePass(Context& context, AssetManager& assets,
                                const CasterRecordRing& records, u32 resolution);
        ~PunctualShadowScenePass() override;

        /// @brief Reads the frustum-cull toggle from the settings.
        void Configure(const SceneRendererSettings& settings) override;

        /// @brief Contributes the depth-only punctual pass into the graph, writing the atlas.
        void Declare(RenderGraph& graph, const PassIO& io) override;

    private:
        /// @brief Loads the skinned depth shader and builds the skinned caster pipeline.
        void BuildSkinnedPipeline(AssetManager& assets);

        /// @brief Culls and sorts every rendered view's casters into the batch and the skinned list.
        void BuildViews(const SceneView& view);

        /// @brief One rendered view: a record's slot and one of its faces.
        struct ShadowView
        {
            /// @brief The record's slot, the atlas row.
            u32 Slot = 0;
            /// @brief The face, the atlas column.
            u32 Face = 0;
        };

        Context& m_Context;
        /// @brief The renderer's caster records the static draws place their instances by.
        const CasterRecordRing& m_Records;
        u32 m_Resolution;
        bool m_FrustumCull = true;
        /// @brief Frustum-query scratch — candidate indices into SceneView::Visible.
        ///
        /// Cleared and refilled per view/face; reused across frames to avoid per-frame allocation.
        vector<u32> m_CullScratch;
        /// @brief The views this frame renders, in batch view order.
        vector<ShadowView> m_Views;
        /// @brief Every view's static casters, as instanced runs.
        DepthInstanceBatch m_Batch;
        /// @brief Every view's skinned caster candidate ids, view after view.
        vector<u32> m_Skinned;
        /// @brief Per view, the end of its range in m_Skinned.
        vector<u32> m_SkinnedViewEnds;

        Ref<GraphicsPipeline> m_Pipeline;
        Ref<PipelineLayout> m_Layout;
        AssetHandle<Veng::Shader> m_VertexShader;

        // The skinned caster path: a parallel depth pipeline driven by the skinned vertex stage,
        // with the per-instance palette bound at set 3 and PaletteBase in the push block.
        Ref<GraphicsPipeline> m_SkinnedPipeline;
        Ref<PipelineLayout> m_SkinnedLayout;
        Ref<DescriptorSetLayout> m_PaletteSetLayout;
        AssetHandle<Veng::Shader> m_SkinnedVertexShader;
    };
}
