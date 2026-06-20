#include <Veng/Renderer/Backend/DebugMarkers.h>

namespace Veng::Renderer
{
    void DebugMarkers::Initialize(vk::Instance instance)
    {
        s_PfnSetDebugUtilsObjectTag = reinterpret_cast<PFN_vkSetDebugUtilsObjectTagEXT>(
            instance.getProcAddr("vkSetDebugUtilsObjectTagEXT"));
        s_PfnSetDebugUtilsObjectName = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
            instance.getProcAddr("vkSetDebugUtilsObjectNameEXT"));

        s_PfnCmdDebugMarkerBegin = reinterpret_cast<PFN_vkCmdDebugMarkerBeginEXT>(
            instance.getProcAddr("vkCmdDebugMarkerBeginEXT"));
        s_PfnCmdDebugMarkerEnd = reinterpret_cast<PFN_vkCmdDebugMarkerEndEXT>(
            instance.getProcAddr("vkCmdDebugMarkerEndEXT"));
        s_PfnCmdDebugMarkerInsert = reinterpret_cast<PFN_vkCmdDebugMarkerInsertEXT>(
            instance.getProcAddr("vkCmdDebugMarkerInsertEXT"));
    }

    void DebugMarkers::MarkObject(vk::Device device, const u64 object,
                                  const VkObjectType objectType, const string& name)
    {
        const VkDebugUtilsObjectNameInfoEXT nameInfo{
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
            .objectType = objectType,
            .objectHandle = object,
            .pObjectName = name.c_str(),
        };

        s_PfnSetDebugUtilsObjectName(device, &nameInfo);
    }

    void DebugMarkers::MarkFence(vk::Device device, vk::Fence fence, const string& name)
    {
        MarkObject(device, VK_OBJECT_TO_U64(VkFence, fence), VK_OBJECT_TYPE_FENCE, name);
    }

    void DebugMarkers::MarkSemaphore(vk::Device device, const vk::Semaphore semaphore,
                                     const string& name)
    {
        MarkObject(device, VK_OBJECT_TO_U64(VkSemaphore, semaphore), VK_OBJECT_TYPE_SEMAPHORE,
                   name);
    }

    void DebugMarkers::MarkCommandBuffer(vk::Device device, const vk::CommandBuffer commandBuffer,
                                         const string& name)
    {
        MarkObject(device, VK_OBJECT_TO_U64(VkCommandBuffer, commandBuffer),
                   VK_OBJECT_TYPE_COMMAND_BUFFER, name);
    }

    void DebugMarkers::MarkImage(vk::Device device, const vk::Image image, const string& name)
    {
        MarkObject(device, VK_OBJECT_TO_U64(VkImage, image), VK_OBJECT_TYPE_IMAGE, name);
    }

    void DebugMarkers::MarkImageView(vk::Device device, const vk::ImageView imageView,
                                     const string& name)
    {
        MarkObject(device, VK_OBJECT_TO_U64(VkImageView, imageView), VK_OBJECT_TYPE_IMAGE_VIEW,
                   name);
    }

    void DebugMarkers::MarkBuffer(vk::Device device, const vk::Buffer buffer, const string& name)
    {
        MarkObject(device, VK_OBJECT_TO_U64(VkBuffer, buffer), VK_OBJECT_TYPE_BUFFER, name);
    }

    void DebugMarkers::MarkDescriptorPool(vk::Device device,
                                          const vk::DescriptorPool descriptorPool,
                                          const string& name)
    {
        MarkObject(device, VK_OBJECT_TO_U64(VkDescriptorPool, descriptorPool),
                   VK_OBJECT_TYPE_DESCRIPTOR_POOL, name);
    }

    void DebugMarkers::MarkDescriptorSetLayout(vk::Device device,
                                               const vk::DescriptorSetLayout descriptorSetLayout,
                                               const string& name)
    {
        MarkObject(device, VK_OBJECT_TO_U64(VkDescriptorSetLayout, descriptorSetLayout),
                   VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, name);
    }

    void DebugMarkers::MarkDescriptorSet(vk::Device device, const vk::DescriptorSet descriptorSet,
                                         const string& name)
    {
        MarkObject(device, VK_OBJECT_TO_U64(VkDescriptorSet, descriptorSet),
                   VK_OBJECT_TYPE_DESCRIPTOR_SET, name);
    }

    void DebugMarkers::MarkPipelineLayout(vk::Device device,
                                          const vk::PipelineLayout pipelineLayout,
                                          const string& name)
    {
        MarkObject(device, VK_OBJECT_TO_U64(VkPipelineLayout, pipelineLayout),
                   VK_OBJECT_TYPE_PIPELINE_LAYOUT, name);
    }

    void DebugMarkers::MarkPipeline(vk::Device device, const vk::Pipeline pipeline,
                                    const string& name)
    {
        MarkObject(device, VK_OBJECT_TO_U64(VkPipeline, pipeline), VK_OBJECT_TYPE_PIPELINE, name);
    }

    void DebugMarkers::MarkSampler(vk::Device device, const vk::Sampler sampler, const string& name)
    {
        MarkObject(device, VK_OBJECT_TO_U64(VkSampler, sampler), VK_OBJECT_TYPE_SAMPLER, name);
    }
}
