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

    // The cascaded directional-shadow pass. It owns one D32 depth atlas sized to
    // the cascade count — a min(Count,2)×ceil(Count/2) grid of ShadowResolution²
    // tiles (1×1 for one cascade, 2×1 for two, 2×2 for three or four) — and
    // contributes one depth-only RenderGraph pass that renders the scene's opaque
    // meshes once per cascade, each into its tile via a per-cascade viewport with
    // that cascade's raw light-space matrix pushed. The one grid cell beyond Count
    // (the fourth cell at Count three) is left at the attachment's depth=1 clear and
    // never selected.
    //
    // The atlas is NOT in bindless: a shadow map is a closed producer→consumer
    // resource, delivered to the lighting pass through a dedicated descriptor set
    // (set 1). The pass exposes its Ref<ImageView> (GetShadowView) for that handoff.
    // The target is single-copy and consumed within the renderer's internal graph;
    // the graph derives its write→sample barrier from the lighting pass's declared
    // .Sample(ShadowMap). On recreate (Configure/Resize) the old image retires
    // through the deferred per-frame window, so an in-flight frame's sample is never
    // reclaimed early.
    class ShadowScenePass final : public ScenePass
    {
    public:
        ShadowScenePass(Context& context, AssetManager& assets, u32 resolution, u32 cascadeCount);
        ~ShadowScenePass() override;

        // The atlas view, threaded into the lighting pipeline's set-1 bound-view
        // slot and written into the shadow descriptor set.
        [[nodiscard]] const Ref<ImageView>& GetShadowView() const { return m_ShadowView; }

        // The per-cascade tile edge length in texels (one tile is Resolution²).
        [[nodiscard]] u32 GetResolution() const { return m_Resolution; }

        // The number of cascades (and tiles) the atlas was sized for.
        [[nodiscard]] u32 GetCascadeCount() const { return m_CascadeCount; }

        // The atlas tile grid: Columns × Rows of Resolution² tiles. Cascade k maps
        // to tile (k % Columns, k / Columns).
        [[nodiscard]] u32 GetTileColumns() const { return m_TileColumns; }
        [[nodiscard]] u32 GetTileRows() const { return m_TileRows; }

        // The atlas's full pixel extent (Columns·Resolution × Rows·Resolution).
        [[nodiscard]] uvec2 GetAtlasExtent() const
        {
            return {m_TileColumns * m_Resolution, m_TileRows * m_Resolution};
        }

        void Declare(RenderGraph& graph, const PassIO& io) override;

    private:
        // Recreate the depth atlas at the current resolution × cascade-count grid,
        // retiring the old image through the deferred window.
        void CreateAtlas();

        Context& m_Context;
        u32 m_Resolution;
        u32 m_CascadeCount;
        u32 m_TileColumns;
        u32 m_TileRows;

        Ref<Image> m_ShadowImage;
        Ref<ImageView> m_ShadowView;

        Ref<GraphicsPipeline> m_Pipeline;
        Ref<PipelineLayout> m_Layout;
        AssetHandle<Veng::Shader> m_VertexShader;
    };
}
