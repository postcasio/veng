#include "PunctualShadowScenePass.h"

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
        // The core pack's depth-only shadow vertex shader (canonical layout in,
        // light-space MVP, no fragment stage).
        constexpr AssetId ShadowDepthVertId{0x156C14C99FFF6B7CULL};

        // The skinned depth-only shadow vertex shader (skinned layout in, palette at set 1,
        // light-space MVP + PaletteBase push) — shared with the directional cascade pass.
        constexpr AssetId ShadowDepthSkinnedVertId{0x83DB748493614120ULL};

        // The atlas's depth format: a single-channel float depth target the lighting
        // pass SampleCmps. The image is renderer-owned; this pass only writes the
        // depth-attachment view per tile.
        constexpr Format PunctualShadowFormat = Format::D32Sfloat;

        // The depth-only pass's vertex push block: the light-space MVP at offset 0,
        // matching the shared push block's leading float4x4.
        struct PunctualShadowPushConstants
        {
            mat4 MVP;
        };

        // The skinned depth-only push block: light-space MVP plus the instance's PaletteBase,
        // matching shadow_depth_skinned.vert's push block.
        struct PunctualSkinnedPushConstants
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

    PunctualShadowScenePass::PunctualShadowScenePass(Context& context, AssetManager& assets,
                                                     u32 resolution)
        : m_Context(context), m_Resolution(resolution)
    {
        const AssetResult<AssetHandle<Veng::Shader>> vs =
            assets.LoadSync<Veng::Shader>(ShadowDepthVertId);
        VE_ASSERT(vs.has_value(), "PunctualShadowScenePass: depth vertex shader load failed: {}",
                  vs.error().Detail);
        m_VertexShader = *vs;

        // Depth-only pipeline: set 0 reserved, one vertex push range for the
        // light-space MVP, no fragment stage, depth write on, no color targets.
        m_Layout = PipelineLayout::Create(
            m_Context,
            {
                .Name = "PunctualShadowScenePass Layout",
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

        // The palette set (set 1): one storage buffer, vertex stage — matching the renderer's
        // palette descriptor set and the skinned shader's reflected set 1.
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

    void PunctualShadowScenePass::Declare(RenderGraph& graph, const PassIO& io)
    {
        const u32 resolution = m_Resolution;

        graph.AddPass("Punctual Shadow Depth")
            .Depth({
                .Resource = io.PunctualShadowMap,
                .Load = LoadOp::Clear,
                .Store = StoreOp::Store,
                // The whole atlas clears to depth = 1; a tile beyond the live
                // record/face count keeps this clear and is never sampled (the
                // lighting pass gates on the record's type/slot).
                .Clear = ClearDepth{.Depth = 1.0f, .Stencil = 0},
            })
            .Execute(
                [this, resolution](PassContext& inner)
                {
                    const ScenePassContext ctx = Wrap(inner);
                    CommandBuffer& cmd = ctx.Cmd();
                    const SceneView& view = ctx.View();
                    const BindlessRegistry& registry = m_Context.GetBindlessRegistry();

                    const std::span<const SubMeshCandidate> candidates =
                        view.Broadphase->GetSubMeshCandidates();

                    // Static caster draw: the canonical-layout depth pipeline, bind buffers + MVP
                    // once per mesh (submeshes are contiguous within a face in GatherMeshes order).
                    const auto DrawStatic = [&](const VisibleMesh& item, u32 subMeshIndex,
                                                const mat4& lightViewProj, const Mesh*& lastBound)
                    {
                        const Mesh& mesh = *item.Mesh;
                        if (mesh.IsSkinned())
                        {
                            return;
                        }
                        const std::span<const AssetHandle<Material>> materials =
                            mesh.GetMaterials();
                        const SubMesh& subMesh = mesh.GetSubMeshes()[subMeshIndex];
                        if (subMesh.MaterialIndex == SubMesh::NoMaterial ||
                            !materials[subMesh.MaterialIndex].IsLoaded())
                        {
                            return;
                        }
                        if (lastBound != &mesh)
                        {
                            cmd.BindVertexBuffer(mesh.GetVertexBuffer());
                            cmd.BindIndexBuffer(mesh.GetIndexBuffer());
                            cmd.PushConstants(
                                PunctualShadowPushConstants{.MVP = lightViewProj * item.World});
                            lastBound = &mesh;
                        }
                        cmd.DrawIndexed(subMesh.IndexCount, 1, subMesh.IndexOffset, 0, 0);
                    };

                    // Skinned caster draw: the skinned depth pipeline; the posed shadow comes from
                    // the per-instance palette (set 1) indexed by the entity's PaletteBase.
                    const auto DrawSkinned = [&](const VisibleMesh& item, u32 subMeshIndex,
                                                 const mat4& lightViewProj, const Mesh*& lastBound)
                    {
                        const Mesh& mesh = *item.Mesh;
                        if (!mesh.IsSkinned())
                        {
                            return;
                        }
                        const std::span<const AssetHandle<Material>> materials =
                            mesh.GetMaterials();
                        const SubMesh& subMesh = mesh.GetSubMeshes()[subMeshIndex];
                        if (subMesh.MaterialIndex == SubMesh::NoMaterial ||
                            !materials[subMesh.MaterialIndex].IsLoaded())
                        {
                            return;
                        }
                        const auto baseIt = view.SkinnedPaletteBases->find(PackEntity(item.Owner));
                        if (baseIt == view.SkinnedPaletteBases->end())
                        {
                            return;
                        }
                        if (lastBound != &mesh)
                        {
                            cmd.BindVertexBuffer(mesh.GetVertexBuffer());
                            cmd.BindIndexBuffer(mesh.GetIndexBuffer());
                            lastBound = &mesh;
                        }
                        cmd.PushConstants(PunctualSkinnedPushConstants{
                            .MVP = lightViewProj * item.World, .PaletteBase = baseIt->second});
                        cmd.DrawIndexed(subMesh.IndexCount, 1, subMesh.IndexOffset, 0, 0);
                    };

                    // Cull against each face's own frustum: off-screen casters within the
                    // light's range/cone are kept; only what falls outside is dropped.
                    const u32 count = view.PunctualShadowCount < MaxShadowedPunctual
                                          ? view.PunctualShadowCount
                                          : MaxShadowedPunctual;
                    for (u32 slot = 0; slot < count; ++slot)
                    {
                        // Params.x encodes the record type: 2 = spot (one face),
                        // 1 = point (six faces), 0 = unused slot (skipped).
                        const f32 type = view.PunctualShadows[slot].Params.x;
                        if (type < 0.5f)
                        {
                            continue;
                        }
                        const u32 faceCount = type > 1.5f ? 1u : CubeFaceCount;

                        for (u32 face = 0; face < faceCount; ++face)
                        {
                            const ivec2 tileOffset{
                                static_cast<i32>(face * resolution),
                                static_cast<i32>(slot * resolution),
                            };
                            cmd.SetViewport(tileOffset, {resolution, resolution});
                            cmd.SetScissor(tileOffset, {resolution, resolution});

                            const mat4 lightViewProj = view.PunctualShadowRawViewProj[slot][face];
                            const Frustum lightFrustum = Frustum::FromViewProjection(lightViewProj);

                            m_CullScratch.clear();
                            if (m_FrustumCull)
                            {
                                view.Broadphase->Cull(lightFrustum, m_CullScratch);
                            }
                            else
                            {
                                for (u32 i = 0; i < candidates.size(); ++i)
                                {
                                    m_CullScratch.push_back(i);
                                }
                            }

                            // Static casters.
                            cmd.BindPipeline(m_Pipeline);
                            registry.Bind(cmd);
                            const Mesh* lastStatic = nullptr;
                            for (const u32 id : m_CullScratch)
                            {
                                const SubMeshCandidate& c = candidates[id];
                                DrawStatic(view.Visible[c.MeshCandidate], c.SubMeshIndex,
                                           lightViewProj, lastStatic);
                            }

                            // Skinned casters.
                            if (view.SkinningPalette != nullptr &&
                                view.SkinnedPaletteBases != nullptr)
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
                                    DrawSkinned(view.Visible[c.MeshCandidate], c.SubMeshIndex,
                                                lightViewProj, lastSkinned);
                                }
                            }
                        }
                    }
                });
    }
}
