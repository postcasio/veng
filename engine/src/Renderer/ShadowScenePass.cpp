#include "ShadowScenePass.h"

#include <span>

#include <fmt/format.h>

#include <Veng/Assert.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/ShaderInterface.h>
#include <Veng/Renderer/VertexBufferLayout.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Asset/VertexLayout.h>

#include <Veng/Math/Frustum.h>

#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/Visibility.h>

namespace Veng::Renderer
{
    namespace
    {
        // The core pack's depth-only shadow vertex shader (canonical layout in,
        // light-space MVP, no fragment stage).
        constexpr AssetId ShadowDepthVertId{0x156C14C99FFF6B7CULL};

        // Depth format and usage: a single-channel float depth target written per cascade
        // and sampled by the lighting pass through its dedicated set.
        constexpr Format ShadowFormat = Format::D32Sfloat;
        constexpr ImageUsage ShadowUsage = ImageUsage::DepthAttachment | ImageUsage::Sampled;

        // The depth-only vertex push block: light-space MVP at offset 0, matching the
        // shared push block's leading float4x4 (64 bytes).
        struct ShadowPushConstants
        {
            mat4 MVP;
        };
    }

    ShadowScenePass::ShadowScenePass(Context& context, AssetManager& assets, u32 resolution,
                                     u32 cascadeCount)
        : m_Context(context), m_Resolution(resolution), m_CascadeCount(cascadeCount)
    {
        const ShadowAtlasGrid grid = ComputeShadowAtlasGrid(cascadeCount);
        m_TileColumns = grid.Columns;
        m_TileRows = grid.Rows;

        const AssetResult<AssetHandle<Veng::Shader>> vs =
            assets.LoadSync<Veng::Shader>(ShadowDepthVertId);
        VE_ASSERT(vs.has_value(), "ShadowScenePass: depth vertex shader load failed: {}",
                  vs.error().Detail);
        m_VertexShader = *vs;

        // Depth-only pipeline: set 0 reserved, one vertex push range for the
        // light-space MVP, no fragment stage, depth write on, no color targets.
        m_Layout = PipelineLayout::Create(
            m_Context, {
                           .Name = "ShadowScenePass Layout",
                           .PushConstantRanges = {PushConstantRange::Of<ShadowPushConstants>(
                               ShaderStage::Vertex)},
                       });

        optional<VertexBufferLayout> vertexBufferLayout;
        const Renderer::ShaderInterface& vsInterface = m_VertexShader.Get()->Interface;
        if (vsInterface.VertexLayoutId.has_value())
        {
            const AssetResult<AssetHandle<Veng::VertexLayout>> layoutResult =
                assets.LoadSync<Veng::VertexLayout>(*vsInterface.VertexLayoutId);
            VE_ASSERT(layoutResult.has_value(), "ShadowScenePass: vertex layout load failed: {}",
                      layoutResult.error().Detail);
            vertexBufferLayout = layoutResult->Get()->GetLayout();
        }

        m_Pipeline = GraphicsPipeline::Create(
            m_Context,
            {
                .Name = "ShadowScenePass Depth Pipeline",
                .ColorAttachments = {},
                .DepthAttachmentFormat = ShadowFormat,
                .VertexBufferLayout = vertexBufferLayout,
                .PipelineLayout = m_Layout,
                .ShaderStages =
                    {
                        {.Stage = ShaderStage::Vertex, .Module = m_VertexShader.Get()->Module},
                    },
                // Front-face culling keeps the caster's back faces in the depth map,
                // pushing self-shadow acne to the lit side where the slope bias covers it.
                .CullMode = CullMode::Front,
                .DepthTestEnable = true,
                .DepthWriteEnable = true,
            });

        CreateAtlas();
    }

    ShadowScenePass::~ShadowScenePass() = default;

    void ShadowScenePass::CreateAtlas()
    {
        // Recreate the depth atlas at the current resolution × tile grid.
        const uvec2 atlasExtent = GetAtlasExtent();

        m_ShadowImage = Image::Create(m_Context, {
                                                     .Name = "ShadowScenePass Atlas",
                                                     .Extent = {atlasExtent.x, atlasExtent.y, 1},
                                                     .Format = ShadowFormat,
                                                     .Usage = ShadowUsage,
                                                 });
        m_ShadowView = ImageView::Create(m_Context, {
                                                        .Name = "ShadowScenePass Atlas View",
                                                        .Image = m_ShadowImage,
                                                    });
    }

    void ShadowScenePass::Configure(const SceneRendererSettings& settings)
    {
        m_FrustumCull = settings.FrustumCull;
    }

    void ShadowScenePass::Declare(RenderGraph& graph, const PassIO& io)
    {
        const u32 resolution = m_Resolution;
        const u32 columns = m_TileColumns;
        const u32 cascadeCount = m_CascadeCount;

        graph.AddPass("Shadow Depth")
            .Depth({
                .Resource = io.ShadowMap,
                .Load = LoadOp::Clear,
                .Store = StoreOp::Store,
                // The whole atlas clears to depth = 1; an unused tile (the fourth
                // cell at three cascades) keeps this clear and is never selected.
                .Clear = ClearDepth{.Depth = 1.0f, .Stencil = 0},
            })
            .Execute(
                [this, resolution, columns, cascadeCount](PassContext& inner)
                {
                    const ScenePassContext ctx = Wrap(inner);
                    CommandBuffer& cmd = ctx.Cmd();
                    const SceneView& view = ctx.View();
                    const BindlessRegistry& registry = m_Context.GetBindlessRegistry();

                    cmd.BindPipeline(m_Pipeline);
                    registry.Bind(cmd);

                    // Render each cascade into its tile: set the viewport + scissor to the
                    // tile sub-rect, push cascade k's raw light-space matrix, and draw.
                    const u32 count =
                        cascadeCount < view.CascadeCount ? cascadeCount : view.CascadeCount;
                    for (u32 k = 0; k < count; ++k)
                    {
                        const ivec2 tileOffset{
                            static_cast<i32>((k % columns) * resolution),
                            static_cast<i32>((k / columns) * resolution),
                        };
                        cmd.SetViewport(tileOffset, {resolution, resolution});
                        cmd.SetScissor(tileOffset, {resolution, resolution});

                        const mat4 lightViewProj = view.CascadeViewProj[k];

                        // Cull against the cascade's own light frustum: an off-screen mesh
                        // can still cast a shadow into view; CascadeViewProj[k] is fit to the
                        // camera slice and extended toward the light, so only what falls
                        // completely outside the cascade's volume is dropped.
                        const Frustum cascadeFrustum = Frustum::FromViewProjection(lightViewProj);

                        const auto Draw = [&](const VisibleMesh& item)
                        {
                            const Mesh& mesh = *item.Mesh;
                            const std::span<const AssetHandle<Material>> materials =
                                mesh.GetMaterials();

                            bool materialsReady = true;
                            for (const AssetHandle<Material>& material : materials)
                            {
                                materialsReady = materialsReady && material.IsLoaded();
                            }
                            if (!materialsReady)
                            {
                                return;
                            }

                            cmd.BindVertexBuffer(mesh.GetVertexBuffer());
                            cmd.BindIndexBuffer(mesh.GetIndexBuffer());

                            const mat4 mvp = lightViewProj * item.World;
                            cmd.PushConstants(ShadowPushConstants{.MVP = mvp});

                            for (const SubMesh& subMesh : mesh.GetSubMeshes())
                            {
                                if (subMesh.MaterialIndex == SubMesh::NoMaterial)
                                {
                                    continue;
                                }
                                cmd.DrawIndexed(subMesh.IndexCount, 1, subMesh.IndexOffset, 0, 0);
                            }
                        };

                        if (m_FrustumCull)
                        {
                            m_CullScratch.clear();
                            view.Broadphase->Cull(cascadeFrustum, m_CullScratch);
                            for (const u32 idx : m_CullScratch)
                            {
                                Draw(view.Visible[idx]);
                            }
                        }
                        else
                        {
                            for (const VisibleMesh& item : view.Visible)
                            {
                                Draw(item);
                            }
                        }
                    }
                });
    }
}
