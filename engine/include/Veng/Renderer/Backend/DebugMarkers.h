#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/Backend/Vulkan.h>

#define VK_OBJECT_TO_U64(objectType, object) reinterpret_cast<u64>(static_cast<objectType>(object))

namespace Veng::Renderer
{
    class DebugMarkers
    {
    public:
        static void Initialize(vk::Instance instance);
        static void MarkObject(vk::Device device, u64 object, VkObjectType objectType, const string& name);
        static void MarkFence(vk::Device device, vk::Fence fence, const string& name);
        static void MarkSemaphore(vk::Device device, vk::Semaphore semaphore, const string& name);
        static void MarkCommandBuffer(vk::Device device, vk::CommandBuffer commandBuffer, const string& name);
        static void MarkImage(vk::Device device, vk::Image image, const string& name);
        static void MarkImageView(vk::Device device, vk::ImageView imageView, const string& name);
        static void MarkBuffer(vk::Device device, vk::Buffer buffer, const string& name);
        static void MarkDescriptorPool(vk::Device device, vk::DescriptorPool descriptorPool, const string& name);
        static void MarkDescriptorSetLayout(vk::Device device, vk::DescriptorSetLayout descriptorSetLayout, const string& name);
        static void MarkDescriptorSet(vk::Device device, vk::DescriptorSet descriptorSet, const string& name);
        static void MarkPipelineLayout(vk::Device device, vk::PipelineLayout pipelineLayout, const string& string);
        static void MarkPipeline(vk::Device device, vk::Pipeline pipeline, const string& name);
        static void MarkSampler(vk::Device device, vk::Sampler sampler, const string& name);

    private:
        static inline PFN_vkSetDebugUtilsObjectTagEXT s_PfnSetDebugUtilsObjectTag = nullptr;
        static inline PFN_vkSetDebugUtilsObjectNameEXT s_PfnSetDebugUtilsObjectName = nullptr;

        static inline PFN_vkCmdDebugMarkerBeginEXT s_PfnCmdDebugMarkerBegin = nullptr;
        static inline PFN_vkCmdDebugMarkerEndEXT s_PfnCmdDebugMarkerEnd = nullptr;
        static inline PFN_vkCmdDebugMarkerInsertEXT s_PfnCmdDebugMarkerInsert = nullptr;
    };
}
