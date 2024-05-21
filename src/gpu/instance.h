#ifndef _INSTANCE_H_
#define _INSTANCE_H_

#include "../gfxcommon.h"
#include "physical_device.h"

class Instance
{
public:
    Instance(VkApplicationInfo appInfo);
    ~Instance();

    bool checkValidationLayerSupport();
    void populateDebugMessengerCreateInfo(VkDebugUtilsMessengerCreateInfoEXT &createInfo);
    std::vector<const char *> getRequiredExtensions();
    void setupDebugMessenger();
    std::unique_ptr<PhysicalDevice> createPhysicalDevice();
    bool isDeviceSuitable(VkPhysicalDevice device);

    VkInstanceCreateInfo instanceCreateInfo;
    VkInstance instance;
    VkDebugUtilsMessengerEXT debugMessenger;
};

#endif // _INSTANCE_H_