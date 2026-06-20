#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/ScenePass.h>
#include <Veng/Renderer/ShadowCascades.h>
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
    class Image;
    class PipelineLayout;

    /// @brief Cascaded directional-shadow depth pass, owning one D32 atlas and one RenderGraph depth pass.
    ///
    /// The atlas is a min(Count,2)×ceil(Count/2) grid of ShadowResolution² tiles (1×1 for one
    /// cascade, 2×1 for two, 2×2 for three or four). Each cascade renders the scene's opaque meshes
    /// into its tile via a per-cascade viewport with the cascade's raw light-space matrix pushed.
    /// The grid cell beyond Count keeps the depth=1 clear and is never selected.
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
        ShadowScenePass(Context& context, AssetManager& assets, u32 resolution, u32 cascadeCount);
        ~ShadowScenePass() override;

        /// @brief The atlas view, written into the shadow descriptor set (set 1 binding 0).
        [[nodiscard]] const Ref<ImageView>& GetShadowView() const { return m_ShadowView; }

        /// @brief Per-cascade tile edge length in texels; one tile is Resolution².
        [[nodiscard]] u32 GetResolution() const { return m_Resolution; }

        /// @brief Number of cascades the atlas was sized for.
        [[nodiscard]] u32 GetCascadeCount() const { return m_CascadeCount; }

        /// @brief Number of atlas tile columns. Cascade k maps to tile column k % Columns.
        [[nodiscard]] u32 GetTileColumns() const { return m_TileColumns; }

        /// @brief Number of atlas tile rows. Cascade k maps to tile row k / Columns.
        [[nodiscard]] u32 GetTileRows() const { return m_TileRows; }

        /// @brief Full atlas extent in pixels (Columns·Resolution × Rows·Resolution).
        [[nodiscard]] uvec2 GetAtlasExtent() const
        {
            return {m_TileColumns * m_Resolution, m_TileRows * m_Resolution};
        }

        /// @brief Reallocates the atlas when the resolution or cascade count in the settings changed.
        void Configure(const SceneRendererSettings& settings) override;

        /// @brief Contributes the cascaded depth pass into the graph, writing the atlas.
        void Declare(RenderGraph& graph, const PassIO& io) override;

    private:
        /// @brief Allocates or reallocates the depth atlas at the current resolution × grid.
        void CreateAtlas();

        Context& m_Context;
        u32 m_Resolution;
        u32 m_CascadeCount;
        u32 m_TileColumns;
        u32 m_TileRows;
        bool m_FrustumCull = true;
        /// @brief Frustum-query scratch — candidate indices into SceneView::Visible.
        ///
        /// Cleared and refilled per cascade; reused across frames to avoid per-frame allocation.
        vector<u32> m_CullScratch;

        Ref<Image> m_ShadowImage;
        Ref<ImageView> m_ShadowView;

        Ref<GraphicsPipeline> m_Pipeline;
        Ref<PipelineLayout> m_Layout;
        AssetHandle<Veng::Shader> m_VertexShader;
    };
}
