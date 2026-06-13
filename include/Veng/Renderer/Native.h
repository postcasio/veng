#pragma once

// The ONE public header that exposes raw Vulkan/VMA handles. Everything else
// in include/Veng/Renderer hides vk:: and Vma* types behind the Native idiom;
// reach for this header only when interop with Vulkan or a third-party
// library (e.g. ImGui backends) requires the underlying handle directly.
#include <Veng/Renderer/Backend/Vulkan.h>
#include <Veng/Renderer/Backend/Natives.h>

namespace Veng
{
    [[nodiscard]] inline GLFWwindow* GetGlfwWindow(const Window& window) { return window.m_Handle; }
    [[nodiscard]] inline vk::SurfaceKHR GetVkSurface(const Window& window) { return window.GetNative().Surface; }
}

namespace Veng::Renderer
{
    [[nodiscard]] inline vk::Instance GetVkInstance(const Context& context) { return context.GetNative().Instance; }
    [[nodiscard]] inline vk::PhysicalDevice GetVkPhysicalDevice(const Context& context) { return context.GetNative().PhysicalDevice; }
    [[nodiscard]] inline vk::Device GetVkDevice(const Context& context) { return context.GetNative().Device; }
    [[nodiscard]] inline vk::Queue GetVkGraphicsQueue(const Context& context) { return context.GetNative().GraphicsQueue; }
    [[nodiscard]] inline vk::Queue GetVkPresentQueue(const Context& context) { return context.GetNative().PresentQueue; }
    [[nodiscard]] inline vk::SurfaceKHR GetVkSurface(const Context& context) { return context.GetNative().Surface; }
    [[nodiscard]] inline VmaAllocator GetVmaAllocator(const Context& context) { return context.GetNative().Allocator; }

    [[nodiscard]] inline vk::Buffer GetVkBuffer(const Buffer& buffer) { return buffer.GetNative().Buffer; }
    [[nodiscard]] inline VmaAllocation GetVmaAllocation(const Buffer& buffer) { return buffer.GetNative().Allocation; }

    [[nodiscard]] inline vk::Image GetVkImage(const Image& image) { return image.GetNative().Image; }
    [[nodiscard]] inline VmaAllocation GetVmaAllocation(const Image& image) { return image.GetNative().Allocation; }
    [[nodiscard]] inline const VmaAllocationInfo& GetVmaAllocationInfo(const Image& image) { return image.GetNative().AllocationInfo; }

    [[nodiscard]] inline vk::ImageView GetVkImageView(const ImageView& imageView) { return imageView.GetNative().ImageView; }

    [[nodiscard]] inline vk::Sampler GetVkSampler(const Sampler& sampler) { return sampler.GetNative().Sampler; }

    [[nodiscard]] inline vk::ShaderModule GetVkShaderModule(const Shader& shader) { return shader.GetNative().Module; }

    [[nodiscard]] inline vk::DescriptorSet GetVkDescriptorSet(const DescriptorSet& descriptorSet) { return descriptorSet.GetNative().Set; }

    [[nodiscard]] inline vk::DescriptorSetLayout GetVkDescriptorSetLayout(const DescriptorSetLayout& layout) { return layout.GetNative().Layout; }

    [[nodiscard]] inline vk::PipelineLayout GetVkPipelineLayout(const PipelineLayout& layout) { return layout.GetNative().Layout; }

    [[nodiscard]] inline vk::Pipeline GetVkPipeline(const GraphicsPipeline& pipeline) { return pipeline.GetNative().Pipeline; }
    [[nodiscard]] inline vk::Pipeline GetVkPipeline(const DynamicGraphicsPipeline& pipeline) { return pipeline.GetNative().Pipeline; }
    [[nodiscard]] inline vk::Pipeline GetVkPipeline(const ComputePipeline& pipeline) { return pipeline.GetNative().Pipeline; }

    [[nodiscard]] inline vk::CommandBuffer GetVkCommandBuffer(const CommandBuffer& commandBuffer) { return commandBuffer.GetNative().CommandBuffer; }

    [[nodiscard]] inline vk::RenderPass GetVkRenderPass(const RenderPass& renderPass) { return renderPass.GetNative().RenderPass; }

    [[nodiscard]] inline vk::Framebuffer GetVkFramebuffer(const Framebuffer& framebuffer) { return framebuffer.GetNative().Framebuffer; }

    [[nodiscard]] inline vk::Fence GetVkFence(const Fence& fence) { return fence.GetNative().Fence; }

    [[nodiscard]] inline vk::Semaphore GetVkSemaphore(const Semaphore& semaphore) { return semaphore.GetNative().Semaphore; }
}
