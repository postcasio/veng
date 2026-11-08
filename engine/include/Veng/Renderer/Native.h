#pragma once

/// @brief The one public header that exposes the Vulkan backend's raw handles.
///
/// Everything else in include/Veng/Renderer hides vk:: and Vma* types behind the Native idiom;
/// reach for this header only when interop with Vulkan or a third-party library requires the
/// underlying handle directly. It exists only in a build whose GPU backend is Vulkan
/// (VE_GPU_BACKEND), and refuses to compile in any other; a consumer including it supplies the
/// Vulkan and VulkanMemoryAllocator headers on its include path.
///
/// It also carries the vulkan.hpp configuration every translation unit parsing Vulkan must agree
/// on, so the backend builds on it rather than restating it.
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

#ifndef VE_GPU_BACKEND_VULKAN
#error "Veng/Renderer/Native.h exposes Vulkan handles and compiles only in a Vulkan-backend build"
#endif

#include <Veng/Assert.h>
#include <Veng/Export.h>

// veng raises no exceptions and builds with -fno-exceptions, so vulkan.hpp's
// throwing convenience overloads must be switched off. With NO_EXCEPTIONS the
// value-returning calls (device.createX(...)) return vk::ResultValue<T> — read
// the result via `.value` — and vulkan.hpp's internal result checks route to
// VULKAN_HPP_ASSERT_ON_RESULT, which we point at the fatal-assert path below.
// Both macros must be defined before <vulkan/vulkan.hpp> is first included.
#define VULKAN_HPP_NO_EXCEPTIONS
#define VULKAN_HPP_ASSERT_ON_RESULT(expr)                                                          \
    VE_ASSERT((expr), "vulkan.hpp call returned a non-success vk::Result")

#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#define VULKAN_HPP_NO_CONSTRUCTORS

// VK_EXT_metal_objects — the Metal device/queue export chained onto instance creation and the
// MTLTexture import behind an external render target — lives behind the Metal platform guard.
// Defined here, before vulkan.hpp is first parsed, because the guard widens the dynamic
// dispatcher's member set: a tree where only some translation units define it would give
// VULKAN_HPP_DEFAULT_DISPATCHER two layouts.
#ifdef __APPLE__
#define VK_USE_PLATFORM_METAL_EXT
#endif

#include <vulkan/vulkan.hpp>

// vk_mem_alloc.h is a third-party single-header allocator whose warnings are not ours to fix;
// silence them per compiler around the one include.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-completeness"
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wunused-function"
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#endif

#include <vk_mem_alloc.h>

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

struct GLFWwindow;

namespace Veng
{
    class Window;
}

namespace Veng::Renderer
{
    class Buffer;
    class CommandBuffer;
    class ComputePipeline;
    class Context;
    class DescriptorSet;
    class DescriptorSetLayout;
    class Fence;
    class GraphicsPipeline;
    class Image;
    class ImageView;
    class PipelineLayout;
    class Sampler;
    class Semaphore;
    class ShaderModule;
    class TimelineSemaphore;
}

namespace Veng
{
    /// @brief Returns the underlying GLFW window handle.
    [[nodiscard]] VE_API GLFWwindow* GetGlfwWindow(const Window& window);

    /// @brief Returns the Vulkan surface for a window.
    [[nodiscard]] VE_API vk::SurfaceKHR GetVkSurface(const Window& window);
}

namespace Veng::Renderer
{
    /// @brief Returns the Vulkan instance.
    [[nodiscard]] VE_API vk::Instance GetVkInstance(const Context& context);

    /// @brief Returns the Vulkan physical device.
    [[nodiscard]] VE_API vk::PhysicalDevice GetVkPhysicalDevice(const Context& context);

    /// @brief Returns the Vulkan logical device.
    [[nodiscard]] VE_API vk::Device GetVkDevice(const Context& context);

    /// @brief Returns the graphics queue.
    [[nodiscard]] VE_API vk::Queue GetVkGraphicsQueue(const Context& context);

    /// @brief Returns the present queue.
    [[nodiscard]] VE_API vk::Queue GetVkPresentQueue(const Context& context);

    /// @brief Returns the Vulkan surface associated with a context.
    [[nodiscard]] VE_API vk::SurfaceKHR GetVkSurface(const Context& context);

    /// @brief Returns the VMA allocator.
    [[nodiscard]] VE_API VmaAllocator GetVmaAllocator(const Context& context);

    /// @brief Returns the pipeline cache.
    [[nodiscard]] VE_API vk::PipelineCache GetVkPipelineCache(const Context& context);

    /// @brief Returns the underlying Vulkan buffer handle.
    [[nodiscard]] VE_API vk::Buffer GetVkBuffer(const Buffer& buffer);

    /// @brief Returns the VMA allocation for a buffer.
    [[nodiscard]] VE_API VmaAllocation GetVmaAllocation(const Buffer& buffer);

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
    [[nodiscard]] VE_API ReleasedBuffer ReleaseBuffer(const Buffer& buffer);

    /// @brief Returns the underlying Vulkan image handle.
    [[nodiscard]] VE_API vk::Image GetVkImage(const Image& image);

    /// @brief Returns the VMA allocation for an image.
    [[nodiscard]] VE_API VmaAllocation GetVmaAllocation(const Image& image);

    /// @brief Returns the VMA allocation info for an image.
    [[nodiscard]] VE_API const VmaAllocationInfo& GetVmaAllocationInfo(const Image& image);

    /// @brief Returns the underlying Vulkan image view handle.
    [[nodiscard]] VE_API vk::ImageView GetVkImageView(const ImageView& imageView);

    /// @brief Returns the underlying Vulkan sampler handle.
    [[nodiscard]] VE_API vk::Sampler GetVkSampler(const Sampler& sampler);

    /// @brief Returns the underlying Vulkan shader module handle.
    [[nodiscard]] VE_API vk::ShaderModule GetVkShaderModule(const ShaderModule& shader);

    /// @brief Returns the underlying Vulkan descriptor set handle.
    [[nodiscard]] VE_API vk::DescriptorSet GetVkDescriptorSet(const DescriptorSet& descriptorSet);

    /// @brief Returns the underlying Vulkan descriptor set layout handle.
    [[nodiscard]] VE_API vk::DescriptorSetLayout
    GetVkDescriptorSetLayout(const DescriptorSetLayout& layout);

    /// @brief Returns the underlying Vulkan pipeline layout handle.
    [[nodiscard]] VE_API vk::PipelineLayout GetVkPipelineLayout(const PipelineLayout& layout);

    /// @brief Returns the underlying Vulkan pipeline handle for a graphics pipeline.
    [[nodiscard]] VE_API vk::Pipeline GetVkPipeline(const GraphicsPipeline& pipeline);

    /// @brief Returns the underlying Vulkan pipeline handle for a compute pipeline.
    [[nodiscard]] VE_API vk::Pipeline GetVkPipeline(const ComputePipeline& pipeline);

    /// @brief Returns the underlying Vulkan command buffer handle.
    [[nodiscard]] VE_API vk::CommandBuffer GetVkCommandBuffer(const CommandBuffer& commandBuffer);

    /// @brief Returns the underlying Vulkan fence handle.
    [[nodiscard]] VE_API vk::Fence GetVkFence(const Fence& fence);

    /// @brief Returns the underlying Vulkan semaphore handle for a binary semaphore.
    [[nodiscard]] VE_API vk::Semaphore GetVkSemaphore(const Semaphore& semaphore);

    /// @brief Returns the underlying Vulkan semaphore handle for a timeline semaphore.
    ///
    /// Used by the Context submit helper to thread it through VkTimelineSemaphoreSubmitInfo
    /// alongside the per-submit u64 value.
    [[nodiscard]] VE_API vk::Semaphore GetVkSemaphore(const TimelineSemaphore& semaphore);
}
