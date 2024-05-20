#ifndef _RENDERER_H_
#define _RENDERER_H_

#include "gfxcommon.h"
#include "scene.h"
#include "camera.h"
#include "render_list.h"
#include "lighting_uniforms.h"
#include "gpu/swap_chain.h"
#include "gpu/descriptor_pool.h"
#include "gpu/descriptor_set.h"
#include "gpu/descriptor_set_layout.h"
#include "gpu/pipeline_layout.h"
#include "gpu/graphics_pipeline.h"
#include "gpu/render_pass.h"
#include "gpu/framebuffer.h"

const int MAX_FRAMES_IN_FLIGHT = 2;

struct QueueFamilyIndices
{
    std::optional<uint32_t> graphicsFamily;
    std::optional<uint32_t> presentFamily;

    bool isComplete();
};

struct SwapChainSupportDetails
{
    VkSurfaceCapabilitiesKHR capabilities;
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> presentModes;
};

class Renderer
{
    void buildRenderList(Object *object, RenderList &list, Camera *camera);

public:
    Renderer();
    ~Renderer();

    RenderList renderList;
    std::unique_ptr<RenderPass> renderPass;
    std::unique_ptr<DescriptorSetLayout> matricesDescriptorSetLayout;
    std::unique_ptr<DescriptorSetLayout> materialDescriptorSetLayout;
    std::unique_ptr<SwapChain> swapChain;
    std::unique_ptr<PipelineLayout> pipelineLayout;
    std::unique_ptr<GraphicsPipeline> graphicsPipeline;

    std::unique_ptr<ImageAllocation> depthImageAllocation;
    std::unique_ptr<ImageView> depthImageView;

    std::unique_ptr<ImageAllocation> colorImageAllocation;
    std::unique_ptr<ImageView> colorImageView;

    std::unique_ptr<LightingUniforms> lightingUniforms;

    std::unique_ptr<Framebuffer> framebuffer;

    std::vector<VkSemaphore> imageAvailableSemaphores;
    std::vector<VkSemaphore> renderFinishedSemaphores;
    std::vector<VkFence> inFlightFences;

    void createDescriptorPool();
    std::unique_ptr<DescriptorPool> descriptorPool;

    bool framebufferResized = false;
    VkQueue presentQueue;
    VkQueue graphicsQueue;
    uint32_t currentFrame = 0;
    std::vector<VkCommandBuffer> commandBuffers;
    void drawFrame();
    void createCommandBuffers();
    void createSyncObjects();
    void cleanupSwapChain();
    void recreateSwapChain();
    void createDepthResources();
    void createGraphicsPipeline();
    void createFramebuffers();
    void createSwapChain();
    void createMatricesDescriptorSetLayout();
    void createMaterialDescriptorSetLayout();
    void createLightingDescriptorSetLayout();
    void createColorResources();
    void updateLightingUniforms();
    void createLightingDescriptorSets();
    void updateLightingDescriptorSets();
    std::unique_ptr<DescriptorSetLayout> lightingDescriptorSetLayout;
    DescriptorSet *lightingDescriptorSet;

    void createRenderPass();
    void draw(VkCommandBuffer commandBuffer, Scene *scene, Camera *camera, uint32_t imageIndex);
    void createQueues(uint32_t graphicsFamilyQueueIndex, uint32_t presentFamilyQueueIndex);
    VkCommandBuffer beginSingleTimeCommands();
    void endSingleTimeCommands(VkCommandBuffer commandBuffer);
    void createLogicalDevice();
    void createCommandPool();
    QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device);
    void createInstance();
    VkCommandPool commandPool;
    bool checkDeviceExtensionSupport(
        VkPhysicalDevice device);
    VkPresentModeKHR chooseSwapPresentMode(
        const std::vector<VkPresentModeKHR> &availablePresentModes);
    SwapChainSupportDetails querySwapChainSupport(VkPhysicalDevice device);
    void setupDebugMessenger();
    std::vector<const char *> getRequiredExtensions();
    bool checkValidationLayerSupport();
    void populateDebugMessengerCreateInfo(VkDebugUtilsMessengerCreateInfoEXT &createInfo);
    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);
    void createSurface();
    VkSampleCountFlagBits getMaxUsableSampleCount();
    void pickPhysicalDevice();

    bool isDeviceSuitable(VkPhysicalDevice device);
    bool hasStencilComponent(VkFormat format);
    VkFormat findDepthFormat();
    VkFormat findSupportedFormat(const std::vector<VkFormat> &candidates, VkImageTiling tiling, VkFormatFeatureFlags features);
    void createMemoryAllocator();
    VkInstance instance;
    VkDebugUtilsMessengerEXT debugMessenger;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;

    VkSurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR> &availableFormats);
    VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR &capabilities);

    VmaAllocator allocator;
    VkDevice device;
    VkSurfaceKHR surface;
    void waitDeviceIdle();

    VkSampleCountFlagBits msaaSamples = VK_SAMPLE_COUNT_1_BIT;
    void initialize();
    void dispose();
};

Renderer *renderer();

#endif