#include "DepthNormalPrepassScenePass.h"

#include "../DrawGather.h"

#include <span>

#include <glm/gtc/matrix_inverse.hpp>

#include <Veng/Assert.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/DescriptorSetLayout.h>
#include <Veng/Renderer/GBuffer.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/ShaderInterface.h>
#include <Veng/Renderer/VertexBufferLayout.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/MaterialInstance.h>
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
        // The core pack's normal-passing prepass shaders (auto-mounted core pack).
        constexpr AssetId DepthNormalVertId{0xDFDE03653C23E0C9ULL};
        constexpr AssetId DepthNormalSkinnedVertId{0x1DFC95204289CD78ULL};
        constexpr AssetId DepthNormalFragId{0xB5DBE86AAB243AA7ULL};

        // The one-MRT world-normal target and the depth attachment share the g-buffer's formats:
        // the normal in the same signed-float world-space convention as G1, depth as the sampled
        // reverse-Z D32 the whole engine reads depth as.
        constexpr Format NormalFormat = GBuffer::NormalFormat;
        constexpr Format DepthFormat = GBuffer::DepthFormat;

        // The static push block: camera MVP then the world normal matrix as three column vectors
        // (matching depth_normal_prepass.vert; a second mat4 would overflow the 128-byte push range).
        struct DepthNormalPushConstants
        {
            mat4 MVP;
            vec4 NormalColumn0;
            vec4 NormalColumn1;
            vec4 NormalColumn2;
        };

        // The skinned push block: adds the instance's PaletteBase, matching
        // depth_normal_prepass_skinned.vert's push block.
        struct DepthNormalSkinnedPushConstants
        {
            mat4 MVP;
            vec4 NormalColumn0;
            vec4 NormalColumn1;
            vec4 NormalColumn2;
            u32 PaletteBase;
        };

        // The world normal matrix: the inverse-transpose of the world upper 3x3, so a non-uniform
        // scale still yields a correct world normal. Returned as three column vectors laid out for
        // the push block (the shader reconstructs the transform from them).
        void WorldNormalColumns(const mat4& world, vec4& c0, vec4& c1, vec4& c2)
        {
            const mat3 normalMatrix = glm::inverseTranspose(mat3(world));
            c0 = vec4(normalMatrix[0], 0.0f);
            c1 = vec4(normalMatrix[1], 0.0f);
            c2 = vec4(normalMatrix[2], 0.0f);
        }

        // Whether a submesh's shape is drawn into the prepass. The lean render wants the silhouette,
        // not the shading, so it draws any opaque geometry — a submesh with no material (a raw
        // preview mesh) as readily as one with a resident opaque material; only a resident
        // Translucent submesh is skipped (it writes no opaque depth, as in the shadow caster).
        bool DrawsInPrepass(const std::span<const AssetHandle<MaterialInstance>> materials,
                            const Mesh& mesh, const u32 subMeshIndex)
        {
            const SubMesh& subMesh = mesh.GetSubMeshes()[subMeshIndex];
            if (subMesh.MaterialIndex == SubMesh::NoMaterial ||
                !materials[subMesh.MaterialIndex].IsLoaded())
            {
                return true;
            }
            return materials[subMesh.MaterialIndex].Get()->GetDomain() !=
                   MaterialDomain::Translucent;
        }
    }

    DepthNormalPrepassScenePass::DepthNormalPrepassScenePass(Context& context, AssetManager& assets,
                                                             uvec2 extent, ResourceId normalId,
                                                             ResourceId depthId)
        : m_Context(context), m_Extent(extent), m_NormalId(normalId), m_DepthId(depthId)
    {
        const AssetResult<AssetHandle<Veng::Shader>> vs =
            assets.LoadSync<Veng::Shader>(DepthNormalVertId);
        VE_ASSERT(vs.has_value(), "DepthNormalPrepassScenePass: vertex shader load failed: {}",
                  vs.error().Detail);
        m_VertexShader = *vs;

        const AssetResult<AssetHandle<Veng::Shader>> fs =
            assets.LoadSync<Veng::Shader>(DepthNormalFragId);
        VE_ASSERT(fs.has_value(), "DepthNormalPrepassScenePass: fragment shader load failed: {}",
                  fs.error().Detail);
        m_FragmentShader = *fs;

        // Set 0 reserved for the bindless registry; one vertex push range for the camera MVP + world
        // normal matrix. One color target (the world normal) plus depth.
        m_Layout = PipelineLayout::Create(
            m_Context, {
                           .Name = "DepthNormalPrepass Layout",
                           .PushConstantRanges = {PushConstantRange::Of<DepthNormalPushConstants>(
                               ShaderStage::Vertex)},
                       });

        optional<VertexBufferLayout> vertexBufferLayout;
        const Renderer::ShaderInterface& vsInterface = m_VertexShader.Get()->Interface;
        if (vsInterface.VertexLayoutId.has_value())
        {
            const AssetResult<AssetHandle<Veng::VertexLayout>> layoutResult =
                assets.LoadSync<Veng::VertexLayout>(*vsInterface.VertexLayoutId);
            VE_ASSERT(layoutResult.has_value(),
                      "DepthNormalPrepassScenePass: vertex layout load failed: {}",
                      layoutResult.error().Detail);
            vertexBufferLayout = layoutResult->Get()->GetLayout();
        }

        m_Pipeline = GraphicsPipeline::Create(
            m_Context,
            {
                .Name = "DepthNormalPrepass Pipeline",
                .ColorAttachments = {{.Format = NormalFormat}},
                .DepthAttachmentFormat = DepthFormat,
                .VertexBufferLayout = vertexBufferLayout,
                .PipelineLayout = m_Layout,
                .ShaderStages =
                    {
                        {.Stage = ShaderStage::Vertex, .Module = m_VertexShader.Get()->Module},
                        {.Stage = ShaderStage::Fragment, .Module = m_FragmentShader.Get()->Module},
                    },
                // A camera view culls back faces, unlike the shadow caster which culls front faces.
                .CullMode = CullMode::Back,
                .DepthTestEnable = true,
                .DepthWriteEnable = true,
            });

        BuildSkinnedPipeline(assets);
    }

    void DepthNormalPrepassScenePass::BuildSkinnedPipeline(AssetManager& assets)
    {
        const AssetResult<AssetHandle<Veng::Shader>> vs =
            assets.LoadSync<Veng::Shader>(DepthNormalSkinnedVertId);
        VE_ASSERT(vs.has_value(),
                  "DepthNormalPrepassScenePass: skinned vertex shader load failed: {}",
                  vs.error().Detail);
        m_SkinnedVertexShader = *vs;

        // The palette set (set 3): one storage buffer, vertex stage — matching the renderer's
        // palette descriptor set and the skinned shader's reflected set 3.
        m_PaletteSetLayout = DescriptorSetLayout::Create(
            m_Context, {
                           .Name = "DepthNormalPrepass Palette Set Layout",
                           .Bindings = {{.Binding = 0,
                                         .Type = DescriptorType::StorageBuffer,
                                         .Count = 1,
                                         .Stages = ShaderStage::Vertex}},
                       });

        m_SkinnedLayout = PipelineLayout::Create(
            m_Context,
            {
                .Name = "DepthNormalPrepass Skinned Layout",
                .DescriptorSetLayouts = {m_PaletteSetLayout},
                .PushConstantRanges = {PushConstantRange::Of<DepthNormalSkinnedPushConstants>(
                    ShaderStage::Vertex)},
            });

        optional<VertexBufferLayout> skinnedLayout;
        const Renderer::ShaderInterface& vsInterface = m_SkinnedVertexShader.Get()->Interface;
        if (vsInterface.VertexLayoutId.has_value())
        {
            const AssetResult<AssetHandle<Veng::VertexLayout>> layoutResult =
                assets.LoadSync<Veng::VertexLayout>(*vsInterface.VertexLayoutId);
            VE_ASSERT(layoutResult.has_value(),
                      "DepthNormalPrepassScenePass: skinned vertex layout load failed: {}",
                      layoutResult.error().Detail);
            skinnedLayout = layoutResult->Get()->GetLayout();
        }

        m_SkinnedPipeline = GraphicsPipeline::Create(
            m_Context,
            {
                .Name = "DepthNormalPrepass Skinned Pipeline",
                .ColorAttachments = {{.Format = NormalFormat}},
                .DepthAttachmentFormat = DepthFormat,
                .VertexBufferLayout = skinnedLayout,
                .PipelineLayout = m_SkinnedLayout,
                .ShaderStages =
                    {
                        {.Stage = ShaderStage::Vertex,
                         .Module = m_SkinnedVertexShader.Get()->Module},
                        {.Stage = ShaderStage::Fragment, .Module = m_FragmentShader.Get()->Module},
                    },
                .CullMode = CullMode::Back,
                .DepthTestEnable = true,
                .DepthWriteEnable = true,
            });
    }

    DepthNormalPrepassScenePass::~DepthNormalPrepassScenePass() = default;

    void DepthNormalPrepassScenePass::Configure(const SceneRendererSettings& settings)
    {
        m_FrustumCull = settings.FrustumCull;
    }

    void DepthNormalPrepassScenePass::Declare(RenderGraph& graph, const PassIO& /*io*/)
    {
        graph.AddPass("Depth+Normal Prepass")
            .Color({
                .Resource = m_NormalId,
                .Load = LoadOp::Clear,
                .Store = StoreOp::Store,
                // Zero world normal for a background texel — the consumer reads it as "no surface".
                .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 0.0f},
            })
            .Depth({
                .Resource = m_DepthId,
                .Load = LoadOp::Clear,
                .Store = StoreOp::Store,
                // Reverse-Z: the far plane is 0, so an un-drawn pixel clears to 0.
                .Clear = ClearDepth{.Depth = 0.0f, .Stencil = 0},
            })
            .Execute(
                [this](PassContext& inner)
                {
                    const ScenePassContext ctx = Wrap(inner);
                    CommandBuffer& cmd = ctx.Cmd();
                    const SceneView& view = ctx.View();
                    const BindlessRegistry& registry = m_Context.GetBindlessRegistry();

                    const uvec2 renderExtent = view.RenderExtent;
                    cmd.SetViewport({0, 0}, renderExtent);
                    cmd.SetScissor({0, 0}, renderExtent);

                    const mat4 viewProj = view.Camera.ViewProjection();
                    const std::span<const SubMeshCandidate> candidates =
                        view.Broadphase->GetSubMeshCandidates();

                    m_CullScratch.clear();
                    if (m_FrustumCull)
                    {
                        const Frustum cameraFrustum = Frustum::FromViewProjection(viewProj);
                        view.Broadphase->Cull(cameraFrustum, m_CullScratch);
                    }
                    else
                    {
                        for (u32 i = 0; i < candidates.size(); ++i)
                        {
                            m_CullScratch.push_back(i);
                        }
                    }

                    // Static opaque submeshes: the canonical normal-passing pipeline.
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
                        // Draw a resident, non-Translucent submesh — the opaque-geometry test the
                        // shadow caster uses; a Translucent submesh writes no depth here either.
                        if (!DrawsInPrepass(item.Materials, mesh, c.SubMeshIndex))
                        {
                            continue;
                        }
                        const SubMesh& subMesh = mesh.GetSubMeshes()[c.SubMeshIndex];

                        // The candidate list is in GatherMeshes order, so a mesh's submeshes are
                        // contiguous — bind its buffers + push once per mesh.
                        if (lastBound != &mesh)
                        {
                            cmd.BindVertexBuffer(mesh.GetVertexBuffer());
                            cmd.BindIndexBuffer(mesh.GetIndexBuffer());
                            DepthNormalPushConstants push{.MVP = viewProj * item.World};
                            WorldNormalColumns(item.World, push.NormalColumn0, push.NormalColumn1,
                                               push.NormalColumn2);
                            cmd.PushConstants(push);
                            lastBound = &mesh;
                        }
                        cmd.DrawIndexed(subMesh.IndexCount, 1, subMesh.IndexOffset, 0, 0);
                    }

                    // Skinned opaque submeshes: the skinned normal-passing pipeline + the palette set
                    // (set 3), posing each mesh through its DrawData PaletteBase.
                    if (view.SkinningPalette != nullptr && view.SkinnedPaletteBases != nullptr)
                    {
                        cmd.BindPipeline(m_SkinnedPipeline);
                        cmd.BindDescriptorSets(DescriptorSetBindInfo{
                            .Sets = {view.SkinningPalette},
                            .FirstSet = 3,
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
                            if (!DrawsInPrepass(item.Materials, mesh, c.SubMeshIndex))
                            {
                                continue;
                            }
                            const SubMesh& subMesh = mesh.GetSubMeshes()[c.SubMeshIndex];

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
                            DepthNormalSkinnedPushConstants push{.MVP = viewProj * item.World};
                            WorldNormalColumns(item.World, push.NormalColumn0, push.NormalColumn1,
                                               push.NormalColumn2);
                            push.PaletteBase = baseIt->second;
                            cmd.PushConstants(push);
                            cmd.DrawIndexed(subMesh.IndexCount, 1, subMesh.IndexOffset, 0, 0);
                        }
                    }
                });
    }
}
