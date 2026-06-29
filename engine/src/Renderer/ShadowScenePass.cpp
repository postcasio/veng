#include "ShadowScenePass.h"

#include <span>

#include <fmt/format.h>

#include <Veng/Assert.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/DescriptorSetLayout.h>
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

        // The skinned depth-only shadow vertex shader (skinned layout in, palette at set 1,
        // light-space MVP + PaletteBase push).
        constexpr AssetId ShadowDepthSkinnedVertId{0x83DB748493614120ULL};

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

        // The skinned depth-only push block: light-space MVP plus the instance's PaletteBase,
        // matching shadow_depth_skinned.vert's push block.
        struct ShadowSkinnedPushConstants
        {
            mat4 MVP;
            u32 PaletteBase;
        };

        // Packs an Entity into the u64 key the renderer's per-entity palette map uses.
        u64 PackEntity(Entity e)
        {
            return (static_cast<u64>(e.Index) << 32) | static_cast<u64>(e.Generation);
        }
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

        BuildSkinnedPipeline(assets, vertexBufferLayout.has_value());

        CreateAtlas();
    }

    void ShadowScenePass::BuildSkinnedPipeline(AssetManager& assets, bool /*hasStaticLayout*/)
    {
        const AssetResult<AssetHandle<Veng::Shader>> vs =
            assets.LoadSync<Veng::Shader>(ShadowDepthSkinnedVertId);
        VE_ASSERT(vs.has_value(), "ShadowScenePass: skinned depth vertex shader load failed: {}",
                  vs.error().Detail);
        m_SkinnedVertexShader = *vs;

        // The palette set (set 1): one storage buffer, vertex stage — matching the renderer's
        // palette descriptor set and the skinned shader's reflected set 1.
        m_PaletteSetLayout = DescriptorSetLayout::Create(
            m_Context, {
                           .Name = "ShadowScenePass Palette Set Layout",
                           .Bindings = {{.Binding = 0,
                                         .Type = DescriptorType::StorageBuffer,
                                         .Count = 1,
                                         .Stages = ShaderStage::Vertex}},
                       });

        m_SkinnedLayout = PipelineLayout::Create(
            m_Context, {
                           .Name = "ShadowScenePass Skinned Layout",
                           .DescriptorSetLayouts = {m_PaletteSetLayout},
                           .PushConstantRanges = {PushConstantRange::Of<ShadowSkinnedPushConstants>(
                               ShaderStage::Vertex)},
                       });

        optional<VertexBufferLayout> skinnedLayout;
        const Renderer::ShaderInterface& vsInterface = m_SkinnedVertexShader.Get()->Interface;
        if (vsInterface.VertexLayoutId.has_value())
        {
            const AssetResult<AssetHandle<Veng::VertexLayout>> layoutResult =
                assets.LoadSync<Veng::VertexLayout>(*vsInterface.VertexLayoutId);
            VE_ASSERT(layoutResult.has_value(),
                      "ShadowScenePass: skinned vertex layout load failed: {}",
                      layoutResult.error().Detail);
            skinnedLayout = layoutResult->Get()->GetLayout();
        }

        m_SkinnedPipeline = GraphicsPipeline::Create(
            m_Context, {
                           .Name = "ShadowScenePass Skinned Depth Pipeline",
                           .ColorAttachments = {},
                           .DepthAttachmentFormat = ShadowFormat,
                           .VertexBufferLayout = skinnedLayout,
                           .PipelineLayout = m_SkinnedLayout,
                           .ShaderStages =
                               {
                                   {.Stage = ShaderStage::Vertex,
                                    .Module = m_SkinnedVertexShader.Get()->Module},
                               },
                           .CullMode = CullMode::Front,
                           .DepthTestEnable = true,
                           .DepthWriteEnable = true,
                       });
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

                    // Render each cascade into its tile: set the viewport + scissor to the
                    // tile sub-rect, push cascade k's raw light-space matrix, and draw.
                    const u32 count =
                        cascadeCount < view.CascadeCount ? cascadeCount : view.CascadeCount;

                    const std::span<const SubMeshCandidate> candidates =
                        view.Broadphase->GetSubMeshCandidates();

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

                        m_CullScratch.clear();
                        if (m_FrustumCull)
                        {
                            view.Broadphase->Cull(cascadeFrustum, m_CullScratch);
                        }
                        else
                        {
                            for (u32 i = 0; i < candidates.size(); ++i)
                            {
                                m_CullScratch.push_back(i);
                            }
                        }

                        // Static casters: the canonical-layout depth pipeline.
                        cmd.BindPipeline(m_Pipeline);
                        registry.Bind(cmd);
                        const Mesh* lastBound = nullptr;
                        for (const u32 id : m_CullScratch)
                        {
                            const SubMeshCandidate& c = candidates[id];
                            const VisibleMesh& item = view.Visible[c.MeshCandidate];
                            const Mesh& mesh = *item.Mesh;
                            if (mesh.IsSkinned())
                            {
                                continue;
                            }

                            const std::span<const AssetHandle<MaterialInstance>> materials =
                                mesh.GetMaterials();
                            const SubMesh& subMesh = mesh.GetSubMeshes()[c.SubMeshIndex];
                            if (subMesh.MaterialIndex == SubMesh::NoMaterial ||
                                !materials[subMesh.MaterialIndex].IsLoaded())
                            {
                                continue;
                            }

                            // The candidate list is in GatherMeshes order, so a mesh's submeshes
                            // are contiguous — bind its buffers + MVP once.
                            if (lastBound != &mesh)
                            {
                                cmd.BindVertexBuffer(mesh.GetVertexBuffer());
                                cmd.BindIndexBuffer(mesh.GetIndexBuffer());
                                cmd.PushConstants(
                                    ShadowPushConstants{.MVP = lightViewProj * item.World});
                                lastBound = &mesh;
                            }
                            cmd.DrawIndexed(subMesh.IndexCount, 1, subMesh.IndexOffset, 0, 0);
                        }

                        // Skinned casters: the skinned depth pipeline + the palette set (set 1),
                        // posing each caster's shadow through its DrawData PaletteBase.
                        if (view.SkinningPalette != nullptr && view.SkinnedPaletteBases != nullptr)
                        {
                            cmd.BindPipeline(m_SkinnedPipeline);
                            cmd.BindDescriptorSets(DescriptorSetBindInfo{
                                .Sets = {view.SkinningPalette},
                                .FirstSet = 1,
                                .PipelineBindPoint = PipelineBindPoint::Graphics,
                            });
                            const Mesh* lastSkinned = nullptr;
                            for (const u32 id : m_CullScratch)
                            {
                                const SubMeshCandidate& c = candidates[id];
                                const VisibleMesh& item = view.Visible[c.MeshCandidate];
                                const Mesh& mesh = *item.Mesh;
                                if (!mesh.IsSkinned())
                                {
                                    continue;
                                }

                                const std::span<const AssetHandle<MaterialInstance>> materials =
                                    mesh.GetMaterials();
                                const SubMesh& subMesh = mesh.GetSubMeshes()[c.SubMeshIndex];
                                if (subMesh.MaterialIndex == SubMesh::NoMaterial ||
                                    !materials[subMesh.MaterialIndex].IsLoaded())
                                {
                                    continue;
                                }

                                const auto baseIt =
                                    view.SkinnedPaletteBases->find(PackEntity(item.Owner));
                                if (baseIt == view.SkinnedPaletteBases->end())
                                {
                                    continue;
                                }

                                if (lastSkinned != &mesh)
                                {
                                    cmd.BindVertexBuffer(mesh.GetVertexBuffer());
                                    cmd.BindIndexBuffer(mesh.GetIndexBuffer());
                                    lastSkinned = &mesh;
                                }
                                cmd.PushConstants(ShadowSkinnedPushConstants{
                                    .MVP = lightViewProj * item.World,
                                    .PaletteBase = baseIt->second,
                                });
                                cmd.DrawIndexed(subMesh.IndexCount, 1, subMesh.IndexOffset, 0, 0);
                            }
                        }
                    }
                });
    }
}
