#include <Veng/Renderer/Backend/DebugMarkers.h>

#include <Veng/Renderer/Backend/Context.h>

namespace Veng::Renderer
{
    void DebugMarkers::Initialize()
    {
        auto& instance = Context::Instance().GetVkInstance();

        s_PfnSetDebugUtilsObjectTag = reinterpret_cast<PFN_vkSetDebugUtilsObjectTagEXT>(instance.getProcAddr(
            "vkSetDebugUtilsObjectTagEXT"));
        s_PfnSetDebugUtilsObjectName = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(instance.getProcAddr(
            "vkSetDebugUtilsObjectNameEXT"));

        s_PfnCmdDebugMarkerBegin = reinterpret_cast<PFN_vkCmdDebugMarkerBeginEXT>(instance.getProcAddr(
            "vkCmdDebugMarkerBeginEXT"));
        s_PfnCmdDebugMarkerEnd = reinterpret_cast<PFN_vkCmdDebugMarkerEndEXT>(instance.getProcAddr(
            "vkCmdDebugMarkerEndEXT"));
        s_PfnCmdDebugMarkerInsert = reinterpret_cast<PFN_vkCmdDebugMarkerInsertEXT>(instance.getProcAddr(
            "vkCmdDebugMarkerInsertEXT"));
    }

    void DebugMarkers::MarkObject(const u64 object, const VkObjectType objectType, const string& name)
    {
        const VkDebugUtilsObjectNameInfoEXT nameInfo{
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
            .objectType = objectType,
            .objectHandle = object,
            .pObjectName = name.c_str(),
        };

        const auto& device = Context::Instance().GetVkDevice();

        s_PfnSetDebugUtilsObjectName(device, &nameInfo);
    }

    void DebugMarkers::MarkFence(vk::Fence fence, const string& name)
    {
        MarkObject(VK_OBJECT_TO_U64(VkFence, fence), VK_OBJECT_TYPE_FENCE, name);
    }

    void DebugMarkers::MarkSemaphore(const vk::Semaphore semaphore, const string& name)
    {
        MarkObject(VK_OBJECT_TO_U64(VkSemaphore, semaphore), VK_OBJECT_TYPE_SEMAPHORE, name);
    }

    void DebugMarkers::MarkCommandBuffer(const vk::CommandBuffer commandBuffer, const string& name)
    {
        MarkObject(VK_OBJECT_TO_U64(VkCommandBuffer, commandBuffer), VK_OBJECT_TYPE_COMMAND_BUFFER, name);
    }

    void DebugMarkers::MarkImage(const vk::Image image, const string& name)
    {
        MarkObject(VK_OBJECT_TO_U64(VkImage, image), VK_OBJECT_TYPE_IMAGE, name);
    }

    void DebugMarkers::MarkImageView(const vk::ImageView imageView, const string& name)
    {
        MarkObject(VK_OBJECT_TO_U64(VkImageView, imageView), VK_OBJECT_TYPE_IMAGE_VIEW, name);
    }

    void DebugMarkers::MarkBuffer(const vk::Buffer buffer, const string& name)
    {
        MarkObject(VK_OBJECT_TO_U64(VkBuffer, buffer), VK_OBJECT_TYPE_BUFFER, name);
    }

    void DebugMarkers::MarkDescriptorPool(const vk::DescriptorPool descriptorPool, const string& name)
    {
        MarkObject(VK_OBJECT_TO_U64(VkDescriptorPool, descriptorPool), VK_OBJECT_TYPE_DESCRIPTOR_POOL, name);
    }

    void DebugMarkers::MarkDescriptorSetLayout(const vk::DescriptorSetLayout descriptorSetLayout, const string& name)
    {
        MarkObject(VK_OBJECT_TO_U64(VkDescriptorSetLayout, descriptorSetLayout), VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT,
                   name);
    }

    void DebugMarkers::MarkDescriptorSet(const vk::DescriptorSet descriptorSet, const string& name)
    {
        MarkObject(VK_OBJECT_TO_U64(VkDescriptorSet, descriptorSet), VK_OBJECT_TYPE_DESCRIPTOR_SET, name);
    }

    void DebugMarkers::MarkRenderPass(const vk::RenderPass renderPass, const string& name)
    {
        MarkObject(VK_OBJECT_TO_U64(VkRenderPass, renderPass), VK_OBJECT_TYPE_RENDER_PASS, name);
    }

    void DebugMarkers::MarkFramebuffer(const vk::Framebuffer framebuffer, const string& name)
    {
        MarkObject(VK_OBJECT_TO_U64(VkFramebuffer, framebuffer), VK_OBJECT_TYPE_FRAMEBUFFER, name);
    }

    void DebugMarkers::MarkPipelineLayout(const vk::PipelineLayout pipelineLayout, const string& name)
    {
        MarkObject(VK_OBJECT_TO_U64(VkPipelineLayout, pipelineLayout), VK_OBJECT_TYPE_PIPELINE_LAYOUT, name);
    }

    void DebugMarkers::MarkPipeline(const vk::Pipeline pipeline, const string& name)
    {
        MarkObject(VK_OBJECT_TO_U64(VkPipeline, pipeline), VK_OBJECT_TYPE_PIPELINE, name);
    }

    void DebugMarkers::MarkSampler(const vk::Sampler sampler, const string& name)
    {
        MarkObject(VK_OBJECT_TO_U64(VkSampler, sampler), VK_OBJECT_TYPE_SAMPLER, name);
    }
}
