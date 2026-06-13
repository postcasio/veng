#pragma once

#include <functional>

#include <Veng/Veng.h>
#include <Veng/Renderer/Types.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/Framebuffer.h>
#include <Veng/Renderer/RenderPass.h>
#include <Veng/Renderer/ImGuiTexture.h>

namespace Veng
{
    class Window;
}

namespace Veng::Renderer
{
    class CommandBuffer;
    class Semaphore;
    class Sampler;
    class SynchronizationFrame;

    struct QueueFamilyIndices
    {
        optional<u32> GraphicsFamily;
        optional<u32> PresentFamily;

        [[nodiscard]] bool IsComplete() const
        {
            return GraphicsFamily.has_value() && PresentFamily.has_value();
        }
    };

    struct ContextInfo
    {
        string ApplicationName;
        string EngineName = "Veng";
        uvec2 InternalRenderExtent;
        // Fonts live here until ImGui is extracted into its own module.
        optional<path> DefaultFontPath;
        optional<path> IconFontPath;
        Format OutputFormat = Format::RGBA16Sfloat;
        Format DepthFormat = Format::D32Sfloat;
    };

    class Context
    {
    public:
        Context();
        ~Context();

        // The window is borrowed, not owned; it must outlive the context and is
        // created by the application before the context initializes.
        void Initialize(const ContextInfo& info, Window* window);
        void DisposeResources();
        void Dispose();

        [[nodiscard]] const QueueFamilyIndices& GetQueueFamilies() const;
        [[nodiscard]] Window& GetWindow() const { return *m_Window; }

        SynchronizationFrame& AcquireNextFrame();
        SynchronizationFrame& GetCurrentFrame();

        // Current frame's command buffer — the common case for recording.
        [[nodiscard]] CommandBuffer& GetCurrentCommandBuffer();

        void SubmitFrame(const SynchronizationFrame& frame) const;
        void PresentFrame(const SynchronizationFrame& frame);
        void SubmitImmediateCommands(CommandBuffer& commandBuffer) const;
        [[nodiscard]] Format GetOutputFormat() const { return m_OutputFormat; }
        [[nodiscard]] Format GetDepthFormat() const { return m_DepthFormat; }

        void UpdateRenderExtent();

        static Context& Instance();

        [[nodiscard]] uvec2 GetInternalRenderExtent() const { return m_InternalRenderExtent; }
        [[nodiscard]] uvec2 GetRenderExtent() const { return m_RenderExtent; }

        [[nodiscard]] u32 GetMaxFramesInFlight() const;
        [[nodiscard]] u32 GetCurrentFrameInFlight() const;

        [[nodiscard]] Ref<Image> GetImGuiImage() const { return m_ImGuiImage; }

        // Current swap chain image/view and extent/format, for compositing.
        [[nodiscard]] uvec2 GetSwapChainExtent() const;
        [[nodiscard]] Format GetSwapChainFormat() const;
        [[nodiscard]] Ref<Image> GetCurrentSwapChainImage() const;
        [[nodiscard]] Ref<ImageView> GetCurrentSwapChainImageView() const;

        void ImmediateCommands(const std::function<void(CommandBuffer&)>& function) const;
        void AcquireNextImage(Semaphore& semaphore);
        void WaitIdle() const;
        void RenderImGui(CommandBuffer& commandBuffer);
        void BeginFrame();
        Ref<ImGuiTexture> CreateImGuiTexture(const Sampler& sampler, const ImageView& imageView);
        void DestroyImGuiTexture(const ImGuiTexture& texture);

        struct Native;
        [[nodiscard]] Native& GetNative() const;

    private:
        static inline Context* s_Instance = nullptr;

        // Borrowed from the application in Initialize(); never owned.
        Window* m_Window = nullptr;

        uvec2 m_InternalRenderExtent;
        uvec2 m_RenderExtent;
        Format m_OutputFormat = Format::RGBA16Sfloat;
        Format m_DepthFormat = Format::D32Sfloat;
        bool m_ImGuiRenderedThisFrame = true;

        Ref<RenderPass> m_ImGuiRenderPass;
        vector<Ref<Framebuffer>> m_ImGuiFramebuffers;
        Ref<Image> m_ImGuiImage;
        Ref<ImageView> m_ImGuiImageView;

        bool m_RenderExtentChanged = false;

        Unique<Native> m_Native;

        void CreateImGuiResources();
        void DisposeImGuiResources();
    };
}
