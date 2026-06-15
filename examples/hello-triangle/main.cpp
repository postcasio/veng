#include <Veng/Application.h>
#include <Veng/Assert.h>
#include <Veng/Log.h>
#include <Veng/Vendor/ImGui.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Primitives.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Asset/Texture.h>

#include <glm/gtc/packing.hpp>

#include <cstdlib>
#include <fstream>

using namespace Veng;

namespace
{
    // Selects the composite shader's bindless texture/sampler slots
    // (Veng/Renderer/BindlessRegistry.h) — set 0 is bound once via
    // BindlessRegistry::Bind, these indices pick the array elements.
    struct CompositePushConstants
    {
        u32 SceneTexture;
        u32 ImGuiTexture;
        u32 Sampler;
    };

    // The material shader's vertex push constant: the MVP for clip-space
    // positions (brick.vert.slang). It is the leading 64 bytes of the shared
    // push-constant block; the material's per-draw selector (MaterialIndex)
    // occupies offset 64 and is pushed by Material::Bind, not here.
    struct MeshPushConstants
    {
        mat4 MVP;
    };
}

class HelloTriangleApp final : public Application
{
public:
    explicit HelloTriangleApp(const ApplicationInfo& info) : Application(info)
    {
    }

protected:
    void OnInitialize() override
    {
        auto& context = GetRenderContext();

        const uvec2 sceneExtent = context.GetInternalRenderExtent();

        m_SmokeOutput = std::getenv("HT_SMOKE");

        // Defined once here and referenced by both the scene image and the
        // triangle pipeline's color attachment (see CreateTrianglePipeline), so
        // they can't drift apart.
        m_SceneFormat = context.GetOutputFormat();

        m_SceneImage = Renderer::Image::Create(context, {
            .Name = "Scene Image",
            .Extent = {sceneExtent.x, sceneExtent.y, 1},
            .Format = m_SceneFormat,
            .Usage = Renderer::ImageUsage::ColorAttachment | Renderer::ImageUsage::Sampled |
            Renderer::ImageUsage::TransferSrc,
        });

        m_SceneImageView = Renderer::ImageView::Create(context, {
            .Name = "Scene Image View",
            .Image = m_SceneImage,
        });

        m_Sampler = Renderer::Sampler::Create(context, {
            .Name = "Sample Sampler",
            .AddressModeU = Renderer::AddressMode::ClampToEdge,
            .AddressModeV = Renderer::AddressMode::ClampToEdge,
            .AddressModeW = Renderer::AddressMode::ClampToEdge,
        });

        // Cooked at build time (see CMakeLists.txt) from assets/sample.vengpack.json
        // into HT_ASSET_DIR; mount and load the brick material by AssetId. Loading
        // the material pulls in its vertex/fragment shaders and the brick texture as
        // eager dependencies, builds its bindless pipeline, and writes a MaterialData
        // entry into the registry's per-material SSBO.
        const VoidResult mountResult = GetAssetManager().Mount(path(HT_ASSET_DIR) / "sample.vengpack");
        VE_ASSERT(mountResult, "{}", mountResult.error());

        // The primitive generator records this material instance on the produced
        // submesh, so it must be resident before Mesh::Create hands it in.
        const AssetResult<AssetHandle<Veng::Material>> brickMaterial =
            GetAssetManager().LoadSync<Veng::Material>(AssetId{0x3EB});
        VE_ASSERT(brickMaterial.has_value(), "{}", brickMaterial.error().Detail);
        m_BrickMaterial = *brickMaterial;

        // Build the geometry at runtime rather than loading a cooked mesh: a
        // geodesic icosphere in the canonical layout, carrying the brick material
        // instance on its single submesh. Its near-uniform tessellation shows the
        // brick UV mapping without the pole clustering of a UV sphere.
        // Mesh::Create uploads synchronously, so it is ready to draw.
        m_Mesh = Veng::Mesh::Create(
            context, Veng::Primitives::Icosphere(0.8f, 4, m_BrickMaterial), "Demo Sphere");

        // The compositing path (ImGui overlay + swapchain present) only exists in
        // windowed mode. The headless smoke run renders just the scene and
        // downloads it — no ImGui layer, no swapchain.
        if (GetImGuiLayer())
        {
            m_ImGuiImageView = Renderer::ImageView::Create(context, {
                .Name = "Sample ImGui Image View",
                .Image = GetImGuiLayer()->GetOutputImage(),
            });

            CreateCompositePipeline();

            m_SceneTexture = GetImGuiLayer()->CreateTexture(*m_Sampler, *m_SceneImageView);

            // A swapchain resize invalidates the composite pass's baked extent;
            // rebuild + re-Compile() both graphs against the new size. The scene
            // graph holds the depth transient whose extent must track the scene
            // image. The headless smoke path has a fixed extent, so this never
            // fires there.
            context.AddSwapChainInvalidationCallback([this]
            {
                m_SceneGraph = BuildSceneGraph();
                m_CompositeGraph = BuildCompositeGraph();
            });
        }

        // Compile the graphs once and replay them every frame. A structural change
        // (resize) re-Compile()s via the invalidation callback above.
        m_SceneGraph = BuildSceneGraph();
        if (GetImGuiLayer())
            m_CompositeGraph = BuildCompositeGraph();
    }

    void OnUpdate(const f32 delta) override
    {
        m_Angle += delta;

        // Smoke-test mode: after a few rendered frames, dump the scene image to
        // disk and exit. Runs before this frame's commands are recorded, so the
        // image holds the previous frame's completed contents.
        if (m_SmokeOutput && ++m_FrameCount == 20)
        {
            WriteSceneCapture(m_SmokeOutput);
            RequestExit();
        }
    }

    void OnRender() override
    {
        auto& context = GetRenderContext();
        auto& cmd = context.GetCurrentCommandBuffer();

        RenderScene(cmd);

        // Headless (smoke) renders only the scene; the ImGui overlay and the
        // composite-to-swapchain pass are windowed-only.
        if (GetImGuiLayer())
        {
            // RenderUserInterface() draws m_SceneTexture via ImGui::Image(), and
            // GetImGuiLayer()->Render(cmd) is what actually records that sampled
            // read — before the composite pass's own .Sample() declaration runs.
            // ImGui samples outside the graph, so declare the read explicitly:
            // transition the scene image to a sampleable layout before that pass.
            cmd.PrepareForAccess(m_SceneImageView, Renderer::AccessKind::Sample);

            RenderUserInterface();
            GetImGuiLayer()->Render(cmd);
            CompositeToSwapChain(cmd);
        }
    }

    void OnDispose() override
    {
        // The compiled graphs own the depth transient; release them so their GPU
        // resources retire before the context tears down.
        m_SceneGraph.reset();
        m_CompositeGraph.reset();
        m_Mesh.reset();
        m_BrickMaterial = {};
        m_SceneTexture.reset();
        m_CompositePipeline.reset();
        m_CompositeLayout.reset();
        m_CompositeVS = {};
        m_CompositeFS = {};
        m_Sampler.reset();
        m_ImGuiImageView.reset();
        m_SceneImageView.reset();
        m_SceneImage.reset();
    }

private:
    void CreateCompositePipeline()
    {
        auto& context = GetRenderContext();

        const AssetResult<AssetHandle<Veng::Shader>> vs =
            GetAssetManager().LoadSync<Veng::Shader>(AssetId{0x3EE});
        VE_ASSERT(vs.has_value(), "{}", vs.error().Detail);
        m_CompositeVS = *vs;

        const AssetResult<AssetHandle<Veng::Shader>> fs =
            GetAssetManager().LoadSync<Veng::Shader>(AssetId{0x3EF});
        VE_ASSERT(fs.has_value(), "{}", fs.error().Detail);
        m_CompositeFS = *fs;

        m_CompositeLayout = Renderer::PipelineLayout::Create(context, {
            .Name = "Composite Layout",
            .PushConstantRanges = {
                Renderer::PushConstantRange::Of<CompositePushConstants>(Renderer::ShaderStage::Fragment),
            },
        });

        m_CompositePipeline = Renderer::GraphicsPipeline::Create(context, {
            .Name = "Composite Pipeline",
            // The composite pass renders into the swapchain image, which the
            // context owns and already exposes a single format accessor — no
            // separate Image::Create to keep in sync with here.
            .ColorAttachments = {{.Format = context.GetSwapChainFormat()}},
            .PipelineLayout = m_CompositeLayout,
            .ShaderStages = {
                {.Stage = Renderer::ShaderStage::Vertex, .Module = m_CompositeVS.Get()->Module},
                {.Stage = Renderer::ShaderStage::Fragment, .Module = m_CompositeFS.Get()->Module},
            },
        });

        // Register the scene/ImGui views and the shared sampler into the
        // bindless registry (set 0) — composite.frag indexes them via push
        // constants.
        auto& bindless = context.GetBindlessRegistry();
        m_SceneTextureHandle = bindless.Register(m_SceneImageView);
        m_ImGuiTextureHandle = bindless.Register(m_ImGuiImageView);
        m_SamplerHandle = bindless.Register(m_Sampler);
    }

    // Build and compile the scene graph. Compiled once (in OnInitialize / on
    // resize) and replayed every frame; the callback closes over only `this` and
    // reads per-frame state (m_Angle → the MVP) and the scene extent from the
    // resolved scene image, so no frame-varying value is baked into it.
    Unique<Renderer::CompiledGraph> BuildSceneGraph()
    {
        auto& context = GetRenderContext();
        const uvec2 extent = {m_SceneImage->GetWidth(), m_SceneImage->GetHeight()};

        // Declare the pass; the graph derives the layout transition and drives
        // BeginRendering/EndRendering from the color attachment. The transition
        // back to a sampleable layout happens at the next declared read — the
        // out-of-graph ImGui sample (see OnRender's cmd.PrepareForAccess) or, in
        // headless, the composite pass — barriers fall out of declared use.
        //
        // The scene image is app-owned (ImGui and the composite pass sample it, the
        // smoke path downloads it) so it is imported; the depth buffer is
        // written-then-discarded within this pass so it is a graph transient.
        Renderer::RenderGraph graph(context);
        m_SceneId = graph.Import("Scene");
        const Renderer::ResourceId depthId = graph.CreateTransient({
            .Name = "Scene Depth Image",
            .Format = m_DepthFormat,
            .Extent = extent,
            .Usage = Renderer::ImageUsage::DepthAttachment,
        });
        graph.AddPass("Scene")
            .Color({
                .Resource = m_SceneId,
                .Load = Renderer::LoadOp::Clear,
                .Store = Renderer::StoreOp::Store,
                .Clear = Renderer::ClearColor{0.05f, 0.05f, 0.08f, 1.0f},
            })
            .Depth({
                .Resource = depthId,
                .Load = Renderer::LoadOp::Clear,
                .Store = Renderer::StoreOp::DontCare,
                .Clear = Renderer::ClearDepth{1.0f, 0},
            })
            .Execute([this](Renderer::PassContext& ctx)
            {
                Renderer::CommandBuffer& cmd = ctx.Cmd();
                const uvec2 extent = {m_SceneImage->GetWidth(), m_SceneImage->GetHeight()};
                cmd.SetViewport({0, 0}, extent);
                cmd.SetScissor({0, 0}, extent);

                const Veng::Mesh& mesh = *m_Mesh;
                const std::span<const AssetHandle<Veng::Material>> materials = mesh.GetMaterials();

                // The brick material loads synchronously in OnInitialize before the
                // mesh is generated, so the mesh's material list is always resident
                // here. Guard anyway: an async-loaded material would clear-only until
                // it lands.
                bool materialsReady = true;
                for (const AssetHandle<Veng::Material>& material : materials)
                    materialsReady = materialsReady && material.IsLoaded();
                if (!materialsReady)
                    return;

                cmd.BindVertexBuffer(mesh.GetVertexBuffer());
                cmd.BindIndexBuffer(mesh.GetIndexBuffer());

                const f32 aspect = static_cast<f32>(extent.x) / static_cast<f32>(extent.y);
                mat4 projection = glm::perspective(glm::radians(45.0f), aspect, 0.1f, 100.0f);
                projection[1][1] *= -1.0f; // Vulkan's clip space has Y pointing down.
                const mat4 view = glm::lookAt(vec3(0.0f, 0.0f, 3.0f), vec3(0.0f), vec3(0.0f, 1.0f, 0.0f));
                const mat4 model = glm::rotate(mat4(1.0f), m_Angle, glm::normalize(vec3(0.5f, 1.0f, 0.2f)));

                const Veng::Renderer::BindlessRegistry& registry = GetRenderContext().GetBindlessRegistry();

                for (const Veng::SubMesh& subMesh : mesh.GetSubMeshes())
                {
                    if (subMesh.MaterialIndex == Veng::SubMesh::NoMaterial)
                        continue;

                    // The submesh's material binds its pipeline (and pushes its
                    // per-draw index selector) first; the bindless registry then
                    // binds set 0 (textures, samplers, MaterialData SSBO) into that
                    // pipeline's layout — BindlessRegistry::Bind uses the
                    // currently-bound layout, so the pipeline must be bound before
                    // it. The MVP occupies the leading 64 bytes of the shared push
                    // block; Material::Bind already pushed MaterialIndex at offset 64.
                    materials[subMesh.MaterialIndex].Get()->Bind(cmd);
                    registry.Bind(cmd);
                    cmd.PushConstants(MeshPushConstants{.MVP = projection * view * model});

                    cmd.DrawIndexed(subMesh.IndexCount, 1, subMesh.IndexOffset, 0, 0);
                }
            });

        return graph.Compile();
    }

    void RenderScene(Renderer::CommandBuffer& cmd)
    {
        const Renderer::RenderGraph::ImportBinding sceneBinding{m_SceneId, m_SceneImageView};
        m_SceneGraph->Execute(cmd, {&sceneBinding, 1});
    }

    void RenderUserInterface() const
    {
        ImGui::Begin("Scene");
        const ImVec2 available = ImGui::GetContentRegionAvail();
        const f32 aspect = static_cast<f32>(m_SceneImage->GetHeight()) / static_cast<f32>(m_SceneImage->GetWidth());
        ImGui::Image(static_cast<ImTextureID>(m_SceneTexture->GetTextureId()),
                     {available.x, available.x * aspect});
        ImGui::End();

        ImGui::Begin("Stats");
        ImGui::Text("%.1f fps (%.2f ms)", ImGui::GetIO().Framerate, 1000.0f / ImGui::GetIO().Framerate);
        ImGui::End();
    }

    void WriteSceneCapture(const char* outPath) const
    {
        const auto data = m_SceneImage->Download();
        const u32 width = m_SceneImage->GetWidth();
        const u32 height = m_SceneImage->GetHeight();

        // Scene image is RGBA16F; decode to 8-bit RGB for a binary PPM.
        const auto* halves = reinterpret_cast<const u16*>(data.data());

        std::ofstream out(outPath, std::ios::binary);
        out << "P6\n" << width << " " << height << "\n255\n";

        for (u32 pixel = 0; pixel < width * height; pixel++)
        {
            for (u32 channel = 0; channel < 3; channel++)
            {
                const f32 value = glm::clamp(glm::unpackHalf1x16(halves[pixel * 4 + channel]), 0.0f, 1.0f);
                out.put(static_cast<char>(value * 255.0f + 0.5f));
            }
        }

        Log::Info("Wrote scene capture to {}", outPath);
    }

    // Build and compile the composite graph. Compiled once and replayed every
    // frame; the callback closes over only `this` and reads the swapchain extent
    // live (fixed for a given swapchain — a resize re-Compile()s the graph).
    Unique<Renderer::CompiledGraph> BuildCompositeGraph()
    {
        auto& context = GetRenderContext();

        // Sampling the scene and ImGui views declares the reads that drive their
        // transitions to ShaderReadOnly; rendering the swapchain view declares
        // the write that drives its transition to ColorAttachment. All three are
        // app-/context-owned, so they are imports; the swapchain view differs each
        // frame and is supplied per call.
        Renderer::RenderGraph graph(context);
        m_SwapId = graph.Import("SwapChain");
        m_CompositeSceneId = graph.Import("Scene");
        m_ImGuiId = graph.Import("ImGui");
        graph.AddPass("Composite")
            .Color({
                .Resource = m_SwapId,
                .Load = Renderer::LoadOp::Clear,
                .Store = Renderer::StoreOp::Store,
                .Clear = Renderer::ClearColor{0.0f, 0.0f, 0.0f, 1.0f},
            })
            .Sample(m_CompositeSceneId)
            .Sample(m_ImGuiId)
            .Execute([this](Renderer::PassContext& ctx)
            {
                Renderer::CommandBuffer& cmd = ctx.Cmd();
                const uvec2 extent = GetRenderContext().GetSwapChainExtent();
                cmd.BindPipeline(m_CompositePipeline);
                cmd.SetViewport({0, 0}, extent);
                cmd.SetScissor({0, 0}, extent);
                GetRenderContext().GetBindlessRegistry().Bind(cmd);
                cmd.PushConstants(CompositePushConstants{
                    .SceneTexture = m_SceneTextureHandle.Index,
                    .ImGuiTexture = m_ImGuiTextureHandle.Index,
                    .Sampler = m_SamplerHandle.Index,
                });
                cmd.DrawFullscreenTriangle();
            });

        return graph.Compile();
    }

    void CompositeToSwapChain(Renderer::CommandBuffer& cmd)
    {
        const Renderer::RenderGraph::ImportBinding bindings[] = {
            {m_SwapId, GetRenderContext().GetCurrentSwapChainImageView()},
            {m_CompositeSceneId, m_SceneImageView},
            {m_ImGuiId, m_ImGuiImageView},
        };
        m_CompositeGraph->Execute(cmd, bindings);
    }

    Renderer::Format m_SceneFormat = Renderer::Format::Undefined;
    Renderer::Format m_DepthFormat = Renderer::Format::D32Sfloat;
    Ref<Renderer::Image> m_SceneImage;
    Ref<Renderer::ImageView> m_SceneImageView;
    Ref<Renderer::ImageView> m_ImGuiImageView;
    Ref<Renderer::Sampler> m_Sampler;

    AssetHandle<Veng::Material> m_BrickMaterial;
    Ref<Veng::Mesh> m_Mesh;

    AssetHandle<Veng::Shader> m_CompositeVS;
    AssetHandle<Veng::Shader> m_CompositeFS;
    Ref<Renderer::PipelineLayout> m_CompositeLayout;
    Ref<Renderer::GraphicsPipeline> m_CompositePipeline;
    Renderer::TextureHandle m_SceneTextureHandle;
    Renderer::TextureHandle m_ImGuiTextureHandle;
    Renderer::SamplerHandle m_SamplerHandle;

    Ref<ImGuiTexture> m_SceneTexture;

    // Compiled once and replayed every frame; re-Compile()d on swapchain resize.
    Unique<Renderer::CompiledGraph> m_SceneGraph;
    Unique<Renderer::CompiledGraph> m_CompositeGraph;

    // Import slots bound per frame to Execute. Stable across replays; only the
    // concrete views they bind to change.
    Renderer::ResourceId m_SceneId;
    Renderer::ResourceId m_SwapId;
    Renderer::ResourceId m_CompositeSceneId;
    Renderer::ResourceId m_ImGuiId;

    f32 m_Angle = 0.0f;
    u32 m_FrameCount = 0;
    const char* m_SmokeOutput = nullptr;
};

int main(const int argc, char** argv)
{
    // Smoke mode runs headless: no window or swapchain, render the scene
    // off-screen and dump it. This is the display-free CI path enabled by the
    // headless context.
    const bool smoke = std::getenv("HT_SMOKE") != nullptr;

    HelloTriangleApp app({
        .Name = "Hello Triangle",
        .InternalRenderExtent = {1280, 720},
        .WindowInfo = {
            .Extent = {1280, 720},
            .Resizable = false,
            .EventCallback = [](Event&) {},
            .Title = "veng — Hello Triangle",
            .CaptureMouse = false,
        },
        .Headless = smoke,
    });

    app.Run(vector<string>(argv, argv + argc));

    return 0;
}
