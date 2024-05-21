
#ifndef _GPU_SWAP_CHAIN_H_
#define _GPU_SWAP_CHAIN_H_

#include "../gfxcommon.h"
#include "image_allocation.h"
#include "image_view.h"
#include "logical_device.h"
class SwapChain
{
public:
    SwapChain(LogicalDevice &device);
    ~SwapChain();

    VkResult acquireNextImage(VkSemaphore semaphore, uint32_t *imageIndex);

    std::vector<VkImage> images{};
    std::vector<std::unique_ptr<ImageView>> imageViews{};

    VkSwapchainKHR chain;
    VkSwapchainCreateInfoKHR swapChainCreateInfo{};
    VkSurfaceFormatKHR surfaceFormat;
    VkPresentModeKHR presentMode;
    VkExtent2D extent;
    std::vector<VkFramebuffer> swapChainFramebuffers;

    VkFormat format;

    uint32_t imageCount;

    LogicalDevice &device;
};

#endif