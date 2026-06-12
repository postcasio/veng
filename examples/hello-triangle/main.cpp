#include <Veng/Application.h>
#include <Veng/Assert.h>
#include <Veng/Log.h>
#include <Veng/Vendor/ImGui.h>

#include <Veng/Renderer/Backend/Buffer.h>
#include <Veng/Renderer/Backend/Command.h>
#include <Veng/Renderer/Backend/DescriptorSet.h>
#include <Veng/Renderer/Backend/DynamicGraphicsPipeline.h>
#include <Veng/Renderer/Backend/ImageView.h>
#include <Veng/Renderer/Backend/Sampler.h>
#include <Veng/Renderer/Backend/Shader.h>

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

        m_ImGuiImageView = Renderer::ImageView::Create({
            .Name = "Sample ImGui Image View",
            .Image = context.GetImGuiImage(),
        });

        m_Sampler = Renderer::Sampler::Create({
            .Name = "Sample Sampler",
            .AddressModeU = Renderer::AddressMode::ClampToEdge,
            .AddressModeV = Renderer::AddressMode::ClampToEdge,
            .AddressModeW = Renderer::AddressMode::ClampToEdge,
        });

        m_VertexBuffer = Renderer::Buffer::Create({
            .Name = "Triangle Vertices",
            .Size = sizeof(k_Vertices),
            .Usage = Renderer::BufferUsage::Vertex,
        });
        m_VertexBuffer->Upload({reinterpret_cast<const u8*>(k_Vertices), sizeof(k_Vertices)});

        CreateTrianglePipeline();
        CreateCompositePipeline();

        m_SceneTexture = context.CreateImGuiTexture(*m_Sampler, *m_SceneImageView);
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
            GetWindow().Close();
        }
    }

    void OnRender() override
    {
        auto& context = GetRenderContext();
        auto& cmd = *context.GetCurrentFrame().GetCommandBuffer();

        RenderScene(cmd);
        RenderUserInterface();
        context.RenderImGui(cmd);
        CompositeToSwapChain(cmd);
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
        m_VertexBuffer.reset();
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
                {.Stages = Renderer::ShaderStage::Vertex, .Offset = 0, .Size = sizeof(mat4)},
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
            .ColorAttachments = {{.Format = GetRenderContext().GetSwapChain().GetFormat()}},
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

        cmd.PipelineBarrier({
            .Image = *m_SceneImage,
            .NewLayout = vk::ImageLayout::eColorAttachmentOptimal,
        });

        cmd.BeginRendering({
            .Extent = extent,
            .ColorAttachments = {
                {
                    .ImageView = m_SceneImageView,
                    .LoadOp = Renderer::LoadOp::Clear,
                    .StoreOp = Renderer::StoreOp::Store,
                    .ClearValue = Renderer::ClearColor{0.05f, 0.05f, 0.08f, 1.0f},
                },
            },
        });

        cmd.BindPipeline(m_TrianglePipeline);
        cmd.SetViewport({0, 0}, extent);
        cmd.SetScissor({0, 0}, extent);
        cmd.BindVertexBuffer(m_VertexBuffer);

        const f32 aspect = static_cast<f32>(extent.x) / static_cast<f32>(extent.y);
        const mat4 transform = glm::scale(mat4(1.0f), vec3(1.0f / aspect, 1.0f, 1.0f)) *
            glm::rotate(mat4(1.0f), m_Angle, vec3(0.0f, 0.0f, 1.0f));

        cmd.PushConstants({
            .PipelineLayout = *m_TriangleLayout,
            .StageFlags = Renderer::ShaderStage::Vertex,
            .Offset = 0,
            .Size = sizeof(mat4),
            .Data = &transform,
        });

        cmd.Draw(3, 1, 0, 0);

        cmd.EndRendering();

        cmd.PipelineBarrier({
            .Image = *m_SceneImage,
            .NewLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        });
    }

    void RenderUserInterface() const
    {
        ImGui::Begin("Scene");
        const ImVec2 available = ImGui::GetContentRegionAvail();
        const f32 aspect = static_cast<f32>(m_SceneImage->GetHeight()) / static_cast<f32>(m_SceneImage->GetWidth());
        ImGui::Image(reinterpret_cast<ImTextureID>(m_SceneTexture->GetDescriptorSet()),
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
        const auto& swapChain = context.GetSwapChain();

        cmd.PipelineBarrier({
            .Image = *swapChain.GetCurrentImage(),
            .NewLayout = vk::ImageLayout::eColorAttachmentOptimal,
        });

        cmd.BeginRendering({
            .Extent = swapChain.GetExtent(),
            .ColorAttachments = {
                {
                    .ImageView = swapChain.GetCurrentImageView(),
                    .LoadOp = Renderer::LoadOp::Clear,
                    .StoreOp = Renderer::StoreOp::Store,
                    .ClearValue = Renderer::ClearColor{0.0f, 0.0f, 0.0f, 1.0f},
                },
            },
        });

        cmd.BindPipeline(m_CompositePipeline);
        cmd.SetViewport({0, 0}, swapChain.GetExtent());
        cmd.SetScissor({0, 0}, swapChain.GetExtent());
        cmd.BindDescriptorSets({.Sets = {m_CompositeSet}});
        cmd.DrawFullscreenTriangle();

        cmd.EndRendering();
    }

    Ref<Renderer::Image> m_SceneImage;
    Ref<Renderer::ImageView> m_SceneImageView;
    Ref<Renderer::ImageView> m_ImGuiImageView;
    Ref<Renderer::Sampler> m_Sampler;
    Ref<Renderer::Buffer> m_VertexBuffer;

    Ref<Renderer::PipelineLayout> m_TriangleLayout;
    Ref<Renderer::DynamicGraphicsPipeline> m_TrianglePipeline;

    Ref<Renderer::DescriptorSetLayout> m_CompositeSetLayout;
    Ref<Renderer::PipelineLayout> m_CompositeLayout;
    Ref<Renderer::DynamicGraphicsPipeline> m_CompositePipeline;
    Ref<Renderer::DescriptorSet> m_CompositeSet;

    Ref<Renderer::ImGuiTexture> m_SceneTexture;

    f32 m_Angle = 0.0f;
    u32 m_FrameCount = 0;
    const char* m_SmokeOutput = nullptr;
};

int main(const int argc, char** argv)
{
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
    });

    app.Run(vector<string>(argv, argv + argc));

    return 0;
}
