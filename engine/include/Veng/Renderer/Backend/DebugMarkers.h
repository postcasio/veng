#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/Backend/Vulkan.h>

/// @brief Casts a Vulkan dispatchable-handle type to the u64 expected by
/// vkSetDebugUtilsObjectNameEXT.
#define VK_OBJECT_TO_U64(objectType, object) reinterpret_cast<u64>(static_cast<objectType>(object))

namespace Veng::Renderer
{
    /// @brief Static helpers for attaching VK_EXT_debug_utils names to Vulkan objects.
    ///
    /// Call Initialize() once after instance creation; all Mark* functions are
    /// no-ops when the extension is unavailable.
    class DebugMarkers
    {
    public:
        /// @brief Loads VK_EXT_debug_utils function pointers from @p instance.
        static void Initialize(vk::Instance instance);

        /// @brief Attaches @p name to an arbitrary Vulkan object identified by its
        /// u64 handle and VkObjectType.
        static void MarkObject(vk::Device device, u64 object, VkObjectType objectType,
                               const string& name);
        /// @brief Attaches @p name to a fence.
        static void MarkFence(vk::Device device, vk::Fence fence, const string& name);
        /// @brief Attaches @p name to a semaphore.
        static void MarkSemaphore(vk::Device device, vk::Semaphore semaphore, const string& name);
        /// @brief Attaches @p name to a command buffer.
        static void MarkCommandBuffer(vk::Device device, vk::CommandBuffer commandBuffer,
                                      const string& name);
        /// @brief Attaches @p name to an image.
        static void MarkImage(vk::Device device, vk::Image image, const string& name);
        /// @brief Attaches @p name to an image view.
        static void MarkImageView(vk::Device device, vk::ImageView imageView, const string& name);
        /// @brief Attaches @p name to a buffer.
        static void MarkBuffer(vk::Device device, vk::Buffer buffer, const string& name);
        /// @brief Attaches @p name to a descriptor pool.
        static void MarkDescriptorPool(vk::Device device, vk::DescriptorPool descriptorPool,
                                       const string& name);
        /// @brief Attaches @p name to a descriptor set layout.
        static void MarkDescriptorSetLayout(vk::Device device,
                                            vk::DescriptorSetLayout descriptorSetLayout,
                                            const string& name);
        /// @brief Attaches @p name to a descriptor set.
        static void MarkDescriptorSet(vk::Device device, vk::DescriptorSet descriptorSet,
                                      const string& name);
        /// @brief Attaches @p name to a pipeline layout.
        static void MarkPipelineLayout(vk::Device device, vk::PipelineLayout pipelineLayout,
                                       const string& string);
        /// @brief Attaches @p name to a pipeline.
        static void MarkPipeline(vk::Device device, vk::Pipeline pipeline, const string& name);
        /// @brief Attaches @p name to a sampler.
        static void MarkSampler(vk::Device device, vk::Sampler sampler, const string& name);

        /// @brief Opens a named command-buffer label region; a no-op when the extension is unavailable.
        ///
        /// A RenderDoc or Xcode GPU capture groups the recorded work between this and the matching
        /// EndLabel under @p name. Balanced by an EndLabel on the same command buffer.
        /// @param commandBuffer  Command buffer the label is recorded into.
        /// @param name           The region's display name.
        static void BeginLabel(vk::CommandBuffer commandBuffer, const string& name);

        /// @brief Closes the most recently opened command-buffer label region.
        /// @param commandBuffer  Command buffer the label is recorded into.
        static void EndLabel(vk::CommandBuffer commandBuffer);

        /// @brief Inserts a single-point command-buffer label; a no-op when the extension is unavailable.
        /// @param commandBuffer  Command buffer the label is recorded into.
        /// @param name           The point label's display name.
        static void InsertLabel(vk::CommandBuffer commandBuffer, const string& name);

    private:
        static inline PFN_vkSetDebugUtilsObjectTagEXT s_PfnSetDebugUtilsObjectTag = nullptr;
        static inline PFN_vkSetDebugUtilsObjectNameEXT s_PfnSetDebugUtilsObjectName = nullptr;

        static inline PFN_vkCmdBeginDebugUtilsLabelEXT s_PfnCmdBeginDebugUtilsLabel = nullptr;
        static inline PFN_vkCmdEndDebugUtilsLabelEXT s_PfnCmdEndDebugUtilsLabel = nullptr;
        static inline PFN_vkCmdInsertDebugUtilsLabelEXT s_PfnCmdInsertDebugUtilsLabel = nullptr;
    };
}
