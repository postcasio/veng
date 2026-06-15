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
        // The queue transfer uploads submit to. On MoltenVK this is the same
        // family as (and may be the same handle as) GraphicsQueue, so every
        // submit to it serializes through SubmitMutex regardless.
        vk::Queue TransferQueue;
        vk::SurfaceKHR Surface;
        vk::DebugUtilsMessengerEXT DebugMessenger;
        vk::PipelineCache PipelineCache;
        // When set, the pipeline cache is seeded from this file at init and
        // written back at shutdown. nullopt keeps it in-memory only.
        optional<path> PipelineCachePath;
        VmaAllocator Allocator = nullptr;

        const vector<const char*> ValidationLayers = vector<const char*>({"VK_LAYER_KHRONOS_validation"});
        // Mutable: the swapchain extension is dropped in headless mode (no
        // window/surface), so this is finalized during Initialize().
        vector<const char*> DeviceExtensions = vector<const char*>({
            VK_KHR_SWAPCHAIN_EXTENSION_NAME,
            VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME,
            VK_EXT_SHADER_VIEWPORT_INDEX_LAYER_EXTENSION_NAME,
            VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,
            VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME
        });
        vector<const char*> RequiredExtensions;

        // Headless: no window, surface or swapchain — off-screen rendering only.
        bool Headless = false;

        Unique<SwapChain> SwapChain;
        Unique<CommandPool> CommandPool;
        Unique<DescriptorPool> DescriptorPool;
        Unique<BindlessRegistry> Bindless;

        // One transfer command pool per worker, indexed by worker index.
        // VkCommandPool is not shareable across threads, so each worker records
        // its uploads into its own pool/buffer; a worker only ever touches the
        // entry at its own index. Created once via TaskSystem::ForEachWorker in
        // InitializeTransferPools, destroyed in Dispose after the workers join.
        struct TransferPool
        {
            vk::CommandPool Pool;
            Ref<CommandBuffer> CommandBuffer;

            // The transfer-timeline value the last upload submitted from this
            // worker signalled. The command buffer is reused across uploads, so a
            // worker must wait this value (the GPU is done reading the buffer)
            // before resetting and re-recording — resetting it earlier corrupts an
            // in-flight submit. Zero before the first upload.
            u64 LastSubmittedValue = 0;
        };

        vector<TransferPool> TransferPools{};

        // When non-null, CommandBuffer's constructor allocates from this pool
        // instead of the shared graphics CommandPool. Set transiently while
        // allocating a worker's transfer command buffer in InitializeTransferPools.
        vk::CommandPool AllocationPoolOverride{};

        // Serializes every vkQueueSubmit to a shared queue. vkQueueSubmit is not
        // thread-safe per VkQueue, and on MoltenVK the transfer and graphics
        // queues share a family (and may be one handle), so a worker's transfer
        // submit and the main thread's frame submit must serialize through this.
        std::mutex SubmitMutex;

        // The transfer timeline. Worker upload submits signal a strictly
        // increasing value on it; the graphics frame submit and the
        // transfer-keyed retire drain wait/poll it. One per Context, created in
        // Initialize alongside the queues.
        Unique<TimelineSemaphore> TransferTimeline;

        // The monotonic transfer-timeline value. A timeline semaphore must signal
        // strictly increasing values, so the next value is allocated *under*
        // SubmitMutex immediately before the submit — never precomputed and raced
        // for the lock, which could submit values out of order. Pre-increment
        // makes the first signalled value 1 (0 is the timeline's initial value).
        u64 TransferTimelineValue = 0;

        // Transfer-timeline waits the next frame submit must fold in. A resource
        // sampled in a frame after an async upload registers its (semaphore,
        // value) here; SubmitFrame chains them onto the submit and clears the set.
        // Guarded by SubmitMutex — accumulated on the main thread but read under
        // the same lock the submit takes.
        vector<std::pair<vk::Semaphore, u64>> PendingFrameTransferWaits{};

        // Serializes Retire, RetireOnTransfer, the transfer-retire list, and both
        // their drains. A worker dropping an upload's scratch (via
        // RetireOnTransfer) must not race the main thread's CurrentFrameInFlight
        // read, the bin vectors' reallocation, or the transfer-list drain.
        std::mutex RetireMutex;

        // Raw allocations whose GPU lifetime is the transfer timeline, not a frame
        // fence. Worker-created upload scratch is released (not retired through a
        // dtor) into here keyed on the timeline value its copy signals; the main
        // thread destroys each entry once TransferTimeline reaches that value.
        struct TransferRetireEntry
        {
            vk::Buffer Buffer;
            VmaAllocation Allocation;
            u64 TimelineValue;
        };

        vector<TransferRetireEntry> TransferRetireList{};

        vector<SynchronizationFrame> SynchronizationFrames{};
        u32 CurrentFrameInFlight = 0;
        u32 MaxFramesInFlight = 2;

        // One retire bin per in-flight frame. A handle retired while frame i is
        // recording goes into bin i and is protected by frame i's fence, so it
        // is destroyed when AcquireNextFrame next waits that fence. MaxFrames
        // bins suffice: retirements outside a frame (init, post-WaitIdle) are
        // caught by the shutdown drain-all in DisposeResources/Dispose.
        struct RetireBin
        {
            vector<std::pair<vk::Buffer, VmaAllocation>> Buffers;
            vector<std::pair<vk::Image, VmaAllocation>> Images;
            vector<vk::ImageView> ImageViews;
            vector<vk::Sampler> Samplers;
            vector<vk::ShaderModule> ShaderModules;
            vector<vk::Pipeline> Pipelines;
            vector<vk::PipelineLayout> PipelineLayouts;
            vector<vk::DescriptorSet> DescriptorSets; // freed back to the descriptor pool
        };

        vector<RetireBin> RetireBins{};
        bool Disposed = false;

        RetireBin& CurrentRetireBin();
        void DrainRetireBin(RetireBin& bin);
        void DrainAllRetireBins();

        // Deferred destruction. Resource destructors hand their Vulkan handles
        // here instead of destroying them immediately; the handle is retired
        // into the current frame's bin and only destroyed once that frame's
        // fence has been waited again (see AcquireNextFrame). This makes
        // dropping the last Ref/Unique to a resource mid-frame always safe —
        // in-flight GPU work that still references the handle finishes first.
        void Retire(vk::Buffer buffer, VmaAllocation allocation);
        void Retire(vk::Image image, VmaAllocation allocation);
        void Retire(vk::ImageView imageView);
        void Retire(vk::Sampler sampler);
        void Retire(vk::ShaderModule shaderModule);
        void Retire(vk::Pipeline pipeline);
        void Retire(vk::PipelineLayout pipelineLayout);
        void Retire(vk::DescriptorSet descriptorSet);

        // Take ownership of a raw allocation whose GPU lifetime is the transfer
        // timeline. Reclaimed (vmaDestroyBuffer) once TransferTimeline reaches
        // timelineValue — drained on the main thread in AcquireNextFrame and at
        // teardown. The caller passes the released raw handle, never a Buffer:
        // routing it through Buffer::~Buffer would re-defer it into a frame bin,
        // pinning it to the wrong lifetime.
        void RetireOnTransfer(vk::Buffer buffer, VmaAllocation allocation, u64 timelineValue);

        // Destroy every transfer-retire entry whose timeline value has been
        // reached. Holds RetireMutex across the GetValue() read and the erase.
        void DrainTransferRetireList();

        // Submit to a shared queue under SubmitMutex. vkQueueSubmit is not
        // thread-safe per VkQueue; every submit (frame, immediate, and the
        // plan-07 transfer submit) funnels through here so worker and main-thread
        // submits to a collapsed MoltenVK queue cannot race.
        void LockedSubmit(vk::Queue queue, const vk::SubmitInfo& submitInfo, vk::Fence fence);

        vector<const char*>& GetRequiredExtensions();
        vk::PhysicalDevice GetPhysicalDevice();
        bool IsDeviceSuitable(vk::PhysicalDevice device);
        QueueFamilyIndices& FindQueueFamilies(vk::PhysicalDevice device);
        [[nodiscard]] SwapChainSupportDetails QuerySwapChainSupport(vk::PhysicalDevice device) const;
        [[nodiscard]] bool CheckDeviceExtensionSupport(vk::PhysicalDevice device) const;
        vk::Device CreateDevice();

        QueueFamilyIndices QueueFamilies{};
    };
}
