#ifndef _LOGICAL_DEVICE_H_
#define _LOGICAL_DEVICE_H_

class PhysicalDevice;
class RenderPass;
class CommandPool;
class SwapChain;
class DescriptorPool;
class Sampler;
class DescriptorSetLayout;
class GraphicsPipeline;
class PipelineLayout;
class Shader;

#include <filesystem>
#include "../gfxcommon.h"
#include "physical_device.h"
#include "render_pass.h"
#include "command_pool.h"
#include "swap_chain.h"
#include "descriptor_pool.h"
#include "sampler.h"
#include "descriptor_set_layout.h"
#include "graphics_pipeline.h"
#include "pipeline_layout.h"
#include "shader.h"

class LogicalDevice
{
public:
    LogicalDevice(PhysicalDevice &physicalDevice);
    ~LogicalDevice();

    void waitIdle();
    void waitForFence(const VkFence &fence);
    void resetFence(const VkFence &fence);

    void createQueues(uint32_t graphicsFamilyQueueIndex, uint32_t presentFamilyQueueIndex);
    std::unique_ptr<CommandPool> createCommandPool();
    void destroyCommandPool(VkCommandPool pool);

    std::unique_ptr<RenderPass> createRenderPass(std::vector<VkAttachmentDescription> attachments, VkSubpassDescription subpass);
    void destroyRenderPass(VkRenderPass renderPass);

    std::unique_ptr<SwapChain> createSwapChain();
    void destroySwapChain(VkSwapchainKHR swapChain);

    std::unique_ptr<DescriptorPool> createDescriptorPool(uint32_t uniformBufferCount, uint32_t imageSamplerCount, uint32_t descriptorSetCount);
    void destroyDescriptorPool(VkDescriptorPool pool);

    std::unique_ptr<Sampler> createSampler();
    void destroySampler(VkSampler sampler);

    std::unique_ptr<DescriptorSetLayout> createDescriptorSetLayout(std::vector<VkDescriptorSetLayoutBinding> bindings);
    void destroyDescriptorSetLayout(VkDescriptorSetLayout layout);

    std::unique_ptr<GraphicsPipeline> createGraphicsPipeline(PipelineLayout &layout, Shader &vertShader, Shader &fragShader, RenderPass &renderPass);
    void destroyGraphicsPipeline(VkPipeline pipeline);

    std::unique_ptr<PipelineLayout> createPipelineLayout(std::vector<DescriptorSetLayout *> descriptorSetLayouts);
    void destroyPipelineLayout(VkPipelineLayout layout);

    std::unique_ptr<Shader> createShader(const std::filesystem::path &path);
    void destroyShader(VkShaderModule shaderModule);

    VkDevice device;

    VkDeviceCreateInfo deviceCreateInfo{};
    VkQueue presentQueue;
    VkQueue graphicsQueue;

    PhysicalDevice &physicalDevice;
};

#endif // _LOGICAL_DEVICE_H_