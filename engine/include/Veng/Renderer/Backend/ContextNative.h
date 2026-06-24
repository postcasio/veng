#pragma once

// Out-of-line definition of Context::Native. Kept in its own header (rather
// than inline in Natives.h) because of its size: it absorbs every vk::/Vma
// handle the context owns, the deferred-destruction (retire bin) machinery,
// and the device-selection helpers that only run during Initialize().
// Internal-only: included by Natives.h.
#include <mutex>

#include <Veng/Renderer/Backend/Vulkan.h>
#include <Veng/Renderer/Backend/CommandPool.h>
#include <Veng/Renderer/Backend/DescriptorPool.h>
#include <Veng/Renderer/Backend/SwapChain.h>
#include <Veng/Renderer/Backend/SwapChainSupport.h>
#include <Veng/Renderer/Backend/SynchronizationFrame.h>

#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/TimelineSemaphore.h>

namespace Veng::Renderer
{
    struct Context::Native
    {
        vk::Instance Instance;
        vk::PhysicalDevice PhysicalDevice;
        vk::Device Device;
        vk::Queue GraphicsQueue;
        vk::Queue PresentQueue;
        /// @brief Queue transfer uploads submit to.
        ///
        /// On MoltenVK this is the same family as (and may be the same handle as)
        /// GraphicsQueue, so every submit to it serializes through SubmitMutex regardless.
        vk::Queue TransferQueue;
        vk::SurfaceKHR Surface;
        vk::DebugUtilsMessengerEXT DebugMessenger;
        vk::PipelineCache PipelineCache;
        /// @brief On-disk pipeline cache path, if persistence is enabled.
        ///
        /// When set, the cache is seeded from this file at init and written back at shutdown.
        /// nullopt keeps it in-memory only.
        optional<path> PipelineCachePath;
        VmaAllocator Allocator = nullptr;

        const vector<const char*> ValidationLayers =
            vector<const char*>({"VK_LAYER_KHRONOS_validation"});
        /// @brief Device extensions to enable.
        ///
        /// Mutable: the swapchain extension is dropped in headless mode (no
        /// window/surface), so this is finalized during Initialize().
        ///
        /// VK_KHR_portability_subset is NOT listed here: the spec requires it be enabled
        /// iff the device advertises it (a non-conformant implementation like MoltenVK).
        /// A conformant native driver (Windows/Linux desktop) does not expose it and would
        /// be wrongly rejected if it were always required, so CreateDevice() appends it
        /// per-device only when present.
        vector<const char*> DeviceExtensions = vector<const char*>(
            {VK_KHR_SWAPCHAIN_EXTENSION_NAME,
             VK_EXT_SHADER_VIEWPORT_INDEX_LAYER_EXTENSION_NAME,
             VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME});
        vector<const char*> RequiredExtensions;

        /// @brief When true, no window, surface, or swapchain — off-screen rendering only.
        bool Headless = false;

        Unique<SwapChain> SwapChain;
        Unique<CommandPool> CommandPool;
        Unique<DescriptorPool> DescriptorPool;
        Unique<BindlessRegistry> Bindless;

        /// @brief One transfer command pool per worker, indexed by worker index.
        ///
        /// VkCommandPool is not shareable across threads; each worker records its uploads
        /// into its own pool/buffer and only ever touches the entry at its own index.
        struct TransferPool
        {
            vk::CommandPool Pool;
            Ref<CommandBuffer> CommandBuffer;

            /// @brief Transfer-timeline value the last upload from this worker signalled.
            ///
            /// The command buffer is reused across uploads; a worker must wait this value
            /// before resetting and re-recording — resetting it earlier corrupts an in-flight
            /// submit. Zero before the first upload.
            u64 LastSubmittedValue = 0;
        };

        vector<TransferPool> TransferPools;

        /// @brief Override pool for CommandBuffer allocation.
        ///
        /// When non-null, CommandBuffer's constructor allocates from this pool instead of
        /// the shared graphics CommandPool. Set transiently while allocating a worker's
        /// transfer command buffer in InitializeTransferPools.
        vk::CommandPool AllocationPoolOverride;

        /// @brief Serializes every vkQueueSubmit to a shared queue.
        ///
        /// vkQueueSubmit is not thread-safe per VkQueue, and on MoltenVK the transfer and
        /// graphics queues share a family (and may be one handle), so worker and main-thread
        /// submits must serialize through this.
        std::mutex SubmitMutex;

        /// @brief Transfer timeline semaphore.
        ///
        /// Worker upload submits signal a strictly increasing value on it; the graphics frame
        /// submit and the transfer-keyed retire drain wait/poll it.
        Unique<TimelineSemaphore> TransferTimeline;

        /// @brief Monotonic counter for transfer-timeline signal values.
        ///
        /// A timeline semaphore must signal strictly increasing values, so the next value is
        /// allocated under SubmitMutex immediately before the submit — never precomputed
        /// and raced for the lock, which could submit values out of order. Pre-increment
        /// makes the first signalled value 1 (0 is the timeline's initial value).
        u64 TransferTimelineValue = 0;

        /// @brief Transfer-timeline waits the next frame submit must fold in.
        ///
        /// A resource sampled in a frame after an async upload registers its
        /// (semaphore, value) here; SubmitFrame chains them onto the submit and clears
        /// the set. Guarded by SubmitMutex.
        vector<std::pair<vk::Semaphore, u64>> PendingFrameTransferWaits;

        /// @brief Serializes Retire, RetireOnTransfer, the transfer-retire list, and their drains.
        ///
        /// A worker dropping upload scratch (via RetireOnTransfer) must not race the main
        /// thread's CurrentFrameInFlight read, bin vector reallocation, or the transfer-list drain.
        std::mutex RetireMutex;

        /// @brief Raw allocations whose GPU lifetime is the transfer timeline, not a frame fence.
        ///
        /// Worker-created upload scratch is released into here keyed on the timeline value its
        /// copy signals; the main thread destroys each entry once TransferTimeline reaches that value.
        struct TransferRetireEntry
        {
            vk::Buffer Buffer;
            VmaAllocation Allocation;
            u64 TimelineValue;
        };

        vector<TransferRetireEntry> TransferRetireList;

        vector<SynchronizationFrame> SynchronizationFrames;
        u32 CurrentFrameInFlight = 0;
        u32 MaxFramesInFlight = 2;

        /// @brief Timestamp query pool holding a (start, end) pair per frame-in-flight.
        ///
        /// Query 2·slot is written top-of-pipe at BeginFrame, 2·slot+1 bottom-of-pipe at
        /// EndFrame; the pair is read back when that slot's frame next retires. Null when the
        /// device reports no timestamp support.
        vk::QueryPool TimestampPool;
        /// @brief Nanoseconds per timestamp tick (PhysicalDeviceLimits::timestampPeriod).
        f32 TimestampPeriodNs = 0.0f;
        /// @brief Mask of the meaningful low bits of a timestamp (from timestampValidBits).
        ///
        /// A device may report fewer than 64 valid bits; the raw query values and their
        /// difference are masked to this before converting to time.
        u64 TimestampValidMask = ~0ULL;
        /// @brief Per-slot flag: true once a slot's query pair has been written at least once.
        ///
        /// Guards the readback so a slot is never read before its first frame wrote it.
        vector<bool> TimestampWritten;

        /// @brief Per-in-flight-frame deferred-destruction bins.
        ///
        /// A handle retired while frame i is recording goes into bin i and is protected
        /// by frame i's fence, destroyed when AcquireNextFrame next waits that fence.
        /// Retirements outside a frame (init, post-WaitIdle) are caught by the shutdown
        /// drain-all in DisposeResources/Dispose.
        struct RetireBin
        {
            vector<std::pair<vk::Buffer, VmaAllocation>> Buffers;
            vector<std::pair<vk::Image, VmaAllocation>> Images;
            vector<vk::ImageView> ImageViews;
            vector<vk::Sampler> Samplers;
            vector<vk::ShaderModule> ShaderModules;
            vector<vk::Pipeline> Pipelines;
            vector<vk::PipelineLayout> PipelineLayouts;
            /// @brief Descriptor sets freed back to the descriptor pool.
            vector<vk::DescriptorSet> DescriptorSets;
        };

        vector<RetireBin> RetireBins;
        bool Disposed = false;

        RetireBin& CurrentRetireBin();
        void DrainRetireBin(RetireBin& bin);
        void DrainAllRetireBins();

        /// @brief Defers destruction of a handle into the current frame's retire bin.
        ///
        /// The handle is destroyed only after the frame's in-flight fence is waited again
        /// (see AcquireNextFrame), so dropping a resource mid-frame is always safe.
        void Retire(vk::Buffer buffer, VmaAllocation allocation);
        void Retire(vk::Image image, VmaAllocation allocation);
        void Retire(vk::ImageView imageView);
        void Retire(vk::Sampler sampler);
        void Retire(vk::ShaderModule shaderModule);
        void Retire(vk::Pipeline pipeline);
        void Retire(vk::PipelineLayout pipelineLayout);
        void Retire(vk::DescriptorSet descriptorSet);

        /// @brief Takes ownership of a raw allocation whose GPU lifetime is the transfer timeline.
        ///
        /// Reclaimed (vmaDestroyBuffer) once TransferTimeline reaches @p timelineValue.
        /// The caller passes the released raw handle, not a Buffer: routing it through
        /// Buffer::~Buffer would re-defer it into a frame bin, pinning it to the wrong lifetime.
        void RetireOnTransfer(vk::Buffer buffer, VmaAllocation allocation, u64 timelineValue);

        /// @brief Destroys every transfer-retire entry whose timeline value has been reached.
        ///
        /// Holds RetireMutex across the GetValue() read and the erase.
        void DrainTransferRetireList();

        /// @brief Submits to @p queue under SubmitMutex.
        ///
        /// vkQueueSubmit is not thread-safe per VkQueue; every submit (frame, immediate,
        /// and transfer) funnels through here so worker and main-thread submits to a
        /// collapsed MoltenVK queue cannot race.
        void LockedSubmit(vk::Queue queue, const vk::SubmitInfo& submitInfo, vk::Fence fence);

        vector<const char*>& GetRequiredExtensions();
        vk::PhysicalDevice GetPhysicalDevice();
        bool IsDeviceSuitable(vk::PhysicalDevice device);
        QueueFamilyIndices& FindQueueFamilies(vk::PhysicalDevice device);
        [[nodiscard]] SwapChainSupportDetails
        QuerySwapChainSupport(vk::PhysicalDevice device) const;
        [[nodiscard]] bool CheckDeviceExtensionSupport(vk::PhysicalDevice device) const;
        vk::Device CreateDevice();

        QueueFamilyIndices QueueFamilies{};

        /// @brief True when both multiDrawIndirect and drawIndirectFirstInstance were
        /// enabled at device creation.
        ///
        /// The GPU-driven cull path (multiDrawIndirect command buffer + candidate index
        /// in firstInstance) is available only when both are present; set in CreateDevice.
        bool GpuDrivenCullingSupported = false;
    };
}
