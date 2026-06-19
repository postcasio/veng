#include "ShadowScenePass.h"

#include <span>

#include <fmt/format.h>

#include <Veng/Assert.h>
#include <Veng/Renderer/BindlessRegistry.h>
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

#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/Transforms.h>

namespace Veng::Renderer
{
    namespace
    {
        // The core pack's depth-only shadow vertex shader (canonical layout in,
        // light-space MVP, no fragment stage).
        constexpr AssetId ShadowDepthVertId{0x156C14C99FFF6B7CULL};

        // The shadow map's depth format and usage: a single-channel float depth
        // target that is both a depth attachment (written by the depth-only pass)
        // and sampled by the lighting pass.
        constexpr Format ShadowFormat = Format::D32Sfloat;
        constexpr ImageUsage ShadowUsage = ImageUsage::DepthAttachment | ImageUsage::Sampled;

        // The depth-only pass's vertex push block: the light-space MVP at offset 0,
        // matching the shared push block's leading float4x4. The depth-only pipeline
        // declares a vertex-stage range over just these 64 bytes.
        struct ShadowPushConstants
        {
            mat4 MVP;
        };
    }

    ShadowScenePass::ShadowScenePass(Context& context, AssetManager& assets, u32 resolution)
        : m_Context(context), m_Resolution(resolution)
    {
        const AssetResult<AssetHandle<Veng::Shader>> vs = assets.LoadSync<Veng::Shader>(ShadowDepthVertId);
        VE_ASSERT(vs.has_value(), "ShadowScenePass: depth vertex shader load failed: {}", vs.error().Detail);
        m_VertexShader = *vs;

        // The depth-only pipeline: set 0 reserved (the registry binds it for the
        // material block / view constants the meshes' draws do not read here, but
        // the layout must match the bound set), one vertex push range for the
        // light-space MVP, no fragment stage, depth write on, no color targets.
        m_Layout = PipelineLayout::Create(m_Context, {
            .Name = "ShadowScenePass Layout",
            .PushConstantRanges = {PushConstantRange::Of<ShadowPushConstants>(ShaderStage::Vertex)},
        });

        optional<VertexBufferLayout> vertexBufferLayout;
        const Renderer::ShaderInterface& vsInterface = m_VertexShader.Get()->Interface;
        if (vsInterface.VertexLayoutId.has_value())
        {
            const AssetResult<AssetHandle<Veng::VertexLayout>> layoutResult =
                assets.LoadSync<Veng::VertexLayout>(*vsInterface.VertexLayoutId);
            VE_ASSERT(layoutResult.has_value(),
                      "ShadowScenePass: vertex layout load failed: {}", layoutResult.error().Detail);
            vertexBufferLayout = layoutResult->Get()->GetLayout();
        }

        m_Pipeline = GraphicsPipeline::Create(m_Context, {
            .Name = "ShadowScenePass Depth Pipeline",
            .ColorAttachments = {},
            .DepthAttachmentFormat = ShadowFormat,
            .VertexBufferLayout = vertexBufferLayout,
            .PipelineLayout = m_Layout,
            .ShaderStages = {
                {.Stage = ShaderStage::Vertex, .Module = m_VertexShader.Get()->Module},
            },
            // Front-face culling keeps the caster's back faces in the depth map,
            // pushing self-shadow acne to the lit side where the slope bias covers it.
            .CullMode = CullMode::Front,
            .DepthTestEnable = true,
            .DepthWriteEnable = true,
        });

        CreateTarget();
    }

    ShadowScenePass::~ShadowScenePass()
    {
        m_Context.GetBindlessRegistry().Release(m_ShadowHandle);
    }

    void ShadowScenePass::CreateTarget()
    {
        // Recreate the depth target at the current resolution; drop the old Refs to
        // retire them and release the old bindless slot through the deferred window,
        // so an in-flight frame's sample of the prior slot is not reclaimed early.
        BindlessRegistry& bindless = m_Context.GetBindlessRegistry();
        bindless.Release(m_ShadowHandle);

        m_ShadowImage = Image::Create(m_Context, {
            .Name = "ShadowScenePass Depth",
            .Extent = {m_Resolution, m_Resolution, 1},
            .Format = ShadowFormat,
            .Usage = ShadowUsage,
        });
        m_ShadowView = ImageView::Create(m_Context, {
            .Name = "ShadowScenePass Depth View",
            .Image = m_ShadowImage,
        });

        m_ShadowHandle = bindless.Register(m_ShadowView);
    }

    void ShadowScenePass::Declare(RenderGraph& graph, const PassIO& io)
    {
        const uvec2 extent{m_Resolution, m_Resolution};

        graph.AddPass("Shadow Depth")
            .Depth({
                .Resource = io.ShadowMap,
                .Load = LoadOp::Clear,
                .Store = StoreOp::Store,
                .Clear = ClearDepth{1.0f, 0},
            })
            .Execute([this, extent](PassContext& inner)
            {
                const ScenePassContext ctx = Wrap(inner);
                CommandBuffer& cmd = ctx.Cmd();
                const SceneView& view = ctx.View();
                const BindlessRegistry& registry = m_Context.GetBindlessRegistry();

                cmd.BindPipeline(m_Pipeline);
                cmd.SetViewport({0, 0}, extent);
                cmd.SetScissor({0, 0}, extent);
                registry.Bind(cmd);

                // The light-space view-projection the renderer wrote into this
                // frame's view-constants region this Execute; the same matrix the
                // lighting pass reads back to project a fragment into shadow space.
                const mat4 lightViewProj = view.LightViewProj;

                // Draw every opaque mesh's geometry into the depth map — the same
                // per-submesh loop the g-buffer pass runs, but only positions matter
                // and only depth is written. Each (Transform, MeshRenderer) entity is
                // drawn at its world transform under the light-space MVP. Casting
                // away const to iterate the borrowed const scene is sound: this only
                // reads.
                const_cast<Scene&>(view.World).Each<Transform, MeshRenderer>(
                    [&](const Entity entity, Transform&, MeshRenderer& meshRenderer)
                {
                    if (!meshRenderer.Mesh.IsLoaded())
                        return;

                    const Mesh& mesh = *meshRenderer.Mesh.Get();
                    const std::span<const AssetHandle<Material>> materials = mesh.GetMaterials();

                    bool materialsReady = true;
                    for (const AssetHandle<Material>& material : materials)
                        materialsReady = materialsReady && material.IsLoaded();
                    if (!materialsReady)
                        return;

                    cmd.BindVertexBuffer(mesh.GetVertexBuffer());
                    cmd.BindIndexBuffer(mesh.GetIndexBuffer());

                    const mat4 world = WorldMatrix(view.World, entity);
                    const mat4 mvp = lightViewProj * world;
                    cmd.PushConstants(ShadowPushConstants{.MVP = mvp});

                    for (const SubMesh& subMesh : mesh.GetSubMeshes())
                    {
                        if (subMesh.MaterialIndex == SubMesh::NoMaterial)
                            continue;
                        cmd.DrawIndexed(subMesh.IndexCount, 1, subMesh.IndexOffset, 0, 0);
                    }
                });
            });
    }
}
