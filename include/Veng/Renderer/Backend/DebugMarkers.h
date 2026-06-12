#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/Backend/Vulkan.h>

#define VK_OBJECT_TO_U64(objectType, object) reinterpret_cast<u64>(static_cast<objectType>(object))

namespace Veng::Renderer
{
    class DebugMarkers
    {
    public:
        static void Initialize();
        static void MarkObject(u64 object, VkObjectType objectType, const string& name);
        static void MarkFence(vk::Fence fence, const string& name);
        static void MarkSemaphore(vk::Semaphore semaphore, const string& name);
        static void MarkCommandBuffer(vk::CommandBuffer commandBuffer, const string& name);
        static void MarkImage(vk::Image image, const string& name);
        static void MarkImageView(vk::ImageView imageView, const string& name);
        static void MarkBuffer(vk::Buffer buffer, const string& name);
        static void MarkDescriptorPool(vk::DescriptorPool descriptorPool, const string& name);
        static void MarkDescriptorSetLayout(vk::DescriptorSetLayout descriptorSetLayout, const string& name);
        static void MarkDescriptorSet(vk::DescriptorSet descriptorSet, const string& name);
        static void MarkRenderPass(vk::RenderPass renderPass, const string& name);
        static void MarkFramebuffer(vk::Framebuffer framebuffer, const string& string);
        static void MarkPipelineLayout(vk::PipelineLayout pipelineLayout, const string& string);
        static void MarkPipeline(vk::Pipeline pipeline, const string& name);
        static void MarkSampler(vk::Sampler sampler, const string& name);

    private:
        static inline PFN_vkSetDebugUtilsObjectTagEXT s_PfnSetDebugUtilsObjectTag = nullptr;
        static inline PFN_vkSetDebugUtilsObjectNameEXT s_PfnSetDebugUtilsObjectName = nullptr;

        static inline PFN_vkCmdDebugMarkerBeginEXT s_PfnCmdDebugMarkerBegin = nullptr;
        static inline PFN_vkCmdDebugMarkerEndEXT s_PfnCmdDebugMarkerEnd = nullptr;
        static inline PFN_vkCmdDebugMarkerInsertEXT s_PfnCmdDebugMarkerInsert = nullptr;
    };
}
