#include <Veng/Application.h>
#include <Veng/Assert.h>
#include <Veng/Log.h>
#include <Veng/Vendor/ImGui.h>

#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/Command.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/DynamicGraphicsPipeline.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Renderer/Shader.h>
#include <Veng/Renderer/TypedBuffers.h>

#include <glm/gtc/packing.hpp>

#include <cstdlib>
#include <fstream>

using namespace Veng;

namespace
{
    struct Vertex
    {
        vec2 Position;
        vec3 Color;
    };

    const Vertex k_Vertices[] = {
        {{0.0f, -0.6f}, {1.0f, 0.2f, 0.2f}},
        {{0.6f, 0.6f}, {0.2f, 1.0f, 0.2f}},
        {{-0.6f, 0.6f}, {0.2f, 0.2f, 1.0f}},
    };

    const u16 k_Indices[] = {0, 1, 2};
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

        m_SceneImage = Renderer::Image::Create({
            .Name = "Scene Image",
            .Extent = {sceneExtent.x, sceneExtent.y, 1},
            .Format = context.GetOutputFormat(),
            .Usage = Renderer::ImageUsage::ColorAttachment | Renderer::ImageUsage::Sampled |
            Renderer::ImageUsage::TransferSrc,
        });

        m_SceneImageView = Renderer::ImageView::Create({
            .Name = "Scene Image View",
            .Image = m_SceneImage,
        });

        m_Sampler = Renderer::Sampler::Create({
            .Name = "Sample Sampler",
            .AddressModeU = Renderer::AddressMode::ClampToEdge,
            .AddressModeV = Renderer::AddressMode::ClampToEdge,
            .AddressModeW = Renderer::AddressMode::ClampToEdge,
        });

        m_VertexBuffer = Renderer::VertexBuffer<Vertex>::Create("Triangle Vertices", std::size(k_Vertices));
        m_VertexBuffer.Upload(k_Vertices);

        m_IndexBuffer = Renderer::IndexBuffer::Create("Triangle Indices", std::size(k_Indices),
                                                      Renderer::IndexType::U16);
        m_IndexBuffer.Upload(k_Indices);

        CreateTrianglePipeline();

        // The compositing path (ImGui overlay + swapchain present) only exists in
        // windowed mode. The headless smoke run renders just the scene and
        // downloads it — no ImGui layer, no swapchain.
        if (GetImGuiLayer())
        {
            m_ImGuiImageView = Renderer::ImageView::Create({
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
            RenderUserInterface();
            GetImGuiLayer()->Render(cmd);
            CompositeToSwapChain(cmd);
        }
    }

    void OnDispose() override
    {
        m_SceneTexture.reset();
        m_CompositeSet.reset();
        m_CompositePipeline.reset();
        m_CompositeLayout.reset();
        m_CompositeSetLayout.reset();
        m_TrianglePipeline.reset();
        m_TriangleLayout.reset();
        m_VertexBuffer = {};
        m_IndexBuffer = {};
        m_Sampler.reset();
        m_ImGuiImageView.reset();
        m_SceneImageView.reset();
        m_SceneImage.reset();
    }

private:
    void CreateTrianglePipeline()
    {
        const auto vertexShader = Renderer::Shader::Create({
            .Name = "triangle.vert",
            .Path = path(HT_SHADER_DIR) / "triangle.vert.spv",
        });
        VE_ASSERT(vertexShader, "{}", vertexShader.error());

        const auto fragmentShader = Renderer::Shader::Create({
            .Name = "triangle.frag",
            .Path = path(HT_SHADER_DIR) / "triangle.frag.spv",
        });
        VE_ASSERT(fragmentShader, "{}", fragmentShader.error());

        m_TriangleLayout = Renderer::PipelineLayout::Create({
            .Name = "Triangle Layout",
            .PushConstantRanges = {
                Renderer::PushConstantRange::Of<mat4>(Renderer::ShaderStage::Vertex),
            },
        });

        m_TrianglePipeline = Renderer::DynamicGraphicsPipeline::Create({
            .Name = "Triangle Pipeline",
            .ColorAttachments = {{.Format = GetRenderContext().GetOutputFormat()}},
            .VertexBufferLayout = Renderer::VertexBufferLayout({
                {Renderer::VertexElementDataType::Float2, "a_Position"},
                {Renderer::VertexElementDataType::Float3, "a_Color"},
            }),
            .PipelineLayout = m_TriangleLayout,
            .ShaderStages = {
                {.Stage = Renderer::ShaderStage::Vertex, .Module = *vertexShader.value()},
                {.Stage = Renderer::ShaderStage::Fragment, .Module = *fragmentShader.value()},
            },
        });
    }

    void CreateCompositePipeline()
    {
        const auto vertexShader = Renderer::Shader::Create({
            .Name = "composite.vert",
            .Path = path(HT_SHADER_DIR) / "composite.vert.spv",
        });
        VE_ASSERT(vertexShader, "{}", vertexShader.error());

        const auto fragmentShader = Renderer::Shader::Create({
            .Name = "composite.frag",
            .Path = path(HT_SHADER_DIR) / "composite.frag.spv",
        });
        VE_ASSERT(fragmentShader, "{}", fragmentShader.error());

        m_CompositeSetLayout = Renderer::DescriptorSetLayout::Create({
            .Name = "Composite Set Layout",
            .Bindings = {
                {
                    .Binding = 0,
                    .Type = Renderer::DescriptorType::CombinedImageSampler,
                    .Count = 1,
                    .Stages = Renderer::ShaderStage::Fragment,
                },
                {
                    .Binding = 1,
                    .Type = Renderer::DescriptorType::CombinedImageSampler,
                    .Count = 1,
                    .Stages = Renderer::ShaderStage::Fragment,
                },
            },
        });

        m_CompositeLayout = Renderer::PipelineLayout::Create({
            .Name = "Composite Layout",
            .DescriptorSetLayouts = {m_CompositeSetLayout},
        });

        m_CompositePipeline = Renderer::DynamicGraphicsPipeline::Create({
            .Name = "Composite Pipeline",
            .ColorAttachments = {{.Format = GetRenderContext().GetSwapChainFormat()}},
            .PipelineLayout = m_CompositeLayout,
            .ShaderStages = {
                {.Stage = Renderer::ShaderStage::Vertex, .Module = *vertexShader.value()},
                {.Stage = Renderer::ShaderStage::Fragment, .Module = *fragmentShader.value()},
            },
        });

        m_CompositeSet = Renderer::DescriptorSet::Create({
            .Name = "Composite Set",
            .Layout = m_CompositeSetLayout,
        });

        m_CompositeSet->Write(0, m_SceneImageView, m_Sampler);
        m_CompositeSet->Write(1, m_ImGuiImageView, m_Sampler);
    }

    void RenderScene(Renderer::CommandBuffer& cmd)
    {
        const uvec2 extent = {m_SceneImage->GetWidth(), m_SceneImage->GetHeight()};

        // Declare the pass; the graph derives the layout transition and drives
        // BeginRendering/EndRendering from the color attachment. The transition
        // back to a sampleable layout is derived by the composite pass that reads
        // the scene image — barriers fall out of declared use, not manual calls.
        Renderer::RenderGraph graph;
        graph.AddPass("Scene")
            .Color({
                .View = m_SceneImageView,
                .Load = Renderer::LoadOp::Clear,
                .Store = Renderer::StoreOp::Store,
                .Clear = Renderer::ClearColor{0.05f, 0.05f, 0.08f, 1.0f},
            })
            .Execute([this, extent](Renderer::CommandBuffer& cmd)
            {
                cmd.BindPipeline(m_TrianglePipeline);
                cmd.SetViewport({0, 0}, extent);
                cmd.SetScissor({0, 0}, extent);
                cmd.BindVertexBuffer(m_VertexBuffer);
                cmd.BindIndexBuffer(m_IndexBuffer);

                const f32 aspect = static_cast<f32>(extent.x) / static_cast<f32>(extent.y);
                const mat4 transform = glm::scale(mat4(1.0f), vec3(1.0f / aspect, 1.0f, 1.0f)) *
                    glm::rotate(mat4(1.0f), m_Angle, vec3(0.0f, 0.0f, 1.0f));

                cmd.PushConstants(transform);

                cmd.DrawIndexed(static_cast<u32>(m_IndexBuffer.GetIndexCount()), 1, 0, 0, 0);
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
            .Execute([this, extent](Renderer::CommandBuffer& cmd)
            {
                cmd.BindPipeline(m_CompositePipeline);
                cmd.SetViewport({0, 0}, extent);
                cmd.SetScissor({0, 0}, extent);
                cmd.BindDescriptorSets({.Sets = {m_CompositeSet}});
                cmd.DrawFullscreenTriangle();
            });

        graph.Execute(cmd);
    }

    Ref<Renderer::Image> m_SceneImage;
    Ref<Renderer::ImageView> m_SceneImageView;
    Ref<Renderer::ImageView> m_ImGuiImageView;
    Ref<Renderer::Sampler> m_Sampler;
    Renderer::VertexBuffer<Vertex> m_VertexBuffer;
    Renderer::IndexBuffer m_IndexBuffer;

    Ref<Renderer::PipelineLayout> m_TriangleLayout;
    Ref<Renderer::DynamicGraphicsPipeline> m_TrianglePipeline;

    Ref<Renderer::DescriptorSetLayout> m_CompositeSetLayout;
    Ref<Renderer::PipelineLayout> m_CompositeLayout;
    Ref<Renderer::DynamicGraphicsPipeline> m_CompositePipeline;
    Ref<Renderer::DescriptorSet> m_CompositeSet;

    Ref<ImGuiTexture> m_SceneTexture;

    f32 m_Angle = 0.0f;
    u32 m_FrameCount = 0;
    const char* m_SmokeOutput = nullptr;
};

int main(const int argc, char** argv)
{
    // Smoke mode runs headless: no window or swapchain, render the scene
    // off-screen and dump it. This is the display-free CI path enabled by the
    // headless context (plan 10).
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
