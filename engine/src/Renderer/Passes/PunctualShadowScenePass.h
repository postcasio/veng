#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/ScenePass.h>
#include <Veng/Renderer/Types.h>

#include "../DepthInstancing.h"
#include "../ShadowCasters.h"

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
    /// viewport + scissor and pushes the raw (non-tile-remapped) light view-proj. Each rendered
    /// face is one view of the renderer's ShadowCasterViews, added by AddViews before the graph
    /// runs and culled against the light's own frustum. A cube face
    /// SceneView::PunctualShadowFaceMask leaves out is not rendered. Static casters draw instanced
    /// from the shared grouping (one draw per submesh per view, each instance placed by its caster
    /// record); skinned casters draw one at a time, posed through the skinning palette.
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
        /// @param casters     The renderer's shadow views, which the faces draw from.
        /// @param resolution  Per-tile edge length in texels; the atlas is sized from this.
        PunctualShadowScenePass(Context& context, AssetManager& assets,
                                const CasterRecordRing& records, const ShadowCasterViews& casters,
                                u32 resolution);
        ~PunctualShadowScenePass() override;

        /// @brief Adds this frame's rendered faces to the shadow views.
        /// @param casters The renderer's shadow views, opened for the frame.
        /// @param view    The frame's view, carrying the punctual records and face matrices.
        void AddViews(ShadowCasterViews& casters, const SceneView& view);

        /// @brief Contributes the depth-only punctual pass into the graph, writing the atlas.
        void Declare(RenderGraph& graph, const PassIO& io) override;

    private:
        /// @brief Loads the skinned depth shader and builds the skinned caster pipeline.
        void BuildSkinnedPipeline(AssetManager& assets);

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
        /// @brief The renderer's shadow views the faces draw from.
        const ShadowCasterViews& m_Casters;
        u32 m_Resolution;
        /// @brief The faces this frame renders, in shadow-view order.
        vector<ShadowView> m_Views;
        /// @brief This frame's first face's index among the shadow views; faces follow in order.
        u32 m_FirstView = 0;

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
