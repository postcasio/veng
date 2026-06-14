#pragma once

#include <functional>

#include <Veng/Veng.h>
#include <Veng/Renderer/Types.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>

namespace Veng
{
    class Window;
    class TaskSystem;
}

namespace Veng::Renderer
{
    class BindlessRegistry;
    class CommandBuffer;
    class Semaphore;
    class SynchronizationFrame;

    struct QueueFamilyIndices
    {
        optional<u32> GraphicsFamily;
        optional<u32> PresentFamily;

        // The family transfer uploads submit to. On a discrete GPU this is a
        // dedicated transfer-only family (the DMA path); on MoltenVK it collapses
        // to the graphics family. When TransferFamily == GraphicsFamily there is
        // no cross-queue ownership transfer and the submission lock alone
        // serializes the shared queue.
        optional<u32> TransferFamily;

        // Graphics is all a headless context needs; presentation is only
        // required when there's a surface (see CanPresent).
        [[nodiscard]] bool IsComplete() const
        {
            return GraphicsFamily.has_value();
        }

        [[nodiscard]] bool CanPresent() const
        {
            return PresentFamily.has_value();
        }

        // True when the transfer family is the graphics family (MoltenVK and any
        // GPU exposing no transfer-only family). Callers key cross-queue
        // ownership-transfer decisions off this.
        [[nodiscard]] bool TransferIsGraphics() const
        {
            return TransferFamily == GraphicsFamily;
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
        // created by the application before the context initializes. Pass
        // window == nullptr for a headless context (no surface or swapchain,
        // off-screen rendering only).
        void Initialize(const ContextInfo& info, Window* window);
        void DisposeResources();
        void Dispose();

        // Create one transfer command pool per worker, indexed by worker index,
        // by running pool creation on each worker once through ForEachWorker.
        // VkCommandPool is single-thread, so a worker may only ever touch its
        // own pool; storing the pools on the Context (not thread_local) ties
        // their lifetime to the device for clean teardown. Called by Application
        // after the TaskSystem is constructed and before any upload is submitted;
        // a headless/test Context with no TaskSystem simply has no transfer pools
        // (nothing submits transfers on that path). The pools are torn down in
        // Dispose(), by which point every worker has been joined.
        void InitializeTransferPools(TaskSystem& taskSystem);

        // True when initialized without a window (no surface/swapchain): the
        // swapchain accessors and present path are unavailable.
        [[nodiscard]] bool IsHeadless() const;

        [[nodiscard]] const QueueFamilyIndices& GetQueueFamilies() const;
        [[nodiscard]] Window& GetWindow() const { return *m_Window; }

        SynchronizationFrame& AcquireNextFrame();
        SynchronizationFrame& GetCurrentFrame();

        // Current frame's command buffer — the common case for recording.
        [[nodiscard]] CommandBuffer& GetCurrentCommandBuffer();

        // Acquires the next frame (and swapchain image, if not headless),
        // resets its fence, and begins recording its command buffer. The
        // returned reference is the same buffer GetCurrentCommandBuffer()
        // returns for the rest of the frame — callers don't need to hold onto
        // it.
        CommandBuffer& BeginFrame();

        // Transitions the swapchain image to present (if not headless), ends
        // recording, submits, and presents the current frame.
        void EndFrame();

        void SubmitFrame(const SynchronizationFrame& frame) const;
        void PresentFrame(const SynchronizationFrame& frame);
        void SubmitImmediateCommands(CommandBuffer& commandBuffer) const;

        // The transfer command buffer owned by the given worker's transfer pool.
        // VkCommandPool is single-thread: a worker may call this only with its
        // own index, and only the owning worker may record into the returned
        // buffer. The buffer is reused across uploads, so its reuse is
        // timeline-gated — a command buffer recorded for upload N must not be
        // reset or re-recorded until that upload's transfer-timeline value has
        // been reached (the GPU is done reading it); resetting it earlier
        // corrupts an in-flight submit. The owning worker enforces this by
        // waiting its own last transfer-timeline value before re-recording.
        // Requires InitializeTransferPools to have run.
        [[nodiscard]] CommandBuffer& GetTransferCommandBuffer(u32 workerIndex);
        [[nodiscard]] Format GetOutputFormat() const { return m_OutputFormat; }
        [[nodiscard]] Format GetDepthFormat() const { return m_DepthFormat; }

        void UpdateRenderExtent();

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

        // The global bindless descriptor registry (set 0). Valid from the end
        // of Initialize() until Dispose(). See BindlessRegistry.h.
        [[nodiscard]] BindlessRegistry& GetBindlessRegistry() const;

        struct Native;
        [[nodiscard]] Native& GetNative() const;

    private:
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
