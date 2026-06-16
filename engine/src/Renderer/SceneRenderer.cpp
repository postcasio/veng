#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Renderer/ScenePass.h>

#include <Veng/Assert.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>

#include <Veng/Asset/Material.h>
#include <Veng/Asset/Mesh.h>

#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/Transforms.h>

namespace Veng::Renderer
{
    namespace
    {
        // The material shader's vertex push constant: the MVP for clip-space
        // positions. It is the leading 64 bytes of the shared push-constant block;
        // the material's per-draw selector (MaterialIndex) occupies offset 64 and is
        // pushed by Material::Bind.
        struct MeshPushConstants
        {
            mat4 MVP;
        };

        // The single forward pass unit: draws every (Transform, MeshRenderer) entity
        // into the renderer's imported output as a color attachment, with a
        // write-only depth transient. Sourced entirely from the per-frame SceneView.
        class ForwardScenePass final : public ScenePass
        {
        public:
            ForwardScenePass(Context& context, Format depthFormat, uvec2 extent)
                : m_Context(context), m_DepthFormat(depthFormat), m_Extent(extent)
            {
            }

            void Resize(const uvec2 extent) override { m_Extent = extent; }

            void Declare(RenderGraph& graph, const PassIO& io) override
            {
                // Depth is write-only (cleared, never sampled), so a graph transient
                // is correct here.
                const ResourceId depthId = graph.CreateTransient({
                    .Name = "Scene Depth Image",
                    .Format = m_DepthFormat,
                    .Extent = m_Extent,
                    .Usage = ImageUsage::DepthAttachment,
                });

                graph.AddPass("Scene")
                    .Color({
                        .Resource = io.Output,
                        .Load = LoadOp::Clear,
                        .Store = StoreOp::Store,
                        .Clear = ClearColor{0.05f, 0.05f, 0.08f, 1.0f},
                    })
                    .Depth({
                        .Resource = depthId,
                        .Load = LoadOp::Clear,
                        .Store = StoreOp::DontCare,
                        .Clear = ClearDepth{1.0f, 0},
                    })
                    .Execute([this](PassContext& inner)
                    {
                        Record(Wrap(inner));
                    });
            }

        private:
            void Record(const ScenePassContext& ctx) const
            {
                CommandBuffer& cmd = ctx.Cmd();
                const SceneView& view = ctx.View();

                cmd.SetViewport({0, 0}, m_Extent);
                cmd.SetScissor({0, 0}, m_Extent);

                const mat4 viewProjection = view.Camera.ViewProjection();
                const BindlessRegistry& registry = m_Context.GetBindlessRegistry();

                // Each (Transform, MeshRenderer) entity draws its own mesh at its
                // world transform; the mesh is the MeshRenderer's AssetHandle —
                // cooked or adopted runtime primitive alike. Scene::Each yields
                // mutable component references and the borrowed view is const; this
                // pass only reads, so casting away const to iterate is sound.
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
                    const mat4 mvp = viewProjection * world;

                    for (const SubMesh& subMesh : mesh.GetSubMeshes())
                    {
                        if (subMesh.MaterialIndex == SubMesh::NoMaterial)
                            continue;

                        // The submesh's material binds its pipeline (and pushes its
                        // per-draw index selector) first; the bindless registry then
                        // binds set 0 into that pipeline's layout — Bind uses the
                        // currently-bound layout, so the pipeline must be bound
                        // before it. The MVP occupies the leading 64 bytes of the
                        // shared push block; Material::Bind already pushed
                        // MaterialIndex at offset 64.
                        materials[subMesh.MaterialIndex].Get()->Bind(cmd);
                        registry.Bind(cmd);
                        cmd.PushConstants(MeshPushConstants{.MVP = mvp});

                        cmd.DrawIndexed(subMesh.IndexCount, 1, subMesh.IndexOffset, 0, 0);
                    }
                });
            }

            Context& m_Context;
            Format m_DepthFormat;
            uvec2 m_Extent;
        };
    }

    // Holds the compiled graph + the stable import-binding slot rebound per Execute.
    // Kept out of the header so SceneRenderer.h needs no CompiledGraph definition.
    struct SceneRenderer::Internal
    {
        Unique<CompiledGraph> Graph;
    };

    Unique<SceneRenderer> SceneRenderer::Create(const SceneRendererInfo& info)
    {
        return Unique<SceneRenderer>(new SceneRenderer(info));
    }

    SceneRenderer::SceneRenderer(const SceneRendererInfo& info)
        : m_Context(info.Context),
          m_OutputFormat(info.OutputFormat),
          m_Extent(info.Extent),
          m_Settings(info.Settings),
          m_Internal(CreateUnique<Internal>())
    {
        m_Passes.push_back(CreateUnique<ForwardScenePass>(
            m_Context, m_Context.GetDepthFormat(), m_Extent));

        CreateOutput();
        Rebuild();
    }

    SceneRenderer::~SceneRenderer() = default;

    void SceneRenderer::CreateOutput()
    {
        // Sampled so a consumer/composite samples the result; TransferSrc so the
        // smoke path can Download() it. Dropping the old Refs retires them through
        // the per-frame deferred-destruction path.
        m_OutputImage = Image::Create(m_Context, {
            .Name = "SceneRenderer Output",
            .Extent = {m_Extent.x, m_Extent.y, 1},
            .Format = m_OutputFormat,
            .Usage = ImageUsage::ColorAttachment | ImageUsage::Sampled | ImageUsage::TransferSrc,
        });

        m_OutputView = ImageView::Create(m_Context, {
            .Name = "SceneRenderer Output View",
            .Image = m_OutputImage,
        });
    }

    void SceneRenderer::Rebuild()
    {
        RenderGraph graph(m_Context);
        m_OutputId = graph.Import("SceneRenderer Output");

        const PassIO io{.Output = m_OutputId};
        for (const Unique<ScenePass>& pass : m_Passes)
        {
            pass->Configure(m_Settings);
            pass->Resize(m_Extent);
            pass->Declare(graph, io);
        }

        m_Internal->Graph = graph.Compile();
    }

    void SceneRenderer::Resize(const uvec2 extent)
    {
        m_Extent = extent;
        CreateOutput();
        Rebuild();
    }

    void SceneRenderer::Configure(const SceneRendererSettings& settings)
    {
        m_Settings = settings;
        Rebuild();
    }

    void SceneRenderer::Execute(CommandBuffer& cmd, const SceneView& view)
    {
        const RenderGraph::ImportBinding binding{m_OutputId, m_OutputView};
        m_Internal->Graph->Execute(cmd, {&binding, 1}, const_cast<SceneView*>(&view));
    }

    Ref<ImageView> SceneRenderer::GetOutput() const { return m_OutputView; }
}
