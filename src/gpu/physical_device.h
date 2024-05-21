#ifndef _PHYSICAL_DEVICE_H_
#define _PHYSICAL_DEVICE_H_

class LogicalDevice;

#include "../gfxcommon.h"
#include "logical_device.h"
#include <optional>

const std::vector<const char *> *getDeviceExtensions();

struct SwapChainSupportDetails
{
    VkSurfaceCapabilitiesKHR capabilities;
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> presentModes;
};

struct QueueFamilyIndices
{
    std::optional<uint32_t> graphicsFamily;
    std::optional<uint32_t> presentFamily;

    bool isComplete();
};

class PhysicalDevice
{
public:
    PhysicalDevice(VkPhysicalDevice device);
    ~PhysicalDevice();

    bool isSuitable();
    QueueFamilyIndices findQueueFamilies();
    bool checkDeviceExtensionSupport();
    SwapChainSupportDetails querySwapChainSupport();
    VkSampleCountFlagBits getMaxUsableSampleCount();
    std::unique_ptr<LogicalDevice> createLogicalDevice();
    VkFormat findSupportedFormat(const std::vector<VkFormat> &candidates, VkImageTiling tiling, VkFormatFeatureFlags features);
    VkFormat findDepthFormat();
    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);

    VkPhysicalDevice device;
};

#endif // _PHYSICAL_DEVICE_H_
