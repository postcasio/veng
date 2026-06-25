#pragma once

#include <functional>
#include <span>

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
    class TimelineSemaphore;

    /// @brief Per-device queue family indices resolved at initialization.
    struct QueueFamilyIndices
    {
        /// @brief Graphics (and compute) queue family.
        optional<u32> GraphicsFamily;
        /// @brief Presentation queue family.
        optional<u32> PresentFamily;

        /// @brief The family transfer uploads submit to.
        ///
        /// On a discrete GPU this is a dedicated transfer-only family (the DMA path);
        /// on MoltenVK it collapses to the graphics family. When TransferFamily ==
        /// GraphicsFamily there is no cross-queue ownership transfer and the submission
        /// lock alone serializes the shared queue.
        optional<u32> TransferFamily;

        /// @brief Returns true when the graphics family is present.
        ///
        /// Graphics is all a headless context needs; presentation is only required when
        /// there is a surface (see CanPresent).
        [[nodiscard]] bool IsComplete() const { return GraphicsFamily.has_value(); }

        /// @brief Returns true when a presentation queue family is available.
        [[nodiscard]] bool CanPresent() const { return PresentFamily.has_value(); }

        /// @brief Returns true when the transfer family is the graphics family.
        ///
        /// True on MoltenVK and any GPU exposing no transfer-only family. Callers key
        /// cross-queue ownership-transfer decisions off this.
        [[nodiscard]] bool TransferIsGraphics() const { return TransferFamily == GraphicsFamily; }
    };

    /// @brief Creation parameters for a render context.
    struct ContextInfo
    {
        /// @brief Application name for the Vulkan instance.
        string ApplicationName;
        /// @brief Engine name for the Vulkan instance.
        string EngineName = "Veng";
        /// @brief Off-screen render-target extent used only when headless (window == nullptr).
        ///
        /// A headless context has no swapchain to derive a render-target extent from; this is
        /// the extent GetRenderExtent() reports. Ignored when a window is present.
        uvec2 HeadlessExtent;
        /// @brief Color format of the off-screen output target.
        Format OutputFormat = Format::RGBA16Sfloat;
        /// @brief Depth format of the off-screen depth target.
        Format DepthFormat = Format::D32Sfloat;
        /// @brief Requested display output mode for the swapchain (a preference; see DisplayMode).
        DisplayMode RequestedDisplayMode = DisplayMode::Auto;

        /// @brief When set, the pipeline cache is seeded from this file at init and written back
        /// at shutdown. nullopt keeps it in-memory only.
        optional<path> PipelineCachePath;
    };

    /// @brief The Vulkan device context: owns the instance, device, queues, swap chain,
    /// frame synchronization, transfer pools, and the bindless registry.
    class Context
    {
    public:
        /// @brief Constructs a context; call Initialize to set up Vulkan.
        Context();
        /// @brief Destructor; all GPU resources must be released before this runs.
        ~Context();

        /// @brief Initializes the Vulkan instance, device, and swap chain.
        ///
        /// The window is borrowed, not owned; it must outlive the context and is created
        /// by the application before the context initializes. Pass window == nullptr for
        /// a headless context (no surface or swapchain, off-screen rendering only).
        /// @param info   Context creation parameters.
        /// @param window Borrowed window, or nullptr for headless.
        void Initialize(const ContextInfo& info, Window* window);

        /// @brief Releases all GPU resources held by the context.
        void DisposeResources();

        /// @brief Tears down the Vulkan instance and device.
        void Dispose();

        /// @brief Creates one transfer command pool per worker, indexed by worker index.
        ///
        /// VkCommandPool is single-thread, so a worker may only ever touch its own pool;
        /// storing the pools on the Context (not thread_local) ties their lifetime to the
        /// device for clean teardown. Called by Application after the TaskSystem is
        /// constructed and before any upload is submitted; a headless/test Context with no
        /// TaskSystem simply has no transfer pools. The pools are torn down in Dispose(),
        /// by which point every worker has been joined.
        /// @param taskSystem The application's task system.
        void InitializeTransferPools(TaskSystem& taskSystem);

        /// @brief Returns true when initialized without a window (no surface/swapchain).
        ///
        /// The swapchain accessors and present path are unavailable in headless mode.
        [[nodiscard]] bool IsHeadless() const;

        /// @brief Returns the resolved queue family indices.
        [[nodiscard]] const QueueFamilyIndices& GetQueueFamilies() const;

        /// @brief Returns true when GPU-driven culling is supported by the device.
        ///
        /// True only when both multiDrawIndirect and drawIndirectFirstInstance were
        /// enabled at device creation — the features the indirect-draw cull path needs.
        /// CullMode::GPU is unavailable otherwise and the CPU path is the fallback.
        [[nodiscard]] bool IsGpuDrivenCullingSupported() const;

        /// @brief Returns true when BC block-compressed formats can be sampled by the device.
        ///
        /// True only when textureCompressionBC was supported by the physical device and was
        /// therefore enabled at device creation — a core VkPhysicalDeviceFeatures boolean that
        /// must be enabled for sampling a BC image to be legal, not merely queried. MoltenVK
        /// exposes it on Apple Silicon; an Intel-Mac / non-BC device reports false, and the
        /// texture loader returns AssetError::Unsupported for a BC-cooked texture there.
        [[nodiscard]] bool IsBlockCompressionSupported() const;

        /// @brief Returns true when the graphics queue can timestamp GPU work.
        ///
        /// True when the graphics queue family reports a non-zero timestampValidBits and the
        /// device exposes a non-zero timestampPeriod — the requirements for the per-frame
        /// timestamp-query pair that measures GPU frame time. False on a device without
        /// timestamp support, where GetLastGpuFrameTimeMs() stays zero and dynamic-resolution
        /// control is inert.
        [[nodiscard]] bool IsGpuTimingSupported() const;

        /// @brief Returns the most recently completed frame's GPU execution time, in milliseconds.
        ///
        /// A timestamp-query pair brackets each frame's command buffer; the value is read back
        /// once the frame retires (its fence is waited again, GetMaxFramesInFlight() frames
        /// later), so it is the freshest fully-completed GPU frame time. The adaptive
        /// resolution controller drives off it. Zero before the first measurement and whenever
        /// IsGpuTimingSupported() is false.
        /// @return The last completed frame's GPU time in ms, or 0 when unavailable.
        [[nodiscard]] f32 GetLastGpuFrameTimeMs() const;

        /// @brief One render pass's measured GPU duration for the last completed frame.
        struct GpuPassTiming
        {
            /// @brief The pass's name, as declared on the RenderGraph.
            string Name;
            /// @brief The pass's GPU duration in milliseconds.
            f32 Milliseconds = 0.0f;
        };

        /// @brief Returns the per-pass GPU durations measured for the last completed frame.
        ///
        /// One entry per timestamp scope bracketed during the frame, in execution order across
        /// every CompiledGraph::Execute of the frame (scene render, then the gather/composite
        /// tail). Each pass is bracketed by a timestamp pair around its GPU work, so a duration
        /// includes the pass's own barrier waits. Empty before the first measurement, whenever
        /// IsGpuTimingSupported() is false, and on a frame whose scope count exceeded the
        /// per-frame budget (the surplus scopes go unmeasured). The span is valid until the
        /// next BeginFrame.
        /// @return The last completed frame's per-pass timings, in execution order.
        [[nodiscard]] std::span<const GpuPassTiming> GetLastGpuPassTimings() const;

        /// @brief Opens a named GPU timestamp scope, recording a begin timestamp into @p cmd.
        ///
        /// The RenderGraph brackets every pass with a scope; a caller recording raw passes
        /// outside the graph may bracket its own work the same way. Scopes may nest and must be
        /// balanced by an EndGpuScope on the same command buffer. A no-op when
        /// IsGpuTimingSupported() is false or the per-frame scope budget is exhausted.
        /// @param cmd   Command buffer the begin timestamp is recorded into.
        /// @param name  Label paired with this scope's duration in GetLastGpuPassTimings().
        void BeginGpuScope(CommandBuffer& cmd, string_view name);

        /// @brief Closes the most recently opened GPU timestamp scope, recording its end timestamp.
        /// @pre A matching BeginGpuScope is open — asserted otherwise.
        /// @param cmd  Command buffer the end timestamp is recorded into.
        void EndGpuScope(CommandBuffer& cmd);

        /// @brief Returns true when the device can linearly filter a sampled image of @p format.
        ///
        /// Queries the format's optimal-tiling feature flags for
        /// SampledImageFilterLinear — the capability a bilinear Sample() of that format
        /// needs. The bloom pyramid asserts it for its HDR format, since point Load()
        /// reductions (hi-Z) never exercise it.
        /// @param format The image format to query.
        [[nodiscard]] bool IsFormatLinearFilterSupported(Format format) const;

        /// @brief Returns the borrowed window.
        [[nodiscard]] Window& GetWindow() const { return *m_Window; }

        /// @brief Advances to the next frame-in-flight, waiting the outgoing frame's fence.
        SynchronizationFrame& AcquireNextFrame();

        /// @brief Returns the synchronization frame that is currently being recorded.
        SynchronizationFrame& GetCurrentFrame();

        /// @brief Returns the current frame's command buffer — the common case for recording.
        [[nodiscard]] CommandBuffer& GetCurrentCommandBuffer();

        /// @brief Acquires the next frame (and swapchain image, if not headless), resets its
        /// fence, and begins recording its command buffer.
        ///
        /// The returned reference is the same buffer GetCurrentCommandBuffer() returns for
        /// the rest of the frame — callers don't need to hold onto it.
        /// @return The frame's command buffer, ready to record.
        CommandBuffer& BeginFrame();

        /// @brief Ends recording, submits, and presents the current frame.
        ///
        /// Transitions the swapchain image to present first (if not headless).
        void EndFrame();

        /// @brief Submits the given synchronization frame's command buffer to the graphics queue.
        void SubmitFrame(const SynchronizationFrame& frame) const;

        /// @brief Presents the currently acquired swapchain image and advances the frame index.
        void PresentFrame();

        /// @brief Registers a transfer-timeline wait the next frame submit must satisfy.
        ///
        /// The frame's graphics submit waits timeline >= value at the fragment-shader
        /// (sampled-image) stage. The render side calls this on first graphics use of a
        /// just-uploaded resource; SubmitFrame folds every accumulated wait in and clears
        /// the set each frame. Rides alongside the binary present/acquire semaphores,
        /// never replacing them.
        /// @param timeline The transfer timeline to wait.
        /// @param value    The timeline value to wait for.
        void AddFrameTransferWait(const TimelineSemaphore& timeline, u64 value);

        /// @brief Submits `commandBuffer` immediately and waits for it to complete.
        void SubmitImmediateCommands(CommandBuffer& commandBuffer) const;

        /// @brief Prepares a worker's transfer command buffer for recording a new upload.
        ///
        /// First waits the worker's last submitted transfer-timeline value (the GPU is done
        /// reading it), then resets and begins the buffer, returning it ready to record.
        /// Only the owning worker may call this with its own index.
        /// @param workerIndex The calling worker's index.
        /// @return The transfer command buffer, ready to record.
        [[nodiscard]] CommandBuffer& BeginTransferRecording(u32 workerIndex);

        /// @brief Ends and submits a worker's transfer command buffer on the transfer queue.
        ///
        /// The next monotonic timeline value is allocated inside the submission lock (never
        /// precomputed and raced for the lock), recorded as the worker's last submitted value,
        /// and returned. The caller uses the returned value for the staging buffer's
        /// transfer-keyed retire and the render side's frame transfer-wait. No device wait.
        /// Only the owning worker may call this with its own index.
        /// @param workerIndex The calling worker's index.
        /// @param timeline    The timeline to signal.
        /// @return The timeline value signalled by this submit.
        [[nodiscard]] u64 SubmitTransfer(u32 workerIndex, const TimelineSemaphore& timeline);

        /// @brief The context-owned transfer timeline that async uploads signal.
        ///
        /// Worker copies signal a strictly increasing value on it (via SubmitTransfer);
        /// the frame submit and the transfer-keyed retire wait/poll it.
        [[nodiscard]] TimelineSemaphore& GetTransferTimeline() const;

        /// @brief The transfer command buffer owned by the given worker's transfer pool.
        ///
        /// VkCommandPool is single-thread: a worker may call this only with its own index,
        /// and only the owning worker may record into the returned buffer. The buffer is
        /// reused across uploads, so its reuse is timeline-gated — a command buffer
        /// recorded for upload N must not be reset or re-recorded until that upload's
        /// transfer-timeline value has been reached (the GPU is done reading it). The
        /// owning worker enforces this by waiting its own last transfer-timeline value
        /// before re-recording. Requires InitializeTransferPools to have run.
        /// @param workerIndex The calling worker's index.
        /// @return The worker's transfer command buffer.
        [[nodiscard]] CommandBuffer& GetTransferCommandBuffer(u32 workerIndex);

        /// @brief Returns the context's off-screen output format.
        [[nodiscard]] Format GetOutputFormat() const { return m_OutputFormat; }

        /// @brief Returns the context's depth buffer format.
        [[nodiscard]] Format GetDepthFormat() const { return m_DepthFormat; }

        /// @brief Updates the render extent to match the current window size.
        void UpdateRenderExtent();

        /// @brief Returns the window render-target extent.
        ///
        /// The swapchain framebuffer extent when windowed; the configured ContextInfo::HeadlessExtent
        /// when headless. Per-viewport render resolution is a Viewport concern, not this.
        [[nodiscard]] uvec2 GetRenderExtent() const { return m_RenderExtent; }

        /// @brief Returns the maximum number of frames that may be in flight simultaneously.
        [[nodiscard]] u32 GetMaxFramesInFlight() const;

        /// @brief Returns the index of the current frame-in-flight (0 .. GetMaxFramesInFlight()-1).
        [[nodiscard]] u32 GetCurrentFrameInFlight() const;

        /// @brief The maximum width/height of a 2D image this device supports.
        ///
        /// The physical device's @c maxImageDimension2D limit (16384 on a typical
        /// Metal device via MoltenVK). A consumer sizing an image — notably a tiled
        /// atlas whose extent is a multiple of a per-tile resolution — must keep its
        /// extent within this bound; exceeding it is a fatal driver-side error.
        /// @return The device's maximum 2D image edge length, in texels.
        [[nodiscard]] u32 GetMaxImageDimension2D() const;

        /// @brief Returns the current swap chain extent.
        [[nodiscard]] uvec2 GetSwapChainExtent() const;

        /// @brief Returns the swap chain's surface format.
        [[nodiscard]] Format GetSwapChainFormat() const;

        /// @brief Returns the display output mode actually resolved against device support.
        ///
        /// May differ from ContextInfo::RequestedDisplayMode when an HDR mode was
        /// unavailable and selection fell back to SDR.
        [[nodiscard]] DisplayMode GetActiveDisplayMode() const;

        /// @brief Returns the resolved color space of the presentable swapchain images.
        [[nodiscard]] DisplayColorSpace GetActiveDisplayColorSpace() const;

        /// @brief Returns the swap chain image for the current frame.
        [[nodiscard]] Ref<Image> GetCurrentSwapChainImage() const;

        /// @brief Returns the swap chain image view for the current frame.
        [[nodiscard]] Ref<ImageView> GetCurrentSwapChainImageView() const;

        /// @brief Returns the total number of swap chain images.
        [[nodiscard]] u32 GetSwapChainImageCount() const;

        /// @brief Returns the index of the current swap chain image.
        [[nodiscard]] u32 GetCurrentSwapChainImageIndex() const;

        /// @brief Registers a callback fired after the swap chain is recreated (e.g. on resize).
        ///
        /// The ImGui layer uses this to recreate its offscreen target.
        /// @param callback The function to call after swap chain recreation.
        void AddSwapChainInvalidationCallback(std::function<void()> callback);

        /// @brief Records commands via a callback on a one-shot command buffer and waits for completion.
        void ImmediateCommands(const std::function<void(CommandBuffer&)>& function) const;

        /// @brief Acquires the next swap chain image, signalling `semaphore` when available.
        void AcquireNextImage(Semaphore& semaphore);

        /// @brief Blocks until the device is idle.
        void WaitIdle() const;

        /// @brief Returns the global bindless descriptor registry (set 0).
        ///
        /// Valid from the end of Initialize() until Dispose(). See BindlessRegistry.h.
        [[nodiscard]] BindlessRegistry& GetBindlessRegistry() const;

        /// @brief Queues a one-time graphics-queue acquire and shader-read transition for a
        /// bindless-sampled resource that has just gone resident.
        ///
        /// A texture sampled through set 0 is invisible to the RenderGraph, so the graph
        /// cannot derive its layout transition or fold its upload's transfer-timeline wait
        /// into the submit. The resident half of the async upload (Texture::Finalize)
        /// enqueues the view here; BeginFrame drains the queue into the frame's command
        /// buffer before any pass records, so the resource is in shader-read layout and
        /// graphics-owned by the time anything samples it. The transition is recorded once,
        /// on the first frame after the resource becomes resident. Called on the main thread
        /// only (same thread as BeginFrame); not synchronized.
        /// @param view The image view to transition.
        void EnqueueBindlessAcquire(const Ref<ImageView>& view);

        /// @brief Opaque backend handle; defined in the matching .cpp.
        struct Native;
        /// @brief Returns the backend handle. Mutable ref from a const method by design — see Native.h.
        [[nodiscard]] Native& GetNative() const;

    private:
        /// @brief Borrowed from the application in Initialize(); never owned.
        Window* m_Window = nullptr;

        /// @brief Window render-target extent (swapchain framebuffer extent, or HeadlessExtent headless).
        uvec2 m_RenderExtent;
        /// @brief Off-screen output format.
        Format m_OutputFormat = Format::RGBA16Sfloat;
        /// @brief Depth buffer format.
        Format m_DepthFormat = Format::D32Sfloat;

        /// @brief Set when a resize is pending; triggers swapchain recreation at the next PresentFrame.
        bool m_RenderExtentChanged = false;

        /// @brief True when the graphics queue supports timestamp queries (set in Initialize).
        bool m_GpuTimingSupported = false;

        /// @brief Most recently completed frame's GPU time in ms; 0 until the first readback.
        ///
        /// Written by BeginFrame from the timestamp-query pair of the frame that just retired.
        f32 m_GpuFrameTimeMs = 0.0f;

        /// @brief Per-pass GPU durations of the frame that just retired, in execution order.
        ///
        /// Rebuilt by BeginFrame from the retiring slot's scope timestamps, paired with the
        /// pass names recorded into that slot. Returned by GetLastGpuPassTimings().
        vector<GpuPassTiming> m_GpuPassTimings;

        /// @brief True between BeginFrame and EndFrame, when the frame's query run is reset.
        ///
        /// Gates BeginGpuScope/EndGpuScope: a graph executed outside the frame loop (an
        /// ImmediateCommands render, a one-shot offscreen render) writes no timestamps, since
        /// its queries were never reset. Per-pass timing covers only the driven frame.
        bool m_GpuScopeRecording = false;

        /// @brief Bindless-sampled resources awaiting their one-time graphics-queue acquire.
        ///
        /// Populated by EnqueueBindlessAcquire and drained by BeginFrame. Holds a Ref so a
        /// view enqueued for a texture dropped before the next frame cannot dangle.
        vector<Ref<ImageView>> m_PendingBindlessAcquires;

        /// @brief Backend Vulkan state (instance, device, queues, swapchain, sync frames).
        Unique<Native> m_Native;
    };
}
