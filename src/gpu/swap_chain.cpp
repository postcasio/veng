#include "swap_chain.h"
#include "../gfxcommon.h"
#include "../engine.h"

SwapChain::SwapChain(LogicalDevice &device) : device(device)
{
    SwapChainSupportDetails swapChainSupport =
        device.physicalDevice.querySwapChainSupport();

    surfaceFormat =
        renderer()->chooseSwapSurfaceFormat(swapChainSupport.formats);
    presentMode =
        renderer()->chooseSwapPresentMode(swapChainSupport.presentModes);
    extent = renderer()->chooseSwapExtent(swapChainSupport.capabilities);

    imageCount = swapChainSupport.capabilities.minImageCount + 1;

    if (swapChainSupport.capabilities.maxImageCount > 0 &&
        imageCount > swapChainSupport.capabilities.maxImageCount)
    {
        imageCount = swapChainSupport.capabilities.maxImageCount;
    }

    swapChainCreateInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapChainCreateInfo.surface = renderer()->surface;

    swapChainCreateInfo.minImageCount = imageCount;
    swapChainCreateInfo.imageFormat = surfaceFormat.format;
    swapChainCreateInfo.imageColorSpace = surfaceFormat.colorSpace;
    swapChainCreateInfo.imageExtent = extent;
    swapChainCreateInfo.imageArrayLayers = 1;
    swapChainCreateInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    QueueFamilyIndices indices = device.physicalDevice.findQueueFamilies();
    uint32_t queueFamilyIndices[] = {indices.graphicsFamily.value(),
                                     indices.presentFamily.value()};

    if (indices.graphicsFamily != indices.presentFamily)
    {
        swapChainCreateInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        swapChainCreateInfo.queueFamilyIndexCount = 2;
        swapChainCreateInfo.pQueueFamilyIndices = queueFamilyIndices;
    }
    else
    {
        swapChainCreateInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        swapChainCreateInfo.queueFamilyIndexCount = 0;     // Optional
        swapChainCreateInfo.pQueueFamilyIndices = nullptr; // Optional
    }

    swapChainCreateInfo.preTransform = swapChainSupport.capabilities.currentTransform;

    swapChainCreateInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;

    swapChainCreateInfo.presentMode = presentMode;
    swapChainCreateInfo.clipped = VK_TRUE;

    swapChainCreateInfo.oldSwapchain = VK_NULL_HANDLE;

    VK_CHECK_RESULT(vkCreateSwapchainKHR(device.device, &swapChainCreateInfo, nullptr, &chain), "failed to create swap chain!");

    vkGetSwapchainImagesKHR(device.device, chain, &imageCount, nullptr);
    images.resize(imageCount);
    vkGetSwapchainImagesKHR(device.device, chain, &imageCount, images.data());

    for (auto &image : images)
    {
        imageViews.push_back(std::make_unique<ImageView>(image, surfaceFormat.format, VK_IMAGE_ASPECT_COLOR_BIT));
    }

    format = surfaceFormat.format;
}

SwapChain::~SwapChain()
{
    for (auto &imageView : imageViews)
    {
        imageView.reset();
    }

    device.destroySwapChain(chain);
}

VkResult SwapChain::acquireNextImage(Semaphore &semaphore, uint32_t *imageIndex)
{
    return vkAcquireNextImageKHR(device.device, chain, UINT64_MAX, semaphore.semaphore, VK_NULL_HANDLE, imageIndex);
}