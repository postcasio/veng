#include <Veng/Renderer/Native.h>

#include "Natives.h"

#include <bit>

namespace Veng
{
    vk::SurfaceKHR GetVkSurface(const Window& window)
    {
        return vk::SurfaceKHR{std::bit_cast<VkSurfaceKHR>(window.GetNative().PresentationSurface)};
    }
}

namespace Veng::Renderer
{
    u64 CreatePresentationSurface(const Context& context, GLFWwindow* window)
    {
        VkSurfaceKHR surface;
        VK_RAW_ASSERT(glfwCreateWindowSurface(GetVkInstance(context), window, nullptr, &surface),
                      "Failed to create window surface!");
        return std::bit_cast<u64>(surface);
    }

    vk::Instance GetVkInstance(const Context& context)
    {
        return context.GetNative().Instance;
    }

    vk::PhysicalDevice GetVkPhysicalDevice(const Context& context)
    {
        return context.GetNative().PhysicalDevice;
    }

    vk::Device GetVkDevice(const Context& context)
    {
        return context.GetNative().Device;
    }

    vk::Queue GetVkGraphicsQueue(const Context& context)
    {
        return context.GetNative().GraphicsQueue;
    }

    vk::Queue GetVkPresentQueue(const Context& context)
    {
        return context.GetNative().PresentQueue;
    }

    vk::SurfaceKHR GetVkSurface(const Context& context)
    {
        return context.GetNative().Surface;
    }

    VmaAllocator GetVmaAllocator(const Context& context)
    {
        return context.GetNative().Allocator;
    }

    vk::PipelineCache GetVkPipelineCache(const Context& context)
    {
        return context.GetNative().PipelineCache;
    }

    vk::Buffer GetVkBuffer(const Buffer& buffer)
    {
        return buffer.GetNative().Buffer;
    }

    VmaAllocation GetVmaAllocation(const Buffer& buffer)
    {
        return buffer.GetNative().Allocation;
    }

    ReleasedBuffer ReleaseBuffer(const Buffer& buffer)
    {
        auto& native = buffer.GetNative();
        const ReleasedBuffer released{.Buffer = native.Buffer, .Allocation = native.Allocation};
        native.Buffer = nullptr;
        native.Allocation = nullptr;
        return released;
    }

    vk::Image GetVkImage(const Image& image)
    {
        return image.GetNative().Image;
    }

    VmaAllocation GetVmaAllocation(const Image& image)
    {
        return image.GetNative().Allocation;
    }

    const VmaAllocationInfo& GetVmaAllocationInfo(const Image& image)
    {
        return image.GetNative().AllocationInfo;
    }

    vk::ImageView GetVkImageView(const ImageView& imageView)
    {
        return imageView.GetNative().ImageView;
    }

    vk::Sampler GetVkSampler(const Sampler& sampler)
    {
        return sampler.GetNative().Sampler;
    }

    vk::ShaderModule GetVkShaderModule(const ShaderModule& shader)
    {
        return shader.GetNative().Module;
    }

    vk::DescriptorSet GetVkDescriptorSet(const DescriptorSet& descriptorSet)
    {
        return descriptorSet.GetNative().Set;
    }

    vk::DescriptorSetLayout GetVkDescriptorSetLayout(const DescriptorSetLayout& layout)
    {
        return layout.GetNative().Layout;
    }

    vk::PipelineLayout GetVkPipelineLayout(const PipelineLayout& layout)
    {
        return layout.GetNative().Layout;
    }

    vk::Pipeline GetVkPipeline(const GraphicsPipeline& pipeline)
    {
        return pipeline.GetNative().Pipeline;
    }

    vk::Pipeline GetVkPipeline(const ComputePipeline& pipeline)
    {
        return pipeline.GetNative().Pipeline;
    }

    vk::CommandBuffer GetVkCommandBuffer(const CommandBuffer& commandBuffer)
    {
        return commandBuffer.GetNative().CommandBuffer;
    }

    vk::Fence GetVkFence(const Fence& fence)
    {
        return fence.GetNative().Fence;
    }

    vk::Semaphore GetVkSemaphore(const Semaphore& semaphore)
    {
        return semaphore.GetNative().Semaphore;
    }

    vk::Semaphore GetVkSemaphore(const TimelineSemaphore& semaphore)
    {
        return semaphore.GetNative().Semaphore;
    }
}
