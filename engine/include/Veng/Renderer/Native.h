#pragma once

/// @brief The one public header that exposes raw Vulkan/VMA handles.
///
/// Everything else in include/Veng/Renderer hides vk:: and Vma* types behind the
/// Native idiom; reach for this header only when interop with Vulkan or a third-party
/// library (e.g. ImGui backends) requires the underlying handle directly.
///
/// GetNative() const doctrine: every resource declares
/// `[[nodiscard]] Native& GetNative() const;` — a const method returning a mutable
/// reference. This is deliberate: GetNative() hands out the raw backend handles
/// (vk::Buffer, vk::Image, ...) so the backend and Vulkan interop helpers below can
/// issue commands and bind resources. The wrapper object's logical constness describes
/// *this engine object's identity* (its name, format, extent, ...) — it says nothing
/// about the constness of the GPU state the handle refers to, which is mutated by
/// recording commands regardless of how the wrapper was reached. Treat
/// `const Resource&` as "this wrapper's identity won't change", not as "the GPU
/// resource behind it is read-only".
#include <Veng/Renderer/Backend/Vulkan.h>
#include <Veng/Renderer/Backend/Natives.h>

namespace Veng
{
    /// @brief Returns the underlying GLFW window handle.
    [[nodiscard]] inline GLFWwindow* GetGlfwWindow(const Window& window) { return window.m_Handle; }

    /// @brief Returns the Vulkan surface for a window.
    [[nodiscard]] inline vk::SurfaceKHR GetVkSurface(const Window& window) { return window.GetNative().Surface; }
}

namespace Veng::Renderer
{
    /// @brief Returns the Vulkan instance.
    [[nodiscard]] inline vk::Instance GetVkInstance(const Context& context) { return context.GetNative().Instance; }

    /// @brief Returns the Vulkan physical device.
    [[nodiscard]] inline vk::PhysicalDevice GetVkPhysicalDevice(const Context& context) { return context.GetNative().PhysicalDevice; }

    /// @brief Returns the Vulkan logical device.
    [[nodiscard]] inline vk::Device GetVkDevice(const Context& context) { return context.GetNative().Device; }

    /// @brief Returns the graphics queue.
    [[nodiscard]] inline vk::Queue GetVkGraphicsQueue(const Context& context) { return context.GetNative().GraphicsQueue; }

    /// @brief Returns the present queue.
    [[nodiscard]] inline vk::Queue GetVkPresentQueue(const Context& context) { return context.GetNative().PresentQueue; }

    /// @brief Returns the Vulkan surface associated with a context.
    [[nodiscard]] inline vk::SurfaceKHR GetVkSurface(const Context& context) { return context.GetNative().Surface; }

    /// @brief Returns the VMA allocator.
    [[nodiscard]] inline VmaAllocator GetVmaAllocator(const Context& context) { return context.GetNative().Allocator; }

    /// @brief Returns the pipeline cache.
    [[nodiscard]] inline vk::PipelineCache GetVkPipelineCache(const Context& context) { return context.GetNative().PipelineCache; }

    /// @brief Returns the underlying Vulkan buffer handle.
    [[nodiscard]] inline vk::Buffer GetVkBuffer(const Buffer& buffer) { return buffer.GetNative().Buffer; }

    /// @brief Returns the VMA allocation for a buffer.
    [[nodiscard]] inline VmaAllocation GetVmaAllocation(const Buffer& buffer) { return buffer.GetNative().Allocation; }

    /// @brief A buffer's raw backend handles after they have been released from the wrapper.
    ///
    /// The wrapper does not own these handles; the caller that received them via
    /// ReleaseBuffer() is responsible for their GPU lifetime.
    struct ReleasedBuffer
    {
        /// @brief The raw Vulkan buffer handle.
        vk::Buffer Buffer;
        /// @brief The VMA allocation.
        VmaAllocation Allocation;
    };

    /// @brief Releases a buffer's raw handles, transferring GPU lifetime to the caller.
    ///
    /// Nulls the wrapper's Native so its destructor is a no-op. Used by the async upload
    /// path to queue the staging buffer for destruction on the transfer timeline rather than
    /// the per-frame graphics fence.
    /// @param buffer The buffer whose handles to release.
    /// @return The released handles; the caller owns them.
    [[nodiscard]] inline ReleasedBuffer ReleaseBuffer(Buffer& buffer)
    {
        auto& native = buffer.GetNative();
        const ReleasedBuffer released{native.Buffer, native.Allocation};
        native.Buffer = nullptr;
        native.Allocation = nullptr;
        return released;
    }

    /// @brief Returns the underlying Vulkan image handle.
    [[nodiscard]] inline vk::Image GetVkImage(const Image& image) { return image.GetNative().Image; }

    /// @brief Returns the VMA allocation for an image.
    [[nodiscard]] inline VmaAllocation GetVmaAllocation(const Image& image) { return image.GetNative().Allocation; }

    /// @brief Returns the VMA allocation info for an image.
    [[nodiscard]] inline const VmaAllocationInfo& GetVmaAllocationInfo(const Image& image) { return image.GetNative().AllocationInfo; }

    /// @brief Returns the underlying Vulkan image view handle.
    [[nodiscard]] inline vk::ImageView GetVkImageView(const ImageView& imageView) { return imageView.GetNative().ImageView; }

    /// @brief Returns the underlying Vulkan sampler handle.
    [[nodiscard]] inline vk::Sampler GetVkSampler(const Sampler& sampler) { return sampler.GetNative().Sampler; }

    /// @brief Returns the underlying Vulkan shader module handle.
    [[nodiscard]] inline vk::ShaderModule GetVkShaderModule(const ShaderModule& shader) { return shader.GetNative().Module; }

    /// @brief Returns the underlying Vulkan descriptor set handle.
    [[nodiscard]] inline vk::DescriptorSet GetVkDescriptorSet(const DescriptorSet& descriptorSet) { return descriptorSet.GetNative().Set; }

    /// @brief Returns the underlying Vulkan descriptor set layout handle.
    [[nodiscard]] inline vk::DescriptorSetLayout GetVkDescriptorSetLayout(const DescriptorSetLayout& layout) { return layout.GetNative().Layout; }

    /// @brief Returns the underlying Vulkan pipeline layout handle.
    [[nodiscard]] inline vk::PipelineLayout GetVkPipelineLayout(const PipelineLayout& layout) { return layout.GetNative().Layout; }

    /// @brief Returns the underlying Vulkan pipeline handle for a graphics pipeline.
    [[nodiscard]] inline vk::Pipeline GetVkPipeline(const GraphicsPipeline& pipeline) { return pipeline.GetNative().Pipeline; }

    /// @brief Returns the underlying Vulkan pipeline handle for a compute pipeline.
    [[nodiscard]] inline vk::Pipeline GetVkPipeline(const ComputePipeline& pipeline) { return pipeline.GetNative().Pipeline; }

    /// @brief Returns the underlying Vulkan command buffer handle.
    [[nodiscard]] inline vk::CommandBuffer GetVkCommandBuffer(const CommandBuffer& commandBuffer) { return commandBuffer.GetNative().CommandBuffer; }

    /// @brief Returns the underlying Vulkan fence handle.
    [[nodiscard]] inline vk::Fence GetVkFence(const Fence& fence) { return fence.GetNative().Fence; }

    /// @brief Returns the underlying Vulkan semaphore handle for a binary semaphore.
    [[nodiscard]] inline vk::Semaphore GetVkSemaphore(const Semaphore& semaphore) { return semaphore.GetNative().Semaphore; }

    /// @brief Returns the underlying Vulkan semaphore handle for a timeline semaphore.
    ///
    /// Used by the Context submit helper to thread it through VkTimelineSemaphoreSubmitInfo
    /// alongside the per-submit u64 value.
    [[nodiscard]] inline vk::Semaphore GetVkSemaphore(const TimelineSemaphore& semaphore) { return semaphore.GetNative().Semaphore; }
}
