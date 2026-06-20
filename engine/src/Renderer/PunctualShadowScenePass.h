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

    // The punctual shadow depth pass. It renders every budgeted shadowed punctual
    // light's depth into the renderer-owned punctual shadow atlas: a spot light is
    // one perspective view into its slot's face-0 tile, a point light is six cube
    // faces into its slot's six tiles. The atlas tiles CubeFaceCount columns ×
    // MaxShadowedPunctual rows of PunctualShadowResolution² (slot = row, face =
    // column), matching the tile layout the lighting sample's tile-remap targets.
    //
    // It contributes one depth-only RenderGraph pass writing the whole atlas, reusing
    // the cascade pass's depth-only pipeline shape (shadow_depth.vert, light-space MVP
    // push at offset 0, front-face cull, per-submesh depth draw). Each view sets its
    // tile's viewport + scissor, pushes the RAW (non-tile-remapped) light view-proj,
    // and culls casters against that raw matrix's frustum through the broadphase the
    // renderer synced once for the frame — the light's own frustum, never the camera's.
    //
    // The atlas is renderer-owned (set 1 binding 4, off bindless — a closed
    // producer→consumer resource needs no bindless registration, and a
    // comparison-sampled image bars set-0 bindless on MoltenVK). The pass receives the
    // atlas view and its tile resolution from the renderer through PassIO; it writes
    // the io.PunctualShadowMap import. The graph derives the write → sample barrier
    // from the lighting pass's declared .Sample(io.PunctualShadowMap).
    class PunctualShadowScenePass final : public ScenePass
    {
    public:
        PunctualShadowScenePass(Context& context, AssetManager& assets, u32 resolution);
        ~PunctualShadowScenePass() override;

        void Configure(const SceneRendererSettings& settings) override;
        void Declare(RenderGraph& graph, const PassIO& io) override;

    private:
        Context& m_Context;
        u32 m_Resolution;
        bool m_FrustumCull = true;
        // The reused per-view frustum-query result scratch (candidate indices into
        // SceneView::Visible). Cleared and refilled per view/face; reused across frames
        // so the steady state allocates nothing.
        vector<u32> m_CullScratch;

        Ref<GraphicsPipeline> m_Pipeline;
        Ref<PipelineLayout> m_Layout;
        AssetHandle<Veng::Shader> m_VertexShader;
    };
}
