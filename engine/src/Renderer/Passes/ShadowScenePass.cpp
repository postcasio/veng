#include "ShadowScenePass.h"

#include "../DrawGather.h"

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
        // The core pack's depth-only shadow vertex shader (canonical layout plus the instance-rate
        // caster-record index in, light-space view-projection pushed, no fragment stage).
        constexpr AssetId ShadowDepthVertId{0x156C14C99FFF6B7CULL};

        // The skinned depth-only shadow vertex shader (skinned layout in, palette at set 1,
        // light-space MVP + PaletteBase push).
        constexpr AssetId ShadowDepthSkinnedVertId{0x83DB748493614120ULL};

        // Depth format and usage: a single-channel float depth target written per cascade
        // and sampled by the lighting pass through its dedicated set.
        constexpr Format ShadowFormat = Format::D32Sfloat;
        constexpr ImageUsage ShadowUsage = ImageUsage::DepthAttachment | ImageUsage::Sampled;

        // The instanced depth-only vertex push block: the tile's light-space view-projection,
        // matching shadow_depth.vert's push block. Each instance's world rides its caster record.
        struct ShadowPushConstants
        {
            mat4 ViewProj;
        };

        // The skinned depth-only push block: light-space MVP plus the instance's PaletteBase,
        // matching shadow_depth_skinned.vert's push block.
        struct ShadowSkinnedPushConstants
        {
            mat4 MVP;
            u32 PaletteBase;
        };
    }

    ShadowScenePass::ShadowScenePass(Context& context, AssetManager& assets,
                                     const CasterRecordRing& records, u32 resolution,
                                     u32 cascadeCount)
        : m_Context(context), m_Records(records), m_Resolution(resolution),
          m_CascadeCount(cascadeCount),
          m_Grid(ComputeShadowAtlasGrid(cascadeCount, MaxCascadeSets)),
          m_Batch(context, "Shadow Depth", context.GetMaxFramesInFlight())
    {
        const AssetResult<AssetHandle<Veng::Shader>> vs =
            assets.LoadSync<Veng::Shader>(ShadowDepthVertId);
        VE_ASSERT(vs.has_value(), "ShadowScenePass: depth vertex shader load failed: {}",
                  vs.error().Detail);
        m_VertexShader = *vs;

        // Depth-only pipeline: the caster records at the first author set, one vertex push range
        // for the light-space view-projection, no fragment stage, depth write on, no color targets.
        m_Layout = PipelineLayout::Create(
            m_Context, {
                           .Name = "ShadowScenePass Layout",
                           .DescriptorSetLayouts = {m_Records.GetSetLayout()},
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
                .InstanceCandidateId = true,
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
                // Pancake casters nearer than the cascade's tight near plane onto it; must
                // match ComputeCascades' PancakeNear so the cull-only near extension is
                // rasterizable. False on both sides when the device lacks depthClamp.
                .DepthClampEnable = m_Context.IsDepthClampSupported(),
            });

        BuildSkinnedPipeline(assets);

        CreateAtlas();
    }

    void ShadowScenePass::BuildSkinnedPipeline(AssetManager& assets)
    {
        const AssetResult<AssetHandle<Veng::Shader>> vs =
            assets.LoadSync<Veng::Shader>(ShadowDepthSkinnedVertId);
        VE_ASSERT(vs.has_value(), "ShadowScenePass: skinned depth vertex shader load failed: {}",
                  vs.error().Detail);
        m_SkinnedVertexShader = *vs;

        // The palette set (set 3): one storage buffer, vertex stage — matching the renderer's
        // palette descriptor set and the skinned shader's reflected set 3.
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
                           .DepthClampEnable = m_Context.IsDepthClampSupported(),
                       });
    }

    ShadowScenePass::~ShadowScenePass() = default;

    void ShadowScenePass::CreateAtlas()
    {
        // Recreate the depth atlas at the current resolution × tile grid, which always spans
        // the full cascade-set budget so a second directional needs no reallocation.
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
        m_MinCasterTexels = settings.ShadowCasterMinTexels;
    }

    void ShadowScenePass::BuildTiles(const SceneView& view, const u32 tileCount,
                                     const u32 cascadeCount)
    {
        const std::span<const SubMeshCandidate> candidates =
            view.Broadphase->GetSubMeshCandidates();
        const bool skinnedPosed =
            view.SkinningPalette != nullptr && view.SkinnedPaletteBases != nullptr;

        m_Batch.Begin(candidates);
        m_Skinned.clear();
        m_SkinnedTileEnds.clear();
        for (u32 tile = 0; tile < tileCount; ++tile)
        {
            const u32 set = tile / cascadeCount;
            const u32 k = tile % cascadeCount;

            // Cull against the near-extended cull matrix, not the render matrix: an off-screen
            // caster between the light and the slice must survive the cull so the depth-clamped
            // rasterization can pancake it onto the render matrix's tight near plane.
            m_CullScratch.clear();
            if (m_FrustumCull)
            {
                view.Broadphase->Cull(Frustum::FromViewProjection(view.CascadeCullViewProj[set][k]),
                                      m_CullScratch);
            }
            else
            {
                for (u32 i = 0; i < candidates.size(); ++i)
                {
                    m_CullScratch.push_back(i);
                }
            }

            // The cascade is orthographic, so one scale maps a world length to tile texels: the
            // clip x row's length is 2 / the tile's world width.
            const mat4& viewProj = view.CascadeViewProj[set][k];
            const f32 texelsPerUnit =
                glm::length(vec3(viewProj[0][0], viewProj[1][0], viewProj[2][0])) * 0.5f *
                static_cast<f32>(m_Resolution);

            for (const u32 id : m_CullScratch)
            {
                const SubMeshCandidate& c = candidates[id];
                const VisibleMesh& item = view.Visible[c.MeshCandidate];
                if (!item.CastsShadows)
                {
                    continue;
                }
                if (glm::length(item.WorldBounds.Size()) * texelsPerUnit < m_MinCasterTexels)
                {
                    continue;
                }
                const Mesh& mesh = *item.Mesh;
                if (!CastsShadow(item.Materials, mesh, c.SubMeshIndex))
                {
                    continue;
                }
                if (!mesh.IsSkinned())
                {
                    m_Batch.Add(id, mesh, c.SubMeshIndex);
                }
                else if (skinnedPosed)
                {
                    m_Skinned.push_back(id);
                }
            }
            m_Batch.EndView();
            m_SkinnedTileEnds.push_back(static_cast<u32>(m_Skinned.size()));
        }
    }

    void ShadowScenePass::Declare(RenderGraph& graph, const PassIO& io)
    {
        const u32 resolution = m_Resolution;
        const ShadowAtlasGrid grid = m_Grid;
        const u32 cascadeCount = m_CascadeCount;

        graph.AddPass("Shadow Depth")
            .Depth({
                .Resource = io.ShadowMap,
                .Load = LoadOp::Clear,
                .Store = StoreOp::Store,
                // The whole atlas clears to depth = 0 (reverse-Z far); an unused tile (the
                // fourth cell at three cascades, or a whole band whose set no light took)
                // keeps this clear and is never selected.
                .Clear = ClearDepth{.Depth = 0.0f, .Stencil = 0},
            })
            .Execute(
                [this, resolution, grid, cascadeCount](PassContext& inner)
                {
                    const ScenePassContext ctx = Wrap(inner);
                    CommandBuffer& cmd = ctx.Cmd();
                    const SceneView& view = ctx.View();
                    const BindlessRegistry& registry = m_Context.GetBindlessRegistry();

                    // Every granted set's cascades, each into its tile. Sets stack as row bands,
                    // so a set costs a full re-traversal of the casters — which is what bounds
                    // MaxCascadeSets. A frame no light took a set from renders none.
                    const u32 count =
                        cascadeCount < view.CascadeCount ? cascadeCount : view.CascadeCount;
                    const u32 sets =
                        grid.Sets < view.CascadeSetCount ? grid.Sets : view.CascadeSetCount;
                    const u32 tileCount = sets * count;
                    if (tileCount == 0)
                    {
                        return;
                    }

                    BuildTiles(view, tileCount, count);
                    const u32 frameIndex = m_Context.GetCurrentFrameInFlight();
                    m_Batch.Upload(frameIndex);

                    const auto SetTile = [&](const u32 tile)
                    {
                        const u32 set = tile / count;
                        const u32 k = tile % count;
                        const ivec2 tileOffset{
                            static_cast<i32>((k % grid.Columns) * resolution),
                            static_cast<i32>((set * grid.Rows + k / grid.Columns) * resolution),
                        };
                        cmd.SetViewport(tileOffset, {resolution, resolution});
                        cmd.SetScissor(tileOffset, {resolution, resolution});
                        return view.CascadeViewProj[set][k];
                    };

                    // Static casters: one instanced draw per submesh per tile, the tile's raw
                    // light-space matrix pushed once.
                    if (!m_Batch.IsEmpty())
                    {
                        cmd.BindPipeline(m_Pipeline);
                        registry.Bind(cmd);
                        cmd.BindDescriptorSets({&m_Records.GetSet(frameIndex)},
                                               BindlessRegistry::FirstUserSet);
                        m_Batch.BindInstanceIds(cmd);
                        for (u32 tile = 0; tile < tileCount; ++tile)
                        {
                            if (m_Batch.GetViewDrawCount(tile) == 0)
                            {
                                continue;
                            }
                            cmd.PushConstants(ShadowPushConstants{.ViewProj = SetTile(tile)});
                            m_Batch.RecordView(cmd, tile);
                        }
                    }

                    // Skinned casters: the skinned depth pipeline + the palette set, each posed
                    // through its entity's palette base.
                    if (m_Skinned.empty())
                    {
                        return;
                    }
                    const std::span<const SubMeshCandidate> candidates =
                        view.Broadphase->GetSubMeshCandidates();
                    cmd.BindPipeline(m_SkinnedPipeline);
                    cmd.BindDescriptorSets({view.SkinningPalette.get()},
                                           BindlessRegistry::FirstUserSet);
                    u32 begin = 0;
                    for (u32 tile = 0; tile < tileCount; ++tile)
                    {
                        const u32 end = m_SkinnedTileEnds[tile];
                        if (begin == end)
                        {
                            continue;
                        }
                        const mat4 lightViewProj = SetTile(tile);
                        const Mesh* lastSkinned = nullptr;
                        for (u32 i = begin; i < end; ++i)
                        {
                            const SubMeshCandidate& c = candidates[m_Skinned[i]];
                            const VisibleMesh& item = view.Visible[c.MeshCandidate];
                            const u32* paletteBase = view.SkinnedPaletteBases->Find(item.Owner);
                            if (paletteBase == nullptr)
                            {
                                continue;
                            }
                            const Mesh& mesh = *item.Mesh;
                            if (lastSkinned != &mesh)
                            {
                                cmd.BindVertexBuffer(mesh.GetVertexBuffer());
                                cmd.BindIndexBuffer(mesh.GetIndexBuffer());
                                lastSkinned = &mesh;
                            }
                            const SubMesh& subMesh = mesh.GetSubMeshes()[c.SubMeshIndex];
                            cmd.PushConstants(ShadowSkinnedPushConstants{
                                .MVP = lightViewProj * item.World,
                                .PaletteBase = *paletteBase,
                            });
                            cmd.DrawIndexed(subMesh.IndexCount, 1, subMesh.IndexOffset, 0, 0);
                        }
                        begin = end;
                    }
                });
    }
}
