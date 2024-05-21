#include "logical_device.h"
#include "physical_device.h"
#include "validation_layers.h"

#include <set>

LogicalDevice::LogicalDevice(PhysicalDevice &physicalDevice) : physicalDevice(physicalDevice)
{
    QueueFamilyIndices indices = physicalDevice.findQueueFamilies();

    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
    std::set<uint32_t> uniqueQueueFamilies = {indices.graphicsFamily.value(),
                                              indices.presentFamily.value()};

    float queuePriority = 1.0f;
    for (uint32_t queueFamily : uniqueQueueFamilies)
    {
        VkDeviceQueueCreateInfo queueCreateInfo{};
        queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueCreateInfo.queueFamilyIndex = queueFamily;
        queueCreateInfo.queueCount = 1;
        queueCreateInfo.pQueuePriorities = &queuePriority;
        queueCreateInfos.push_back(queueCreateInfo);
    }

    VkPhysicalDeviceFeatures deviceFeatures{};
    deviceFeatures.samplerAnisotropy = VK_TRUE;
    deviceFeatures.sampleRateShading = VK_TRUE;

    deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;

    deviceCreateInfo.queueCreateInfoCount =
        static_cast<uint32_t>(queueCreateInfos.size());
    deviceCreateInfo.pQueueCreateInfos = queueCreateInfos.data();

    deviceCreateInfo.pEnabledFeatures = &deviceFeatures;

    auto deviceExtensions = getDeviceExtensions();

    deviceCreateInfo.enabledExtensionCount =
        static_cast<uint32_t>(deviceExtensions->size());
    deviceCreateInfo.ppEnabledExtensionNames = deviceExtensions->data();

#ifdef ENABLE_VALIDATION_LAYERS
    auto validationLayers = getValidationLayers();
    deviceCreateInfo.enabledLayerCount =
        static_cast<uint32_t>(validationLayers->size());
    deviceCreateInfo.ppEnabledLayerNames = validationLayers->data();
#else
    deviceCreateInfo.enabledLayerCount = 0;
#endif

    VK_CHECK_RESULT(vkCreateDevice(physicalDevice.device, &deviceCreateInfo, nullptr, &device), "failed to create logical device!");

    graphicsQueue = createQueue(indices.graphicsFamily.value(), 0);
    presentQueue = createQueue(indices.presentFamily.value(), 0);
}

void LogicalDevice::waitIdle()
{
    vkDeviceWaitIdle(device);
}

LogicalDevice::~LogicalDevice()
{
    vkDestroyDevice(device, nullptr);
}

std::unique_ptr<CommandPool> LogicalDevice::createCommandPool()
{
    return std::make_unique<CommandPool>(*this);
}

void LogicalDevice::destroyCommandPool(VkCommandPool pool)
{
    vkDestroyCommandPool(device, pool, nullptr);
}

std::unique_ptr<RenderPass> LogicalDevice::createRenderPass(std::vector<VkAttachmentDescription> attachments, VkSubpassDescription subpass)
{
    return std::make_unique<RenderPass>(*this, attachments, subpass);
}

void LogicalDevice::destroyRenderPass(VkRenderPass renderPass)
{
    vkDestroyRenderPass(device, renderPass, nullptr);
}

std::unique_ptr<SwapChain> LogicalDevice::createSwapChain()
{
    return std::make_unique<SwapChain>(*this);
}

void LogicalDevice::destroySwapChain(VkSwapchainKHR swapChain)
{
    vkDestroySwapchainKHR(device, swapChain, nullptr);
}

std::unique_ptr<DescriptorPool> LogicalDevice::createDescriptorPool(uint32_t uniformBufferCount, uint32_t imageSamplerCount, uint32_t descriptorSetCount)
{
    return std::make_unique<DescriptorPool>(*this, uniformBufferCount, imageSamplerCount, descriptorSetCount);
}

void LogicalDevice::destroyDescriptorPool(VkDescriptorPool pool)
{
    vkDestroyDescriptorPool(device, pool, nullptr);
}

std::unique_ptr<Sampler> LogicalDevice::createSampler()
{
    return std::make_unique<Sampler>(*this);
}

void LogicalDevice::destroySampler(VkSampler sampler)
{
    vkDestroySampler(device, sampler, nullptr);
}

std::unique_ptr<DescriptorSetLayout> LogicalDevice::createDescriptorSetLayout(std::vector<VkDescriptorSetLayoutBinding> bindings)
{
    return std::make_unique<DescriptorSetLayout>(*this, bindings);
}

void LogicalDevice::destroyDescriptorSetLayout(VkDescriptorSetLayout layout)
{
    vkDestroyDescriptorSetLayout(device, layout, nullptr);
}

std::unique_ptr<GraphicsPipeline> LogicalDevice::createGraphicsPipeline(PipelineLayout &layout, Shader &vertShader, Shader &fragShader, RenderPass &renderPass)
{
    return std::make_unique<GraphicsPipeline>(*this, layout, vertShader, fragShader, renderPass);
}

void LogicalDevice::destroyGraphicsPipeline(VkPipeline pipeline)
{
    vkDestroyPipeline(device, pipeline, nullptr);
}

std::unique_ptr<PipelineLayout> LogicalDevice::createPipelineLayout(std::vector<DescriptorSetLayout *> descriptorSetLayouts)
{
    return std::make_unique<PipelineLayout>(*this, descriptorSetLayouts);
}

void LogicalDevice::destroyPipelineLayout(VkPipelineLayout layout)
{
    vkDestroyPipelineLayout(device, layout, nullptr);
}

std::unique_ptr<Shader> LogicalDevice::createShader(std::filesystem::path const &path)
{
    return std::make_unique<Shader>(*this, path);
}

void LogicalDevice::destroyShader(VkShaderModule shaderModule)
{
    vkDestroyShaderModule(device, shaderModule, nullptr);
}

std::unique_ptr<Semaphore> LogicalDevice::createSemaphore()
{
    return std::make_unique<Semaphore>(*this);
}

void LogicalDevice::destroySemaphore(VkSemaphore semaphore)
{
    vkDestroySemaphore(device, semaphore, nullptr);
}

std::unique_ptr<Fence> LogicalDevice::createFence(VkFenceCreateFlags flags)
{
    return std::make_unique<Fence>(*this, flags);
}

void LogicalDevice::destroyFence(VkFence fence)
{
    vkDestroyFence(device, fence, nullptr);
}

std::unique_ptr<Queue> LogicalDevice::createQueue(uint32_t queueFamilyIndex, uint32_t queueIndex)
{
    return std::make_unique<Queue>(*this, queueFamilyIndex, queueIndex);
}