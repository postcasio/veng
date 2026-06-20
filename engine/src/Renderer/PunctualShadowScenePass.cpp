#include "PunctualShadowScenePass.h"

#include <span>

#include <Veng/Assert.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
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

#include <Veng/Scene/Visibility.h>

namespace Veng::Renderer
{
    namespace
    {
        // The core pack's depth-only shadow vertex shader (canonical layout in,
        // light-space MVP, no fragment stage).
        constexpr AssetId ShadowDepthVertId{0x156C14C99FFF6B7CULL};

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

                    cmd.BindPipeline(m_Pipeline);
                    registry.Bind(cmd);

                    const Mesh* lastBound = nullptr;

                    const auto DrawSubMesh =
                        [&](const VisibleMesh& item, u32 subMeshIndex, const mat4& lightViewProj)
                    {
                        const Mesh& mesh = *item.Mesh;
                        const std::span<const AssetHandle<Material>> materials =
                            mesh.GetMaterials();

                        const SubMesh& subMesh = mesh.GetSubMeshes()[subMeshIndex];
                        if (subMesh.MaterialIndex == SubMesh::NoMaterial)
                        {
                            return;
                        }
                        if (!materials[subMesh.MaterialIndex].IsLoaded())
                        {
                            return;
                        }

                        // The candidate list is in GatherMeshes order, so a mesh's submeshes
                        // are contiguous within a face — bind its buffers + MVP once.
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

                    const std::span<const SubMeshCandidate> candidates =
                        view.Broadphase->GetSubMeshCandidates();

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

                            // Each face is a fresh draw set with its own MVP; reset the
                            // redundant-bind tracker so the first mesh rebinds + repushes.
                            lastBound = nullptr;

                            if (m_FrustumCull)
                            {
                                m_CullScratch.clear();
                                view.Broadphase->Cull(lightFrustum, m_CullScratch);
                                for (const u32 id : m_CullScratch)
                                {
                                    const SubMeshCandidate& c = candidates[id];
                                    DrawSubMesh(view.Visible[c.MeshCandidate], c.SubMeshIndex,
                                                lightViewProj);
                                }
                            }
                            else
                            {
                                for (const SubMeshCandidate& c : candidates)
                                {
                                    DrawSubMesh(view.Visible[c.MeshCandidate], c.SubMeshIndex,
                                                lightViewProj);
                                }
                            }
                        }
                    }
                });
    }
}
