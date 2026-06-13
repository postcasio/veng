#pragma once

// Out-of-line definition of Context::Native. Kept in its own header (rather
// than inline in Natives.h) because of its size: it absorbs every vk::/Vma
// handle the context owns, the deferred-destruction (retire bin) machinery,
// and the device-selection helpers that only run during Initialize().
// Internal-only: included by Natives.h.
#include <Veng/Renderer/Backend/Vulkan.h>
#include <Veng/Renderer/Backend/CommandPool.h>
#include <Veng/Renderer/Backend/DescriptorPool.h>
#include <Veng/Renderer/Backend/SwapChain.h>
#include <Veng/Renderer/Backend/SwapChainSupport.h>
#include <Veng/Renderer/Backend/SynchronizationFrame.h>

#include <Veng/Renderer/Context.h>

namespace Veng::Renderer
{
    struct Context::Native
    {
        vk::Instance Instance;
        vk::PhysicalDevice PhysicalDevice;
        vk::Device Device;
        vk::Queue GraphicsQueue;
        vk::Queue PresentQueue;
        vk::SurfaceKHR Surface;
        vk::DebugUtilsMessengerEXT DebugMessenger;
        VmaAllocator Allocator = nullptr;

        const vector<const char*> ValidationLayers = vector<const char*>({"VK_LAYER_KHRONOS_validation"});
        const vector<const char*> DeviceExtensions = vector<const char*>({
            VK_KHR_SWAPCHAIN_EXTENSION_NAME,
            VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME,
            VK_EXT_SHADER_VIEWPORT_INDEX_LAYER_EXTENSION_NAME,
            VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,
            VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME
        });
        vector<const char*> RequiredExtensions;

        Unique<SwapChain> SwapChain;
        Unique<CommandPool> CommandPool;
        Unique<DescriptorPool> DescriptorPool;

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
            vector<vk::RenderPass> RenderPasses;
            vector<vk::Framebuffer> Framebuffers;
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
        void Retire(vk::RenderPass renderPass);
        void Retire(vk::Framebuffer framebuffer);
        void Retire(vk::DescriptorSet descriptorSet);

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
