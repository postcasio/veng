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

    /// @brief The lean geometry render's prepass: rasterizes opaque geometry to depth + world normal.
    ///
    /// The single pass of the shading-free RenderPath::GeometryDepthNormal topology. It draws the
    /// camera-visible opaque submeshes into a one-MRT world-normal target plus a depth attachment,
    /// running no material shader, no lighting, and no tonemap — the whole cost saving. It follows
    /// the depth-only rasterization approach of the shadow caster (ShadowScenePass) and shares the
    /// canonical/skinned vertex buffer layouts with it, but is a distinct pipeline: its vertex
    /// shaders pass a world-space normal varying (skinned meshes skin the normal too), its fragment
    /// writes the normal as one MRT, and it culls back faces (CullMode::Back) rather than the shadow
    /// caster's front faces. Static meshes draw instanced as the shadow caster's do: the survivors
    /// sorted by (mesh, submesh), one instanced draw per submesh, each instance's world and normal
    /// matrices read from its caster record and the camera view-projection pushed once. Skinned
    /// meshes draw one at a time with their transform in the push block.
    ///
    /// Opaque geometry only, as the shadow caster is — a Translucent submesh writes no depth here.
    class DepthNormalPrepassScenePass final : public ScenePass
    {
    public:
        /// @brief Constructs the pass, loading the normal-passing vertex/fragment shaders and
        ///        building the static and skinned pipelines with back-face culling.
        /// @param context   Renderer context for pipeline and bindless access.
        /// @param assets    Asset manager for the core-pack shader loads.
        /// @param records   The renderer's caster records, read by the static draws.
        /// @param extent    Initial render extent; updated via Resize.
        /// @param normalId  Imported world-normal target the pass writes as its one MRT.
        /// @param depthId   Imported depth attachment the pass writes.
        DepthNormalPrepassScenePass(Context& context, AssetManager& assets,
                                    const CasterRecordRing& records, uvec2 extent,
                                    ResourceId normalId, ResourceId depthId);
        ~DepthNormalPrepassScenePass() override;

        /// @brief Updates the render extent the draws viewport into.
        void Resize(uvec2 extent) override { m_Extent = extent; }

        /// @brief Reads whether the frustum cull runs from the settings.
        void Configure(const SceneRendererSettings& settings) override;

        /// @brief Contributes the depth + world-normal prepass into the graph.
        void Declare(RenderGraph& graph, const PassIO& io) override;

    private:
        /// @brief Builds the skinned normal-passing pipeline over the palette set (set 3).
        void BuildSkinnedPipeline(AssetManager& assets);

        /// @brief Renderer context for pipeline and bindless access.
        Context& m_Context;
        /// @brief The renderer's caster records the static draws place their instances by.
        const CasterRecordRing& m_Records;
        /// @brief Current render extent.
        uvec2 m_Extent;
        /// @brief Imported world-normal target id (one MRT).
        ResourceId m_NormalId;
        /// @brief Imported depth attachment id.
        ResourceId m_DepthId;
        /// @brief Whether draws are frustum-culled against the camera.
        bool m_FrustumCull = true;
        /// @brief Frustum-query scratch — candidate indices into SceneView::Visible; reused per frame.
        vector<u32> m_CullScratch;
        /// @brief The static survivors, as instanced runs.
        DepthInstanceBatch m_Batch;
        /// @brief The skinned survivors' candidate ids.
        vector<u32> m_Skinned;

        /// @brief Static (canonical layout) normal-passing pipeline and its layout.
        Ref<GraphicsPipeline> m_Pipeline;
        /// @brief Layout for m_Pipeline (the caster records at set 3, one vertex push range).
        Ref<PipelineLayout> m_Layout;
        /// @brief The static normal-passing vertex shader.
        AssetHandle<Veng::Shader> m_VertexShader;
        /// @brief The one-MRT world-normal fragment shader.
        AssetHandle<Veng::Shader> m_FragmentShader;

        /// @brief Skinned normal-passing pipeline (palette at set 3) and its layout.
        Ref<GraphicsPipeline> m_SkinnedPipeline;
        /// @brief Layout for m_SkinnedPipeline.
        Ref<PipelineLayout> m_SkinnedLayout;
        /// @brief The palette set (set 3) layout matching the renderer's palette descriptor set.
        Ref<DescriptorSetLayout> m_PaletteSetLayout;
        /// @brief The skinned normal-passing vertex shader.
        AssetHandle<Veng::Shader> m_SkinnedVertexShader;
    };
}
