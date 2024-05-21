#include "renderer.h"
#include "engine.h"
#include "fs.h"
#include "gpu/shader.h"
#include "gpu/image_allocation.h"
#include "gpu/image_view.h"
#include "gpu/descriptor_pool.h"
#include "gpu/instance.h"
#include "gpu/validation_layers.h"
#include "gpu/utils.h"
#include <iostream>
#include <array>
#include <algorithm>
#include <set>

Renderer *renderer()
{
    return engine()->renderer.get();
}

LogicalDevice *device()
{
    return engine()->renderer->device.get();
}

uint32_t currentFrame()
{
    return engine()->renderer->currentFrame;
}

uint32_t currentImage()
{
    return engine()->renderer->currentImage;
}

Renderer::Renderer()
{
}

void Renderer::initialize()
{
    createInstance();
    createSurface();
    createPhysicalDevice();
    createLogicalDevice();
    createMemoryAllocator();
    createCommandPool();
    createSwapChain();
    createRenderPass();
    createDescriptorPool();
    createMatricesDescriptorSetLayout();
    createLightingDescriptorSetLayout();
    createMaterialDescriptorSetLayout();
    createGraphicsPipeline();
    createLightingDescriptorSets();
    createDepthResources();
#ifdef ENABLE_MULTISAMPLING
    createColorResources();
#endif
    createFramebuffers();
    createSyncObjects();
    createCommandBuffer();
}

Renderer::~Renderer()
{
}

void Renderer::dispose()
{
    cleanupSwapChain();

    lightingUniforms.reset();

    lightingDescriptorSet.reset();
    descriptorPool.reset();

    matricesDescriptorSetLayout.reset();
    materialDescriptorSetLayout.reset();
    lightingDescriptorSetLayout.reset();

    graphicsPipeline.reset();
    pipelineLayout.reset();

    renderPass.reset();

    renderFinishedSemaphores.clear();
    imageAvailableSemaphores.clear();
    inFlightFences.clear();

    commandBuffer.reset();
    commandPool.reset();

    vmaDestroyAllocator(allocator);

    device.reset();

    vkDestroySurfaceKHR(instance->instance, surface, nullptr);

    instance.reset();
}

void Renderer::createMatricesDescriptorSetLayout()
{
    VkDescriptorSetLayoutBinding uboLayoutBinding{
        .binding = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .pImmutableSamplers = nullptr,
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT};

    std::vector<VkDescriptorSetLayoutBinding> bindings = {uboLayoutBinding};

    matricesDescriptorSetLayout = device->createDescriptorSetLayout(bindings);
}

void Renderer::createMaterialDescriptorSetLayout()
{

    VkDescriptorSetLayoutBinding samplerLayoutBinding{
        .binding = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImmutableSamplers = nullptr,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};

    VkDescriptorSetLayoutBinding normalSamplerLayoutBinding{
        .binding = 1,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImmutableSamplers = nullptr,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};

    VkDescriptorSetLayoutBinding displacementSamplerLayoutBinding{
        .binding = 2,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImmutableSamplers = nullptr,
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT};

    VkDescriptorSetLayoutBinding occlusionSamplerLayoutBinding{
        .binding = 3,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImmutableSamplers = nullptr,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};

    std::vector<VkDescriptorSetLayoutBinding> bindings = {samplerLayoutBinding, normalSamplerLayoutBinding, displacementSamplerLayoutBinding, occlusionSamplerLayoutBinding};

    materialDescriptorSetLayout = device->createDescriptorSetLayout(bindings);
}

void Renderer::createLightingDescriptorSetLayout()
{
    VkDescriptorSetLayoutBinding uboLayoutBinding{
        .binding = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .pImmutableSamplers = nullptr,
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT};

    std::vector<VkDescriptorSetLayoutBinding> bindings = {uboLayoutBinding};

    lightingDescriptorSetLayout = device->createDescriptorSetLayout(bindings);
}

void Renderer::createRenderPass()
{
    VkAttachmentDescription colorAttachment{
        .format = swapChain->format,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR};

    VkAttachmentReference colorAttachmentRef{
        .attachment = 0,
        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};

    VkAttachmentDescription depthAttachment{
        .format = physicalDevice->findDepthFormat(),
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

    VkAttachmentReference depthAttachmentRef{
        .attachment = 1,
        .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

#ifdef ENABLE_MULTISAMPLING
    colorAttachment.samples = engine()->msaaSamples;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    depthAttachment.samples = engine()->msaaSamples;

    VkAttachmentDescription colorAttachmentResolve{
        .format = swapChain->format,
        .samples = engine()->msaaSamples,
        .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR};

    VkAttachmentReference colorAttachmentResolveRef{
        .attachment = 2,
        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};

    VkSubpassDescription subpass{
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &colorAttachmentRef,
        .pDepthStencilAttachment = &depthAttachmentRef,
        .pResolveAttachments = &colorAttachmentResolveRef};

    std::vector<VkAttachmentDescription> attachments = {colorAttachment, depthAttachment, colorAttachmentResolve};
#else
    VkSubpassDescription subpass{
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &colorAttachmentRef,
        .pDepthStencilAttachment = &depthAttachmentRef};

    std::vector<VkAttachmentDescription> attachments = {colorAttachment, depthAttachment};
#endif

    renderPass = device->createRenderPass(attachments, subpass);
}

void Renderer::createGraphicsPipeline()
{
    auto vertShader = device->createShader("shaders/test/test.vert.spv");
    auto fragShader = device->createShader("shaders/test/test.frag.spv");

    std::vector<DescriptorSetLayout *> descriptorSetLayouts{
        materialDescriptorSetLayout.get(),
        matricesDescriptorSetLayout.get(),
        lightingDescriptorSetLayout.get()};

    pipelineLayout = device->createPipelineLayout(descriptorSetLayouts);

    graphicsPipeline = device->createGraphicsPipeline(
        *pipelineLayout,
        *vertShader,
        *fragShader,
        *renderPass);
}

void Renderer::createDepthResources()
{
    VkFormat depthFormat = physicalDevice->findDepthFormat();

    depthImageAllocation = std::make_unique<ImageAllocation>(swapChain->extent.width, swapChain->extent.height, msaaSamples, depthFormat, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    depthImageView = std::make_unique<ImageView>(depthImageAllocation->image, depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT);

    depthImageAllocation->transitionImageLayout(depthFormat, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
}

void Renderer::cleanupSwapChain()
{
    std::cout << " Cleanup swap chain " << std::endl;

#ifdef ENABLE_MULTISAMPLING
    colorImageAllocation.reset();
    colorImageView.reset();
#endif

    depthImageAllocation.reset();
    depthImageView.reset();

    framebuffer.reset();

    swapChain.reset();
}

void Renderer::recreateSwapChain()
{
    cleanupSwapChain();
    createSwapChain();
#ifdef ENABLE_MULTISAMPLING
    createColorResources();
#endif
    createDepthResources();
    createFramebuffers();
}

void Renderer::createSwapChain()
{
    swapChain = device->createSwapChain();
}

void Renderer::draw(Scene &scene, Camera &camera)
{
    scene.updateWorldMatrix();

    if (camera.parent == nullptr)
    {
        camera.updateWorldMatrix();
    }

    renderList.clear();
    renderList.build(&scene, camera);

    commandBuffer->begin();
    commandBuffer->beginRenderPass(*renderPass, *framebuffer, *swapChain);
    commandBuffer->bindPipeline(*graphicsPipeline);

    commandBuffer->setViewport(vkViewport(
        0.0f, 0.0f,
        static_cast<float>(swapChain->extent.width),
        static_cast<float>(swapChain->extent.height),
        0.0f, 1.0f));

    commandBuffer->setScissor(vkRect2D(0, 0, swapChain->extent.width, swapChain->extent.height));

    updateLightingUniforms();

    commandBuffer->bindDescriptorSet(*pipelineLayout, 2, *lightingDescriptorSet);

    Model *lastModel = nullptr;
    Material *lastMaterial = nullptr;

    for (auto renderable : renderList.opaque)
    {
        auto model = renderable.model;
        auto mesh = renderable.mesh;
        auto material = model->materials[mesh->materialIndex];

        if (material.get() != lastMaterial)
        {
            commandBuffer->bindDescriptorSet(*pipelineLayout, 0, *material->descriptorSet);
            lastMaterial = material.get();
        }

        if (model != lastModel)
        {
            commandBuffer->bindDescriptorSet(*pipelineLayout, 1, *model->descriptorSet);
            lastModel = model;
        }

        mesh->draw(*commandBuffer);
    }

    commandBuffer->endRenderPass();
    commandBuffer->end();
}

void Renderer::createFramebuffers()
{
#ifdef ENABLE_MULTISAMPLING
    framebuffer = std::make_unique<Framebuffer>(swapChain.get(), renderPass.get(), depthImageView.get(), colorImageView.get());
#else
    framebuffer = std::make_unique<Framebuffer>(swapChain.get(), renderPass.get(), depthImageView.get(), nullptr);
#endif
}

void Renderer::createColorResources()
{
    colorImageAllocation = std::make_unique<ImageAllocation>(swapChain->extent.width, swapChain->extent.height, msaaSamples, swapChain->format, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    colorImageView = std::make_unique<ImageView>(colorImageAllocation->image, swapChain->format, VK_IMAGE_ASPECT_COLOR_BIT);
}

void Renderer::createDescriptorPool()
{
    descriptorPool = device->createDescriptorPool(MAX_FRAMES_IN_FLIGHT * 400, MAX_FRAMES_IN_FLIGHT * 400, MAX_FRAMES_IN_FLIGHT * 200);
}

void Renderer::updateLightingUniforms()
{
    lightingUniforms->ubo.pointLightCount = renderList.pointLights.size();

    for (size_t i = 0; i < std::min<size_t>(std::end(lightingUniforms->ubo.pointLights) - std::begin(lightingUniforms->ubo.pointLights), renderList.pointLights.size()); i++)
    {
        lightingUniforms->ubo.pointLights[i].color = renderList.pointLights[i]->color;
        lightingUniforms->ubo.pointLights[i].position = renderList.pointLights[i]->position;
        lightingUniforms->ubo.pointLights[i].intensity = renderList.pointLights[i]->intensity;
        lightingUniforms->ubo.pointLights[i].constant = renderList.pointLights[i]->constant;
        lightingUniforms->ubo.pointLights[i].linear = renderList.pointLights[i]->linear;
        lightingUniforms->ubo.pointLights[i].quadratic = renderList.pointLights[i]->quadratic;
    }

    lightingUniforms->updateUniformBuffer(currentFrame);
}

void Renderer::createLightingDescriptorSets()
{
    lightingUniforms = std::make_unique<LightingUniforms>();

    lightingDescriptorSet = descriptorPool->createDescriptorSet(*lightingDescriptorSetLayout);

    updateLightingDescriptorSets();
}

void Renderer::updateLightingDescriptorSets()
{
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        VkDescriptorBufferInfo bufferInfoLighting{};
        bufferInfoLighting.buffer = lightingUniforms->uniformBufferAllocations[i]->buffer;
        bufferInfoLighting.offset = 0;
        bufferInfoLighting.range = sizeof(LightingUniformBufferObject);

        std::array<VkWriteDescriptorSet, 1> descriptorWrites{};

        descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrites[0].dstSet = lightingDescriptorSet->sets[i];
        descriptorWrites[0].dstBinding = 0;
        descriptorWrites[0].dstArrayElement = 0;
        descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        descriptorWrites[0].descriptorCount = 1;
        descriptorWrites[0].pBufferInfo = &bufferInfoLighting;

        vkUpdateDescriptorSets(device->device, static_cast<uint32_t>(descriptorWrites.size()), descriptorWrites.data(), 0, nullptr);
    }
}

void Renderer::createSyncObjects()
{
    imageAvailableSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
    renderFinishedSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
    inFlightFences.resize(MAX_FRAMES_IN_FLIGHT);

    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        imageAvailableSemaphores[i] = device->createSemaphore();
        renderFinishedSemaphores[i] = device->createSemaphore();
        inFlightFences[i] = device->createFence(VK_FENCE_CREATE_SIGNALED_BIT);
    }
}

void Renderer::drawFrame()
{
    auto engine = Engine::current();

    inFlightFences[currentFrame]->wait();

    VkResult result = swapChain->acquireNextImage(*imageAvailableSemaphores[currentFrame], &currentImage);

    if (result == VK_ERROR_OUT_OF_DATE_KHR)
    {
        framebufferResized = false;
        recreateSwapChain();
    }
    else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
    {
        throw std::runtime_error("failed to acquire swap chain image!");
    }

    inFlightFences[currentFrame]->reset();

    commandBuffer->reset();

    draw(engine->scene, engine->camera);

    VkSemaphore waitSemaphores[] = {imageAvailableSemaphores[currentFrame]->semaphore};
    VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;

    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer->buffers[currentFrame];

    VkSemaphore signalSemaphores[] = {renderFinishedSemaphores[currentFrame]->semaphore};
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = signalSemaphores;

    device->graphicsQueue->submit(submitInfo, *inFlightFences[currentFrame]);

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;

    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = signalSemaphores;

    VkSwapchainKHR swapChains[] = {swapChain->chain};
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = swapChains;
    presentInfo.pImageIndices = &currentImage;

    presentInfo.pResults = nullptr; // Optional

    result = device->presentQueue->present(presentInfo);

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || framebufferResized)
    {
        framebufferResized = false;
        engine->window.spinUntilValidSize();

        device->waitIdle();
        recreateSwapChain();
    }
    else
    {
        VK_CHECK_RESULT(result, "failed to present swap chain image!");
    }

    currentFrame = (currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
}

void Renderer::createCommandBuffer()
{
    commandBuffer = commandPool->createCommandBuffer();
}

void Renderer::createLogicalDevice()
{
    device = physicalDevice->createLogicalDevice();
}

void Renderer::createCommandPool()
{
    commandPool = device->createCommandPool();
}

void Renderer::createInstance()
{
    VkApplicationInfo appInfo = {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "QUAG";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "VENG";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_3;

    instance = std::make_unique<Instance>(appInfo);
}

VkSurfaceFormatKHR Renderer::chooseSwapSurfaceFormat(
    const std::vector<VkSurfaceFormatKHR> &availableFormats)
{
    for (const auto &availableFormat : availableFormats)
    {
        if (availableFormat.format == VK_FORMAT_B8G8R8A8_SRGB &&
            availableFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        {
            return availableFormat;
        }
    }

    return availableFormats[0];
}

VkExtent2D Renderer::chooseSwapExtent(
    const VkSurfaceCapabilitiesKHR &capabilities)
{
    if (capabilities.currentExtent.width !=
        std::numeric_limits<uint32_t>::max())
    {
        return capabilities.currentExtent;
    }
    else
    {
        VkExtent2D actualExtent = engine()->window.getExtent();

        actualExtent.width =
            std::clamp(actualExtent.width, capabilities.minImageExtent.width,
                       capabilities.maxImageExtent.width);
        actualExtent.height =
            std::clamp(actualExtent.height, capabilities.minImageExtent.height,
                       capabilities.maxImageExtent.height);

        return actualExtent;
    }
}

VkPresentModeKHR Renderer::chooseSwapPresentMode(
    const std::vector<VkPresentModeKHR> &availablePresentModes)
{
    for (const auto &availablePresentMode : availablePresentModes)
    {
        if (availablePresentMode == VK_PRESENT_MODE_MAILBOX_KHR)
        {
            return availablePresentMode;
        }
    }

    return VK_PRESENT_MODE_FIFO_KHR;
}

void Renderer::createSurface()
{
    if (glfwCreateWindowSurface(instance->instance, engine()->window.window, nullptr, &surface) !=
        VK_SUCCESS)
    {
        throw std::runtime_error("failed to create window surface!");
    }
}

void Renderer::createMemoryAllocator()
{
    VmaAllocatorCreateInfo allocatorInfo = {};
    allocatorInfo.physicalDevice = physicalDevice->device;
    allocatorInfo.device = device->device;
    allocatorInfo.instance = instance->instance;
    allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_3;

    vmaCreateAllocator(&allocatorInfo, &allocator);
}

bool Renderer::hasStencilComponent(VkFormat format)
{
    return format == VK_FORMAT_D32_SFLOAT_S8_UINT || format == VK_FORMAT_D24_UNORM_S8_UINT;
}

void Renderer::createPhysicalDevice()
{
    physicalDevice = instance->createPhysicalDevice();
}
