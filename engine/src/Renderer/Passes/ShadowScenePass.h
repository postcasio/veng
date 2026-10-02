#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/ScenePass.h>
#include <Veng/Renderer/ShadowCascades.h>
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
    class Image;
    class PipelineLayout;

    /// @brief Cascaded directional-shadow depth pass, owning one D32 atlas and one RenderGraph depth pass.
    ///
    /// One cascade set occupies a min(Count,2)×ceil(Count/2) grid of ShadowResolution² tiles (1×1
    /// for one cascade, 2×1 for two, 2×2 for three or four), and MaxCascadeSets of those stack as
    /// row bands — so the atlas is always sized for the full set budget, and a frame lighting from
    /// fewer sources simply leaves the upper bands at their clear. Each cascade renders the scene's
    /// opaque meshes into its tile via a per-cascade viewport with that cascade's raw light-space
    /// matrix pushed. A tile beyond the frame's cascade or set count keeps the clear and is never
    /// selected.
    ///
    /// Each tile is one view of the renderer's ShadowCasterViews, added by AddViews before the graph
    /// runs: static casters draw instanced from the shared grouping, one instanced draw per submesh,
    /// each instance placed by its caster record. A caster whose bound spans fewer than
    /// ShadowCasterMinTexels texels in a tile is skipped there. Skinned casters draw one at a time,
    /// posed through the skinning palette.
    ///
    /// The atlas is off bindless: it is a closed producer→consumer resource delivered to the lighting
    /// pass through a dedicated descriptor set (set 1). GetShadowView exposes the Ref<ImageView>
    /// for that handoff. The graph derives the write→sample barrier from the lighting pass's
    /// declared .Sample(ShadowMap). On recreate (Configure/Resize) the old image is deferred
    /// until the GPU is done with it.
    class ShadowScenePass final : public ScenePass
    {
    public:
        /// @brief Constructs the pass, loading the depth-only vertex shader, building the pipeline,
        ///        and allocating the atlas at the given resolution × cascade-count grid.
        /// @param context      Renderer context.
        /// @param assets       Asset manager for the core-pack shader loads.
        /// @param records      The renderer's caster records, read by the static caster draws.
        /// @param casters      The renderer's shadow views, which the tiles draw from.
        /// @param resolution   Per-cascade tile edge length in texels.
        /// @param cascadeCount Cascades per set the atlas is sized for.
        ShadowScenePass(Context& context, AssetManager& assets, const CasterRecordRing& records,
                        const ShadowCasterViews& casters, u32 resolution, u32 cascadeCount);
        ~ShadowScenePass() override;

        /// @brief The atlas view, written into the shadow descriptor set (set 1 binding 0).
        [[nodiscard]] const Ref<ImageView>& GetShadowView() const { return m_ShadowView; }

        /// @brief Per-cascade tile edge length in texels; one tile is Resolution².
        [[nodiscard]] u32 GetResolution() const { return m_Resolution; }

        /// @brief Number of cascades the atlas was sized for.
        [[nodiscard]] u32 GetCascadeCount() const { return m_CascadeCount; }

        /// @brief Number of atlas tile columns. Cascade k maps to tile column k % Columns.
        [[nodiscard]] u32 GetTileColumns() const { return m_Grid.Columns; }

        /// @brief Number of atlas tile rows across every stacked cascade set.
        [[nodiscard]] u32 GetTileRows() const { return m_Grid.TotalRows(); }

        /// @brief Full atlas extent in pixels (Columns·Resolution × TotalRows·Resolution).
        [[nodiscard]] uvec2 GetAtlasExtent() const
        {
            return {m_Grid.Columns * m_Resolution, m_Grid.TotalRows() * m_Resolution};
        }

        /// @brief Reads the caster size threshold from the settings.
        void Configure(const SceneRendererSettings& settings) override;

        /// @brief Adds this frame's tiles to the shadow views: every granted set's cascades.
        /// @param casters The renderer's shadow views, opened for the frame.
        /// @param view    The frame's view, carrying the cascade matrices.
        void AddViews(ShadowCasterViews& casters, const SceneView& view);

        /// @brief Contributes the cascaded depth pass into the graph, writing the atlas.
        void Declare(RenderGraph& graph, const PassIO& io) override;

    private:
        /// @brief Allocates or reallocates the depth atlas at the current resolution × grid.
        void CreateAtlas();

        /// @brief Loads the skinned depth shader and builds the skinned caster pipeline.
        void BuildSkinnedPipeline(AssetManager& assets);

        Context& m_Context;
        /// @brief The renderer's caster records the static draws place their instances by.
        const CasterRecordRing& m_Records;
        u32 m_Resolution;
        u32 m_CascadeCount;
        /// @brief The renderer's shadow views the tiles draw from.
        const ShadowCasterViews& m_Casters;
        /// @brief The atlas tile grid: one set's columns/rows plus the stacked set count.
        ShadowAtlasGrid m_Grid;
        /// @brief A caster spanning fewer texels than this in a tile is skipped there.
        f32 m_MinCasterTexels = 1.0f;
        /// @brief This frame's first tile's index among the shadow views; tiles follow in order.
        u32 m_FirstView = 0;
        /// @brief Tiles this frame renders: granted sets times cascades.
        u32 m_TileCount = 0;
        /// @brief Cascades per set this frame renders.
        u32 m_FrameCascadeCount = 0;

        Ref<Image> m_ShadowImage;
        Ref<ImageView> m_ShadowView;

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
