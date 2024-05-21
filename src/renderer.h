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
#include "gpu/command_pool.h"
#include "gpu/instance.h"
#include "gpu/physical_device.h"
#include "gpu/logical_device.h"

const int MAX_FRAMES_IN_FLIGHT = 2;

class Renderer
{
    void buildRenderList(Object *object, RenderList &list, Camera &camera);

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
    std::unique_ptr<CommandBuffer> commandBuffer;

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

    uint32_t currentFrame = 0;
    uint32_t currentImage = 0;

    void drawFrame();
    void createCommandBuffer();
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
    std::unique_ptr<DescriptorSet> lightingDescriptorSet;

    void createRenderPass();
    void draw(Scene &scene, Camera &camera);

    VkCommandBuffer beginSingleTimeCommands();
    void endSingleTimeCommands(VkCommandBuffer commandBuffer);
    void createLogicalDevice();
    void createCommandPool();
    void createPhysicalDevice();
    void createInstance();
    std::unique_ptr<CommandPool> commandPool;

    VkPresentModeKHR chooseSwapPresentMode(
        const std::vector<VkPresentModeKHR> &availablePresentModes);

    void createSurface();
    VkSampleCountFlagBits getMaxUsableSampleCount();

    bool hasStencilComponent(VkFormat format);

    void createMemoryAllocator();
    std::unique_ptr<Instance> instance;

    std::unique_ptr<PhysicalDevice> physicalDevice;

    VkSurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR> &availableFormats);
    VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR &capabilities);

    VmaAllocator allocator;
    std::unique_ptr<LogicalDevice> device;
    VkSurfaceKHR surface;
    void waitDeviceIdle();

    VkSampleCountFlagBits msaaSamples = VK_SAMPLE_COUNT_1_BIT;
    void initialize();
    void dispose();
};

Renderer *renderer();
LogicalDevice *device();
uint32_t currentFrame();
uint32_t currentImage();

#endif