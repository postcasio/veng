#pragma once

#include <functional>

#include <Veng/Veng.h>
#include <Veng/Renderer/Types.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>

namespace Veng
{
    class Window;
}

namespace Veng::Renderer
{
    class CommandBuffer;
    class Semaphore;
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

        // Current swap chain image/view and extent/format, for compositing.
        [[nodiscard]] uvec2 GetSwapChainExtent() const;
        [[nodiscard]] Format GetSwapChainFormat() const;
        [[nodiscard]] Ref<Image> GetCurrentSwapChainImage() const;
        [[nodiscard]] Ref<ImageView> GetCurrentSwapChainImageView() const;
        [[nodiscard]] u32 GetSwapChainImageCount() const;
        [[nodiscard]] u32 GetCurrentSwapChainImageIndex() const;

        // Register a callback fired after the swap chain is recreated (resize).
        // The ImGui layer uses this to recreate its offscreen target.
        void AddSwapChainInvalidationCallback(std::function<void()> callback);

        void ImmediateCommands(const std::function<void(CommandBuffer&)>& function) const;
        void AcquireNextImage(Semaphore& semaphore);
        void WaitIdle() const;

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

        bool m_RenderExtentChanged = false;

        Unique<Native> m_Native;
    };
}
