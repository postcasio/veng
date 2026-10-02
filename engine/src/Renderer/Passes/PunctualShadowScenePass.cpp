#include "PunctualShadowScenePass.h"

#include "../DrawGather.h"

#include <span>

#include <Veng/Assert.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/DescriptorSetLayout.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/PunctualShadows.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Renderer/ShaderInterface.h>
#include <Veng/Renderer/VertexBufferLayout.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Asset/VertexLayout.h>

#include <Veng/Math/Frustum.h>

#include <Veng/Scene/Entity.h>
#include <Veng/Scene/Visibility.h>

namespace Veng::Renderer
{
    namespace
    {
        // The core pack's depth-only shadow vertex shader (canonical layout plus the instance-rate
        // caster-record index in, light-space view-projection pushed, no fragment stage).
        constexpr AssetId ShadowDepthVertId{0x156C14C99FFF6B7CULL};

        // The skinned depth-only shadow vertex shader (skinned layout in, palette at set 1,
        // light-space MVP + PaletteBase push) — shared with the directional cascade pass.
        constexpr AssetId ShadowDepthSkinnedVertId{0x83DB748493614120ULL};

        // The atlas's depth format: a single-channel float depth target the lighting
        // pass SampleCmps. The image is renderer-owned; this pass only writes the
        // depth-attachment view per tile.
        constexpr Format PunctualShadowFormat = Format::D32Sfloat;

        // The instanced depth-only vertex push block: the view's light-space view-projection,
        // matching shadow_depth.vert's push block. Each instance's world rides its caster record.
        struct PunctualShadowPushConstants
        {
            mat4 ViewProj;
        };

        // The skinned depth-only push block: light-space MVP plus the instance's PaletteBase,
        // matching shadow_depth_skinned.vert's push block.
        struct PunctualSkinnedPushConstants
        {
            mat4 MVP;
            u32 PaletteBase;
        };
    }

    PunctualShadowScenePass::PunctualShadowScenePass(Context& context, AssetManager& assets,
                                                     const CasterRecordRing& records,
                                                     u32 resolution)
        : m_Context(context), m_Records(records), m_Resolution(resolution),
          m_Batch(context, "Punctual Shadow Depth", context.GetMaxFramesInFlight())
    {
        const AssetResult<AssetHandle<Veng::Shader>> vs =
            assets.LoadSync<Veng::Shader>(ShadowDepthVertId);
        VE_ASSERT(vs.has_value(), "PunctualShadowScenePass: depth vertex shader load failed: {}",
                  vs.error().Detail);
        m_VertexShader = *vs;

        // Depth-only pipeline: the caster records at the first author set, one vertex push range
        // for the light-space view-projection, no fragment stage, depth write on, no color targets.
        m_Layout = PipelineLayout::Create(
            m_Context,
            {
                .Name = "PunctualShadowScenePass Layout",
                .DescriptorSetLayouts = {m_Records.GetSetLayout()},
                .PushConstantRanges = {PushConstantRange::Of<PunctualShadowPushConstants>(
                    ShaderStage::Vertex)},
            });

        optional<VertexBufferLayout> vertexBufferLayout;
        const Renderer::ShaderInterface& vsInterface = m_VertexShader.Get()->Interface;
        if (vsInterface.VertexLayoutId.has_value())
        {
            const AssetResult<AssetHandle<Veng::VertexLayout>> layoutResult =
                assets.LoadSync<Veng::VertexLayout>(*vsInterface.VertexLayoutId);
            VE_ASSERT(layoutResult.has_value(),
                      "PunctualShadowScenePass: vertex layout load failed: {}",
                      layoutResult.error().Detail);
            vertexBufferLayout = layoutResult->Get()->GetLayout();
        }

        m_Pipeline = GraphicsPipeline::Create(
            m_Context,
            {
                .Name = "PunctualShadowScenePass Depth Pipeline",
                .ColorAttachments = {},
                .DepthAttachmentFormat = PunctualShadowFormat,
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
            });

        BuildSkinnedPipeline(assets);
    }

    void PunctualShadowScenePass::BuildSkinnedPipeline(AssetManager& assets)
    {
        const AssetResult<AssetHandle<Veng::Shader>> vs =
            assets.LoadSync<Veng::Shader>(ShadowDepthSkinnedVertId);
        VE_ASSERT(vs.has_value(),
                  "PunctualShadowScenePass: skinned depth vertex shader load failed: {}",
                  vs.error().Detail);
        m_SkinnedVertexShader = *vs;

        // The palette set (set 3): one storage buffer, vertex stage — matching the renderer's
        // palette descriptor set and the skinned shader's reflected set 3.
        m_PaletteSetLayout = DescriptorSetLayout::Create(
            m_Context, {
                           .Name = "PunctualShadowScenePass Palette Set Layout",
                           .Bindings = {{.Binding = 0,
                                         .Type = DescriptorType::StorageBuffer,
                                         .Count = 1,
                                         .Stages = ShaderStage::Vertex}},
                       });

        m_SkinnedLayout = PipelineLayout::Create(
            m_Context,
            {
                .Name = "PunctualShadowScenePass Skinned Layout",
                .DescriptorSetLayouts = {m_PaletteSetLayout},
                .PushConstantRanges = {PushConstantRange::Of<PunctualSkinnedPushConstants>(
                    ShaderStage::Vertex)},
            });

        optional<VertexBufferLayout> skinnedLayout;
        const Renderer::ShaderInterface& vsInterface = m_SkinnedVertexShader.Get()->Interface;
        if (vsInterface.VertexLayoutId.has_value())
        {
            const AssetResult<AssetHandle<Veng::VertexLayout>> layoutResult =
                assets.LoadSync<Veng::VertexLayout>(*vsInterface.VertexLayoutId);
            VE_ASSERT(layoutResult.has_value(),
                      "PunctualShadowScenePass: skinned vertex layout load failed: {}",
                      layoutResult.error().Detail);
            skinnedLayout = layoutResult->Get()->GetLayout();
        }

        m_SkinnedPipeline = GraphicsPipeline::Create(
            m_Context, {
                           .Name = "PunctualShadowScenePass Skinned Depth Pipeline",
                           .ColorAttachments = {},
                           .DepthAttachmentFormat = PunctualShadowFormat,
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

    PunctualShadowScenePass::~PunctualShadowScenePass() = default;

    void PunctualShadowScenePass::Configure(const SceneRendererSettings& settings)
    {
        m_FrustumCull = settings.FrustumCull;
    }

    void PunctualShadowScenePass::BuildViews(const SceneView& view)
    {
        const std::span<const SubMeshCandidate> candidates =
            view.Broadphase->GetSubMeshCandidates();
        const bool skinnedPosed =
            view.SkinningPalette != nullptr && view.SkinnedPaletteBases != nullptr;

        m_Views.clear();
        m_Batch.Begin(candidates);
        m_Skinned.clear();
        m_SkinnedViewEnds.clear();

        const u32 count = view.PunctualShadowCount < MaxShadowedPunctual ? view.PunctualShadowCount
                                                                         : MaxShadowedPunctual;
        for (u32 slot = 0; slot < count; ++slot)
        {
            // Params.x encodes the record type: 2 = spot (one face), 1 = point (six faces),
            // 0 = unused slot (skipped).
            const f32 type = view.PunctualShadows[slot].Params.x;
            if (type < 0.5f)
            {
                continue;
            }
            const u32 faceCount = type > 1.5f ? 1u : CubeFaceCount;

            for (u32 face = 0; face < faceCount; ++face)
            {
                if ((view.PunctualShadowFaceMask[slot] & (1u << face)) == 0)
                {
                    continue;
                }
                m_Views.push_back(ShadowView{.Slot = slot, .Face = face});

                // Cull against the face's own frustum: off-screen casters within the light's
                // range/cone are kept; only what falls outside is dropped.
                m_CullScratch.clear();
                if (m_FrustumCull)
                {
                    view.Broadphase->Cull(
                        Frustum::FromViewProjection(view.PunctualShadowRawViewProj[slot][face]),
                        m_CullScratch);
                }
                else
                {
                    for (u32 i = 0; i < candidates.size(); ++i)
                    {
                        m_CullScratch.push_back(i);
                    }
                }

                for (const u32 id : m_CullScratch)
                {
                    const SubMeshCandidate& c = candidates[id];
                    const VisibleMesh& item = view.Visible[c.MeshCandidate];
                    if (!item.CastsShadows)
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
                m_SkinnedViewEnds.push_back(static_cast<u32>(m_Skinned.size()));
            }
        }
    }

    void PunctualShadowScenePass::Declare(RenderGraph& graph, const PassIO& io)
    {
        const u32 resolution = m_Resolution;

        graph.AddPass("Punctual Shadow Depth")
            .Depth({
                .Resource = io.PunctualShadowMap,
                .Load = LoadOp::Clear,
                .Store = StoreOp::Store,
                // The whole atlas clears to depth = 0 (reverse-Z far); a tile beyond the
                // live record/face count keeps this clear and is never sampled (the
                // lighting pass gates on the record's type/slot).
                .Clear = ClearDepth{.Depth = 0.0f, .Stencil = 0},
            })
            .Execute(
                [this, resolution](PassContext& inner)
                {
                    const ScenePassContext ctx = Wrap(inner);
                    CommandBuffer& cmd = ctx.Cmd();
                    const SceneView& view = ctx.View();
                    const BindlessRegistry& registry = m_Context.GetBindlessRegistry();

                    BuildViews(view);
                    if (m_Views.empty())
                    {
                        return;
                    }
                    const u32 frameIndex = m_Context.GetCurrentFrameInFlight();
                    m_Batch.Upload(frameIndex);

                    // Slot s, face f renders into the tile at (column f, row s).
                    const auto SetTile = [&](const ShadowView& shadowView)
                    {
                        const ivec2 tileOffset{
                            static_cast<i32>(shadowView.Face * resolution),
                            static_cast<i32>(shadowView.Slot * resolution),
                        };
                        cmd.SetViewport(tileOffset, {resolution, resolution});
                        cmd.SetScissor(tileOffset, {resolution, resolution});
                        return view.PunctualShadowRawViewProj[shadowView.Slot][shadowView.Face];
                    };

                    // Static casters: one instanced draw per submesh per view, the view's raw
                    // light-space matrix pushed once.
                    if (!m_Batch.IsEmpty())
                    {
                        cmd.BindPipeline(m_Pipeline);
                        registry.Bind(cmd);
                        cmd.BindDescriptorSets({&m_Records.GetSet(frameIndex)},
                                               BindlessRegistry::FirstUserSet);
                        m_Batch.BindInstanceIds(cmd);
                        for (u32 v = 0; v < m_Views.size(); ++v)
                        {
                            if (m_Batch.GetViewDrawCount(v) == 0)
                            {
                                continue;
                            }
                            cmd.PushConstants(
                                PunctualShadowPushConstants{.ViewProj = SetTile(m_Views[v])});
                            m_Batch.RecordView(cmd, v);
                        }
                    }

                    // Skinned casters: the skinned depth pipeline; the posed shadow comes from the
                    // per-instance palette indexed by the entity's PaletteBase.
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
                    for (u32 v = 0; v < m_Views.size(); ++v)
                    {
                        const u32 end = m_SkinnedViewEnds[v];
                        if (begin == end)
                        {
                            continue;
                        }
                        const mat4 lightViewProj = SetTile(m_Views[v]);
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
                            cmd.PushConstants(PunctualSkinnedPushConstants{
                                .MVP = lightViewProj * item.World, .PaletteBase = *paletteBase});
                            cmd.DrawIndexed(subMesh.IndexCount, 1, subMesh.IndexOffset, 0, 0);
                        }
                        begin = end;
                    }
                });
    }
}
