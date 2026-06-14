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
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Asset/ShaderAsset.h>
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

        // Depth buffer for the cube draw (the mesh pass depth-tests so faces
        // resolve correctly regardless of triangle winding).
        m_DepthImage = Renderer::Image::Create(context, {
            .Name = "Scene Depth Image",
            .Extent = {sceneExtent.x, sceneExtent.y, 1},
            .Format = m_DepthFormat,
            .Usage = Renderer::ImageUsage::DepthAttachment,
        });

        m_DepthImageView = Renderer::ImageView::Create(context, {
            .Name = "Scene Depth Image View",
            .Image = m_DepthImage,
        });

        m_Sampler = Renderer::Sampler::Create(context, {
            .Name = "Sample Sampler",
            .AddressModeU = Renderer::AddressMode::ClampToEdge,
            .AddressModeV = Renderer::AddressMode::ClampToEdge,
            .AddressModeW = Renderer::AddressMode::ClampToEdge,
        });

        // Cooked at build time (see CMakeLists.txt) from assets/sample.vengpack.json
        // into HT_ASSET_DIR; mount and load the cube mesh + the brick material by
        // AssetId. The material pulls in its vertex/fragment shaders and the brick
        // texture as eager dependencies, builds its bindless pipeline, and writes a
        // MaterialData entry into the registry's per-material SSBO.
        const VoidResult mountResult = GetAssetManager().Mount(path(HT_ASSET_DIR) / "sample.vengpack");
        VE_ASSERT(mountResult, "{}", mountResult.error());

        const AssetResult<AssetHandle<Veng::Mesh>> cubeMesh =
            GetAssetManager().LoadSync<Veng::Mesh>(AssetId{1002});
        VE_ASSERT(cubeMesh.has_value(), "{}", cubeMesh.error().Detail);
        m_CubeMesh = *cubeMesh;

        const AssetResult<AssetHandle<Veng::Material>> brickMaterial =
            GetAssetManager().LoadSync<Veng::Material>(AssetId{1003});
        VE_ASSERT(brickMaterial.has_value(), "{}", brickMaterial.error().Detail);
        m_BrickMaterial = *brickMaterial;

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
        }
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
            // read — before CompositeToSwapChain's own .Sample() declaration runs.
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
        m_BrickMaterial = {};
        m_CubeMesh = {};
        m_SceneTexture.reset();
        m_CompositePipeline.reset();
        m_CompositeLayout.reset();
        m_CompositeVS = {};
        m_CompositeFS = {};
        m_Sampler.reset();
        m_ImGuiImageView.reset();
        m_DepthImageView.reset();
        m_DepthImage.reset();
        m_SceneImageView.reset();
        m_SceneImage.reset();
    }

private:
    void CreateCompositePipeline()
    {
        auto& context = GetRenderContext();

        const AssetResult<AssetHandle<Veng::ShaderAsset>> vs =
            GetAssetManager().LoadSync<Veng::ShaderAsset>(AssetId{1006});
        VE_ASSERT(vs.has_value(), "{}", vs.error().Detail);
        m_CompositeVS = *vs;

        const AssetResult<AssetHandle<Veng::ShaderAsset>> fs =
            GetAssetManager().LoadSync<Veng::ShaderAsset>(AssetId{1007});
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

    void RenderScene(Renderer::CommandBuffer& cmd)
    {
        const uvec2 extent = {m_SceneImage->GetWidth(), m_SceneImage->GetHeight()};

        // Declare the pass; the graph derives the layout transition and drives
        // BeginRendering/EndRendering from the color attachment. The transition
        // back to a sampleable layout happens at the next declared read — the
        // out-of-graph ImGui sample (see OnRender's cmd.PrepareForAccess) or, in
        // headless, the composite pass — barriers fall out of declared use.
        Renderer::RenderGraph graph;
        graph.AddPass("Scene")
            .Color({
                .View = m_SceneImageView,
                .Load = Renderer::LoadOp::Clear,
                .Store = Renderer::StoreOp::Store,
                .Clear = Renderer::ClearColor{0.05f, 0.05f, 0.08f, 1.0f},
            })
            .Depth({
                .View = m_DepthImageView,
                .Load = Renderer::LoadOp::Clear,
                .Store = Renderer::StoreOp::DontCare,
                .Clear = Renderer::ClearDepth{1.0f, 0},
            })
            .Execute([this, extent](Renderer::CommandBuffer& cmd)
            {
                cmd.SetViewport({0, 0}, extent);
                cmd.SetScissor({0, 0}, extent);

                // The material binds its pipeline (and pushes its per-draw index
                // selector) first; the bindless registry then binds set 0
                // (textures, samplers, MaterialData SSBO) into that pipeline's
                // layout — BindlessRegistry::Bind uses the currently-bound layout,
                // so the pipeline must be bound before it.
                m_BrickMaterial->Bind(cmd);
                GetRenderContext().GetBindlessRegistry().Bind(cmd);

                const Veng::Mesh& mesh = *m_CubeMesh.Get();
                cmd.BindVertexBuffer(mesh.GetVertexBuffer());
                cmd.BindIndexBuffer(mesh.GetIndexBuffer(), mesh.GetIndexType());

                const f32 aspect = static_cast<f32>(extent.x) / static_cast<f32>(extent.y);
                mat4 projection = glm::perspective(glm::radians(45.0f), aspect, 0.1f, 100.0f);
                projection[1][1] *= -1.0f; // Vulkan's clip space has Y pointing down.
                const mat4 view = glm::lookAt(vec3(0.0f, 0.0f, 3.0f), vec3(0.0f), vec3(0.0f, 1.0f, 0.0f));
                const mat4 model = glm::rotate(mat4(1.0f), m_Angle, glm::normalize(vec3(0.5f, 1.0f, 0.2f)));

                // The MVP occupies the leading 64 bytes of the shared push block;
                // Material::Bind already pushed MaterialIndex at offset 64.
                cmd.PushConstants(MeshPushConstants{.MVP = projection * view * model});

                for (const Veng::SubMesh& subMesh : mesh.GetSubMeshes())
                    cmd.DrawIndexed(subMesh.IndexCount, 1, subMesh.IndexOffset, 0, 0);
            });

        graph.Execute(cmd);
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

    void CompositeToSwapChain(Renderer::CommandBuffer& cmd)
    {
        auto& context = GetRenderContext();
        const uvec2 extent = context.GetSwapChainExtent();

        // Sampling the scene and ImGui views declares the reads that drive their
        // transitions to ShaderReadOnly; rendering the swapchain view declares
        // the write that drives its transition to ColorAttachment.
        Renderer::RenderGraph graph;
        graph.AddPass("Composite")
            .Color({
                .View = context.GetCurrentSwapChainImageView(),
                .Load = Renderer::LoadOp::Clear,
                .Store = Renderer::StoreOp::Store,
                .Clear = Renderer::ClearColor{0.0f, 0.0f, 0.0f, 1.0f},
            })
            .Sample(m_SceneImageView)
            .Sample(m_ImGuiImageView)
            .Execute([this, &context, extent](Renderer::CommandBuffer& cmd)
            {
                cmd.BindPipeline(m_CompositePipeline);
                cmd.SetViewport({0, 0}, extent);
                cmd.SetScissor({0, 0}, extent);
                context.GetBindlessRegistry().Bind(cmd);
                cmd.PushConstants(CompositePushConstants{
                    .SceneTexture = m_SceneTextureHandle.Index,
                    .ImGuiTexture = m_ImGuiTextureHandle.Index,
                    .Sampler = m_SamplerHandle.Index,
                });
                cmd.DrawFullscreenTriangle();
            });

        graph.Execute(cmd);
    }

    Renderer::Format m_SceneFormat = Renderer::Format::Undefined;
    Renderer::Format m_DepthFormat = Renderer::Format::D32Sfloat;
    Ref<Renderer::Image> m_SceneImage;
    Ref<Renderer::ImageView> m_SceneImageView;
    Ref<Renderer::Image> m_DepthImage;
    Ref<Renderer::ImageView> m_DepthImageView;
    Ref<Renderer::ImageView> m_ImGuiImageView;
    Ref<Renderer::Sampler> m_Sampler;

    AssetHandle<Veng::Mesh> m_CubeMesh;
    AssetHandle<Veng::Material> m_BrickMaterial;

    AssetHandle<Veng::ShaderAsset> m_CompositeVS;
    AssetHandle<Veng::ShaderAsset> m_CompositeFS;
    Ref<Renderer::PipelineLayout> m_CompositeLayout;
    Ref<Renderer::GraphicsPipeline> m_CompositePipeline;
    Renderer::TextureHandle m_SceneTextureHandle;
    Renderer::TextureHandle m_ImGuiTextureHandle;
    Renderer::SamplerHandle m_SamplerHandle;

    Ref<ImGuiTexture> m_SceneTexture;

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
